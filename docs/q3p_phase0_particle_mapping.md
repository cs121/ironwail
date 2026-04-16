# Phase 0: Netzwerk/QC Entry-Points und `TE_*`-Mapping

Current mapping snapshot for the legacy-to-q3p particle bridge.

Dieses Dokument erfasst den **Ist-Zustand** der relevanten Entry-Points und definiert für Phase 0 eine reine Mapping-/Fallback-Spezifikation ohne Laufzeitänderung.

## 1) Entry-Points aus Netzwerk/QC

| Quelle | Parse-/Dispatch-Pfad | Aktuelle Wirkung (Ist) | q3p-Äquivalent (Ziel) | Fallback-Regel (Phase 0) |
|---|---|---|---|---|
| `svc_particle` | `CL_ParseServerMessage` → `R_ParseParticleEffect` | Liest `org/dir/msgcount/color` aus der Netzmessage und ruft `R_RunParticleEffect` auf. | `q3p_particle_effect` (geplant) | **Wenn q3p verfügbar:** q3p-Pfad. **Sonst:** unverändert `R_RunParticleEffect` (classic/glquake). |
| `svc_temp_entity` | `CL_ParseServerMessage` → `CL_ParseTEnt` | Decodiert `TE_*`-Typ und führt je Typ den klassischen Effektpfad aus (Partikel-/Explosion-/Splash-/Beam-Funktionen). | `q3p_temp_entity_dispatch` (geplant) | **Wenn q3p verfügbar:** q3p-Dispatch. **Sonst:** unverändert `CL_ParseTEnt`-Verhalten (classic/glquake). |
| QC-Builtin `particle()` | `PF_particle` → `SV_StartParticle` → Netzmessage `svc_particle` → Client `R_ParseParticleEffect` | QC löst serverseitig `svc_particle` aus; Client landet im gleichen Effektpfad wie Netzwerk-`svc_particle`. | `q3p_particle_effect` (über denselben `svc_particle`-Pfad) | **Wenn q3p verfügbar:** q3p im Client-Effektpfad. **Sonst:** unverändert klassischer `svc_particle`-Pfad. |

## 2) TempEntity-Mapping (`TE_*` aus `Quake/protocol.h`)

> Ziel: Für jeden `TE_*` genau **ein** Mapping-Eintrag. Renderer-Spalte beschreibt die aktuell aufgerufenen Funktionen in `CL_ParseTEnt`.

