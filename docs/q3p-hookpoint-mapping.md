# q3p-Integrationsanalyse (ohne Breaking Changes)

Diese Notiz bildet den aktuellen Partikel-Callgraph ab und markiert konkrete Hookpoints, an denen ein q3p-Backend schrittweise eingehängt werden kann, ohne den klassischen/glquake-Pfad zu brechen.

## 1) Callgraph heute (Init → Spawn/API → Sim → Draw)

```text
R_Init (gl_rmisc.c)
  -> R_InitParticles (r_part.c)
       - liest -particles
       - allokiert particles[] via Hunk_AllocName
       - registriert r_particles + Callback

Server-/Effekt-Einspeisung
  -> R_ParseParticleEffect (r_part.c)
       -> R_RunParticleEffect (r_part.c)
            -> R_AllocParticle (r_part.c)
  -> weitere Spawner in r_part.c
       (R_ParticleExplosion, R_ParticleExplosion2, R_BlobExplosion,
        R_LavaSplash, R_TeleportSplash, R_RocketTrail, R_EntityParticles)

Frame-Update (Host-Loop)
  -> CL_RunParticles (host.c -> r_part.c)
       - integriert Bewegung/Lebensdauer
       - kompaktifiziert aktive Partikel in-place

Render-Frame
  -> R_RenderView (gl_rmain.c)
     -> R_RenderScene (gl_rmain.c)
         -> R_DrawParticles(false)   // opaquer/square pass
         -> ... Translucency-Block ...
         -> R_DrawParticles(true)    // alpha pass
  -> optional Debug: R_ShowTris -> R_DrawParticles_ShowTris
```

## 2) Integrationspunkte (q3p) + Fallback-Hotspots

Ziel: eine austauschbare q3p-Implementierung hinter bestehenden API-Einstiegen, ohne neue CVars in diesem Task.

### Hook A: Initialisierung / Lifetime

- **Heute:** `R_InitParticles` allokiert/initialisiert klassischen Pool.
- **q3p-Hook:** In `R_InitParticles` nach CVar-Setup optionalen q3p-Init-Aufruf ergänzen (z. B. `Q3P_Init()`), der nur interne q3p-Ressourcen vorbereitet.
- **Fallback:** Wenn q3p-Init fehlschlägt oder deaktiviert ist, bleibt der bestehende Pool (`particles`, `r_numparticles`) alleiniger Pfad.

### Hook B: Spawn/API-Eingänge

- **Heute:** Alle Spawnpfade landen effektiv bei `R_AllocParticle`/Direktbefüllung von `particle_t`.
- **q3p-Hook:** Zentralen Adapter pro Spawnfamilie vorsehen (mindestens in `R_RunParticleEffect`, optional in weiteren Spawnern), der q3p-Events enqueued, **aber** klassischen Spawn parallel/intakt lässt.
- **Fallback:** Adapter darf bei Nichterfolg/Nichtverfügbarkeit still auf klassischen `particle_t`-Spawn zurückfallen.

### Hook C: Simulation

- **Heute:** `CL_RunParticles` ist die zentrale CPU-Simulation für klassische Partikel.
- **q3p-Hook:** Früher Einstieg in `CL_RunParticles` (oder davor im Host-Frame) für `Q3P_Simulate(dt)`, getrennt vom klassischen Loop.
- **Fallback:** Klassischer Simulationsblock bleibt unverändert ausführbar; q3p-Fehler dürfen diesen nicht abbrechen.

### Hook D: Draw-Pipeline / Pass-Trennung

- **Heute:** `R_RenderScene` ruft `R_DrawParticles(false)` vor Translucency und `R_DrawParticles(true)` im transparenten Pass auf.
- **q3p-Hook:** In `R_DrawParticles`/`R_DrawParticles_Real` pro Pass einen q3p-Draw-Aufruf ergänzen, der dieselbe Pass-Semantik (opaque vs. alpha) respektiert.
- **Fallback:** Falls q3p in einem Pass nichts liefert oder fehlschlägt, zeichnet der klassische Pfad weiterhin über bestehende Batch-Logik (`R_FlushParticleBatch`).

### Hook E: Map-/Reset-Lebenszyklus

- **Heute:** `R_NewMap` ruft `R_ClearParticles`; dies setzt den klassischen Zustand zurück.
- **q3p-Hook:** Parallel `Q3P_Clear()` an denselben Lebenszyklus hängen.
- **Fallback:** Klassisches `R_ClearParticles` bleibt maßgeblich; q3p-Clear-Fehler dürfen keinen Mapwechsel blockieren.

### Hook F: Öffentliche Signaturen

- **Heute:** Öffentliche Partikel-API ist in `render.h`/`glquake.h` bereits stabil (`R_RunParticleEffect`, `R_DrawParticles`, `CL_RunParticles`, ...).
- **q3p-Hook:** Keine API-Breaks; q3p hinter bestehender Signatur verstecken. Falls nötig, nur interne/`static` Helfer hinzufügen.
- **Fallback:** Externe Caller bleiben unverändert; Build- und Laufzeitverhalten ohne q3p bleibt identisch.

## 3) Mapping-Tabelle „heute vs. Ziel-q3p-Hookpoints“

| Bereich | Heute (Codepfad) | Ziel-q3p-Hookpoint | Fallback (classic/glquake intakt) |
|---|---|---|---|
| Init | `R_Init` → `R_InitParticles` | `Q3P_Init()` am Ende von `R_InitParticles` | Bei Fehler nur klassischer Init aktiv |
| Spawn/API | `R_ParseParticleEffect` → `R_RunParticleEffect` (+ weitere Spawner) | q3p-Adapter an zentralen Spawn-Eingängen | Bei Fehler/NOP weiter via `R_AllocParticle` |
| Sim | `CL_RunParticles` (Host-Frame) | `Q3P_Simulate(frametime)` vor/parallel klassischem Loop | Klassischer Simulationsloop immer lauffähig |
| Draw opaque | `R_DrawParticles(false)` in `R_RenderScene` | `Q3P_Draw(false)` im selben Pass | Klassische Batch-Zeichnung bleibt |
| Draw alpha | `R_DrawParticles(true)` in Translucency | `Q3P_Draw(true)` im selben Pass | Klassische Batch-Zeichnung bleibt |
| Reset/NewMap | `R_NewMap` → `R_ClearParticles` | `Q3P_Clear()` im selben Resetfenster | Klassisches Clear bleibt allein ausreichend |
| Public API | Deklarationen in `render.h`/`glquake.h` | Keine externen Signaturänderungen | Vorhandene Caller unverändert |

## 4) Reihenfolge für risikoarme Umsetzung

1. **Nur Init/Clear-Hooks** einführen (ohne Spawn/Draw-Verhalten zu ändern).
2. **Spawn-Adapter** an `R_RunParticleEffect` anbinden (weiterhin dualer klassischer Spawn).
3. **Sim-Hook** ergänzen, klassische Sim unverändert bestehen lassen.
4. **Draw-Hooks pro Pass** ergänzen und Pass-Parität validieren.
5. Optional weitere Spawner auf q3p-Events erweitern, solange der klassische Pfad vollständig erhalten bleibt.

So bleibt in jeder Zwischenstufe ein funktionaler classic/glquake-Pfad verfügbar.
