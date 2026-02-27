# Render Backend Readiness Matrix (QA)

## Ziel
Dieses Dokument definiert eine reproduzierbare QA-Matrix für den Vergleich von Legacy-Renderpfad (`r_backend 0`) und Backend-Renderpfad (`r_backend 1`) über 7 Szenentypen.

**Done-Kriterium:** Ein Entwickler kann jeden Testfall ohne Interpretationsspielraum ausführen, Hashes vergleichen und ein eindeutiges Pass/Fail ableiten.

---

## Globale Testvoraussetzungen

1. Identische Build-Version, identische GPU-Treiber-Version und identische Videoeinstellungen (`vid_*`, Auflösung, VSync, MSAA, Anisotropie).
2. Startparameter für reproduzierbare Läufe:
   - `+host_maxfps 0`
   - `+r_vsync 0`
   - falls vorhanden: fixer Demo-/Kamera-Pfad pro Szene.
3. Vor jedem Szenenlauf:
   - Neues Engine-Starten (kalter Start) **oder** mindestens `map <szene>` nach `disconnect`.
   - Konsole leeren/log neu beginnen.
4. Framehash-Logik:
   - Referenzlauf immer mit `r_backend 0`.
   - Vergleichslauf immer mit `r_backend 1`.
5. Pro Testlauf ist `r_gl_state_validate 1` verpflichtend.

---

## Standard-CVar-Block (für alle Szenen)

Diese CVars werden pro Lauf explizit gesetzt, damit nichts vom Nutzerprofil „durchblutet“:

```cfg
r_backend <0|1>
r_backend_ui <0|1>
r_backend_postfx <0|1>
r_backend_particles <0|1>
r_backend_alias <0|1>
r_backend_world <0|1>
r_backend_fogvol <0|1>
r_backend_framehash_debug 1
r_backend_framehash_epsilon <0|1|2>
r_gl_state_validate 1
```

Hinweis: `r_backend_framehash_scene` wird pro Szene explizit auf die unten definierte Szenen-ID gesetzt.

---

## Szenenmatrix (7 Szenen)

> Konvention: Subtoggles stehen standardmäßig auf `1`, außer in der jeweiligen Szene explizit anders angegeben.

| ID | Szene | Zielabdeckung | r_backend 0 (Referenz) | r_backend 1 (Vergleich) | r_backend_* Subtoggles | framehash_debug | framehash_epsilon | r_gl_state_validate |
|---|---|---|---|---|---|---|---|---|
| 1 | Startmap | Basis-World + UI-HUD | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `0` | `1` |
| 2 | Open area | Große Sichtweite, viele World-Batches | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `0` | `1` |
| 3 | Partikel-lastig | Opaque/Alpha-Particles, Blend-Reihenfolge | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `1` (nur Partikelrauschen) | `1` |
| 4 | Wasser/Warp/Teleport | Refraktion/Warp/Teleport-Visuals | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `1` (Warp-Rundung) | `1` |
| 5 | Viewmodel + Dlights | Weapon-Viewmodel, dynamische Lichter | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `0` | `1` |
| 6 | UI/Console Overlay | HUD, Menüs, Konsole/Overdraw-Reihenfolge | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `0` | `1` |
| 7 | FogVol/Godray/SSAO | FogVol-Backend + PostFX-Pfad | `r_backend 0` | `r_backend 1` | `ui=1, postfx=1, particles=1, alias=1, world=1, fogvol=1` | `1` | `2` (PostFX/FogVol Toleranz) | `1` |

### Szenen-spezifische Kommandoblöcke

Für eindeutige Reproduktion pro Szene immer folgende Sequenz verwenden (Beispiel Szene 3):

```cfg
r_backend_framehash_scene 3
r_backend 0
r_backend_ui 1
r_backend_postfx 1
r_backend_particles 1
r_backend_alias 1
r_backend_world 1
r_backend_fogvol 1
r_backend_framehash_debug 1
r_backend_framehash_epsilon 1
r_gl_state_validate 1
map <partikel_map>
```

Danach identischer Block mit nur einer Änderung:

```cfg
r_backend 1
```

---

## Pass/Fail-Kriterien

Ein Testfall ist nur **PASS**, wenn **alle** Bedingungen erfüllt sind:

1. **Framehash-Kriterium**
   - Primär: Hashes von `r_backend 0` und `r_backend 1` sind gleich.
   - Ausnahmefall: Hash-Differenz ist zulässig, wenn die Szene einen `r_backend_framehash_epsilon > 0` vorgibt **und** die Abweichung innerhalb dieser Epsilon-Grenze liegt.
   - Jede Epsilon-Ausnahme muss im QA-Log begründet werden (z. B. „Partikel-Noise in Szene 3“).

2. **Stabilitäts-/Leak-Kriterium**
   - Keine Leak-Warnings im Log (insbesondere Render-/GPU-Ressourcen-Leaks).
   - Keine zusätzlichen GL-Validate-Fehler bei `r_gl_state_validate 1`.

3. **Reproduzierbarkeit**
   - Ein zweiter Lauf mit identischer Konfiguration führt zum gleichen Urteil (PASS oder FAIL).

Ein Testfall ist **FAIL**, sobald eine der obigen Bedingungen verletzt ist.

---

## QA-Protokollvorlage (pro Szene)

| Feld | Wert |
|---|---|
| Szene-ID / Name | |
| Map / Startpunkt | |
| Lauf A (`r_backend 0`) Hash | |
| Lauf B (`r_backend 1`) Hash | |
| Epsilon gesetzt | |
| Epsilon-Begründung (falls > 0) | |
| Leak-Warnings gefunden | Ja/Nein |
| GL-Validate-Fehler gefunden | Ja/Nein |
| Ergebnis | PASS / FAIL |
| Tester + Datum | |

---

## Abschlussregel für Readiness

`Render Backend Ready` gilt erst dann, wenn:

- alle 7 Szenen mindestens einmal mit PASS abgeschlossen sind,
- kein FAIL offen ist,
- alle Epsilon-Ausnahmen dokumentiert und technisch begründet sind,
- keine Leak-Warnings in irgendeiner Szene aufgetreten sind.