| `TE_*` | Aktueller Dispatch in `CL_ParseTEnt` | Renderer-/Effektfunktion(en) aktuell | q3p-Äquivalent (Ziel) | Fallback-Regel (Phase 0) |
|---|---|---|---|---|
| `TE_SPIKE` | liest `pos` | `R_RunParticleEffect(pos, vec3_origin, 0, 10)` (+ Decal/Sound) | `q3p_te_spike` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_SUPERSPIKE` | liest `pos` | `R_RunParticleEffect(pos, vec3_origin, 0, 20)` (+ Decal/Sound) | `q3p_te_superspike` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_GUNSHOT` | liest `pos` | `R_RunParticleEffect(pos, vec3_origin, 0, 20)` (+ Decal) | `q3p_te_gunshot` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_EXPLOSION` | liest `pos` | `R_ParticleExplosion(pos)` (+ DLight/Sound/Decal) | `q3p_te_explosion` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_TAREXPLOSION` | liest `pos` | `R_BlobExplosion(pos)` (+ Sound/Decal) | `q3p_te_tarexplosion` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_LIGHTNING1` | Beam-Event | `CL_ParseBeam(Mod_ForName("progs/bolt.mdl", true))` | `q3p_te_lightning1` | q3p falls verfügbar, sonst klassischer Beam-Pfad unverändert. |
| `TE_LIGHTNING2` | Beam-Event | `CL_ParseBeam(Mod_ForName("progs/bolt2.mdl", true))` | `q3p_te_lightning2` | q3p falls verfügbar, sonst klassischer Beam-Pfad unverändert. |
| `TE_WIZSPIKE` | liest `pos` | `R_RunParticleEffect(pos, vec3_origin, 20, 30)` (+ Sound) | `q3p_te_wizspike` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_KNIGHTSPIKE` | liest `pos` | `R_RunParticleEffect(pos, vec3_origin, 226, 20)` (+ Sound) | `q3p_te_knightspike` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_LIGHTNING3` | Beam-Event | `CL_ParseBeam(Mod_ForName("progs/bolt3.mdl", true))` | `q3p_te_lightning3` | q3p falls verfügbar, sonst klassischer Beam-Pfad unverändert. |
| `TE_LAVASPLASH` | liest `pos` | `R_LavaSplash(pos)` | `q3p_te_lavasplash` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_TELEPORT` | liest `pos` | `R_TeleportSplash(pos)` | `q3p_te_teleport` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_EXPLOSION2` | liest `pos,colorStart,colorLength` | `R_ParticleExplosion2(pos, colorStart, colorLength)` (+ DLight/Sound/Decal) | `q3p_te_explosion2` | q3p falls verfügbar, sonst klassisch unverändert. |
| `TE_BEAM` | Beam-Event | `CL_ParseBeam(Mod_ForName("progs/beam.mdl", true))` | `q3p_te_beam` | q3p falls verfügbar, sonst klassischer Beam-Pfad unverändert. |

## 3) Globale Zielregel „q3p-Äquivalent“

Für **alle** oben dokumentierten Pfade gilt einheitlich:

1. `q3p` vorhanden → q3p-Äquivalent ausführen.
2. sonst `classic/glquake` unverändert.

Damit bleibt Phase 0 rein dokumentarisch/spezifikativ; es gibt keine Verhaltensänderung zur Laufzeit.

## 4) Akzeptanzkriterien / Review-Checks

- [x] Für jeden `TE_*` aus `Quake/protocol.h` gibt es genau einen Mapping-Eintrag in der Tabelle.
- [x] Für jeden bestehenden Aufrufpfad (`svc_particle`, `svc_temp_entity`, QC `particle()`) ist eine explizite Fallback-Entscheidung dokumentiert.
- [x] Review-Check: **keine** Änderung am Laufzeitverhalten in Phase 0 (nur Dokumentation).

## 5) Implementierungsstatus `R_ParseParticleEffect` (legacy `(org,dir,color,count)` → q3p)

- `R_ParseParticleEffect` liest weiterhin unverändert `org/dir/msgcount/color` aus `svc_particle`.
- Ist `msgcount == 255`, bleibt die Legacy-Semantik `count = 1024` (Rocket-Explosion).
- Für `r_particles_mode = q3p|hybrid` wird nun ein q3p-Preset-Mapping ausgeführt:
  - `count == 1024` → Material `explosion` (Explosion-Preset).
  - sonst → Material `bullet` (klassischer `R_RunParticleEffect`-Sprühpfad).
- Für andere Modi (`classic|glquake`) bleibt der alte Pfad vollständig erhalten (`R_RunParticleEffect`).

### Indirekter QC-`particle()`-Pfad (`sv_main.c`/`pr_cmds.c` → `svc_particle` → Client)

Der Laufweg ist unverändert und weiter konsistent:

1. QC `particle(origin, dir, color, count)` ruft `PF_particle` auf.
2. `PF_particle` ruft `SV_StartParticle(org, dir, color, count)`.
3. `SV_StartParticle` serialisiert ein `svc_particle`-Event (`org`, quantisiertes `dir`, `count`, `color`).
4. Client empfängt `svc_particle` und landet in `R_ParseParticleEffect`, das jetzt abhängig vom Partikelmodus q3p-Presets oder Legacy rendert.

### Farb-/Count-Semantik und bekannte Abweichungen

- **Beibehalten:**
  - Netz-/QC-Werte `color` und `count` werden weiterhin aus demselben `svc_particle`-Payload gelesen.
  - Sonderfall `count == 255` auf dem Netzkanal bleibt `1024` (Explosion).
  - Bei deaktiviertem q3p ist das Verhalten byte-identisch zum alten Pfad.
- **Abweichungen im q3p-Pfad (dokumentiert):**
  - Legacy-Rand-Color-Jitter `(color&~7)+(rand&7)` wird nicht 1:1 repliziert; q3p übernimmt den Basisfarbwert als `p.color`.
  - Legacy-Partikeltypen (`pt_slowgrav`, `pt_explode`, `pt_explode2`) werden durch q3p-Material-Presets (`bullet`/`explosion`) angenähert.
  - Bei erschöpftem q3p-Partikelpool wird der Spawn früh beendet (best-effort) statt auf Legacy zurückzufallen.


## 6) Legacy-Mapping-Keys für den gemeinsamen Descriptor-Layer

Die Legacy-Entry-Points verwenden jetzt gemeinsame Descriptor-Familien mit denselben Schlüsseln in Code und Design. Jeder Schlüssel trägt Material, Lifetime-Range, Size/Size-Ramp, Alpha-Ramp, Gravity/Drag sowie Jitter-/Collision-Policy.

| Mapping-Key | Entry-Point | Legacy-Fall | Default-Material |
|---|---|---|---|
| `svc_particle.explosion` | `R_TrySpawnQ3PLegacyParticleEffect` | `count == 1024` (Rocket-Explosion) | `explosion` |
| `svc_particle.spray` | `R_TrySpawnQ3PLegacyParticleEffect` | allgemeiner `(org,dir,color,count)`-Sprühpfad | `bullet` |
| `trail.rocket` | `R_RocketTrail` | `type 0` Rocket Trail | `smoke` |
| `trail.smoke` | `R_RocketTrail` | `type 1` Smoke Trail | `smoke` |
| `trail.blood_heavy` | `R_RocketTrail` | `type 2` Blood Trail | `blood_heavy` |
| `trail.blood_light` | `R_RocketTrail` | `type 4` Light Blood Trail | `blood_light` |
| `trail.voor` | `R_RocketTrail` | `type 6` Vore Trail | `voor` |

### Merge-/Override-Regel

- Basiswerte kommen aus dem Legacy-Descriptor (familienbasiert, shared layer).
- `Q3P_GetEffectDef(material)` bleibt die einzige Override-Quelle.
- Wenn ein `.prt`-Override vorhanden ist, überschreibt es den Descriptor vollständig über den gemeinsamen Merge-Helper.
- Ohne Override bleibt das Legacy-Fallback mit denselben Mapping-Keys und Policies aktiv.
