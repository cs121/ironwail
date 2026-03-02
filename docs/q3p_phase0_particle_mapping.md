# Phase 0: Netzwerk/QC Entry-Points und `TE_*`-Mapping

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
