# Temp-Entity Event Matrix (`TE_*`)

Quellenbasis: `Quake/protocol.h`, `Quake/cl_tent.c` (`CL_ParseTEnt`, `CL_ParseBeam`) und `Quake/cl_parse.c` (Dispatch von `svc_temp_entity` auf `CL_ParseTEnt`).

## Dispatch / Parsing-Pipeline

- Netzwerkmessage `svc_temp_entity` wird in `CL_ParseServerMessage` direkt an `CL_ParseTEnt()` weitergereicht.
- `CL_ParseTEnt()` liest zuerst den Event-Typ (`type = MSG_ReadByte()`) und verarbeitet dann event-spezifische Payload.
- Für `TE_LIGHTNING1/2/3` und `TE_BEAM` ruft `CL_ParseTEnt()` die Hilfsroutine `CL_ParseBeam(model)` auf.

## Event-Matrix

| TE-Typ | ID (`protocol.h`) | Eingabeparameter aus Netzmessage | Effektroutinen / Assets | q3p-geeignet | Legacy behalten |
|---|---:|---|---|---|---|
| `TE_SPIKE` | 0 | `pos` (`coord3`) | `R_RunParticleEffect(pos, vec3_origin, 0, 10)`, `R_SpawnImpactDecal("bullet", ...)` (wenn Normal verfügbar), Sound: `tink1` oder `ric1/2/3` | Ja (Impact-Hit mit Position + Material-Sound/Decal) | Ja (originale Sound-Randomisierung) |
| `TE_SUPERSPIKE` | 1 | `pos` (`coord3`) | `R_RunParticleEffect(..., color=0, count=20)`, Bullet-Decal, Sound: `tink1` oder `ric1/2/3` | Ja | Ja |
| `TE_GUNSHOT` | 2 | `pos` (`coord3`) | `R_RunParticleEffect(..., color=0, count=20)`, Bullet-Decal | Ja | Ja |
| `TE_EXPLOSION` | 3 | `pos` (`coord3`) | `V_AddExplosionVibration`, `R_ParticleExplosion`, Scorch-Decal, Dynamic Light (`radius=350`, `die=+0.5`, `decay=300`, `DLIGHT_EXPLOSION`), Sound `r_exp3` | Ja (direkt mapbar auf Explosion-FX inkl. Light/Sound/Decal) | Ja |
| `TE_TAREXPLOSION` | 4 | `pos` (`coord3`) | `V_AddExplosionVibration`, `R_BlobExplosion`, Scorch-Decal, Sound `r_exp3` | Eher ja (eigener Blob-Explosionstyp) | Ja |
| `TE_LIGHTNING1` | 5 | via `CL_ParseBeam`: `ent` (`short`), `start` (`coord3`), `end` (`coord3`) | `CL_ParseBeam(Mod_ForName("progs/bolt.mdl"))` | Ja (Beam-Segment aus Start/End + Emitter-Entity) | Ja |
| `TE_LIGHTNING2` | 6 | `ent` (`short`), `start` (`coord3`), `end` (`coord3`) | `CL_ParseBeam(Mod_ForName("progs/bolt2.mdl"))` | Ja | Ja |
| `TE_WIZSPIKE` | 7 | `pos` (`coord3`) | `R_RunParticleEffect(..., color=20, count=30)`, Sound `wizard/hit.wav` | Ja | Ja |
| `TE_KNIGHTSPIKE` | 8 | `pos` (`coord3`) | `R_RunParticleEffect(..., color=226, count=20)`, Sound `hknight/hit.wav` | Ja | Ja |
| `TE_LIGHTNING3` | 9 | `ent` (`short`), `start` (`coord3`), `end` (`coord3`) | `CL_ParseBeam(Mod_ForName("progs/bolt3.mdl"))` | Ja | Ja |
| `TE_LAVASPLASH` | 10 | `pos` (`coord3`) | `R_LavaSplash(pos)` | Ja (Spezial-Splash) | Ja |
| `TE_TELEPORT` | 11 | `pos` (`coord3`) | `R_TeleportSplash(pos)` | Ja | Ja |
| `TE_EXPLOSION2` | 12 | `pos` (`coord3`), `colorStart` (`byte`), `colorLength` (`byte`) | `V_AddExplosionVibration`, `R_ParticleExplosion2(pos, colorStart, colorLength)`, Scorch-Decal, Dynamic Light (`DLIGHT_EXPLOSION`), Sound `r_exp3` | Ja (wichtig für palette-/color-range-basierte Varianten) | Ja |
| `TE_BEAM` | 13 | `ent` (`short`), `start` (`coord3`), `end` (`coord3`) | `CL_ParseBeam(Mod_ForName("progs/beam.mdl"))` (Grapple-Beam) | Ja | Ja |

## Parameterabdeckung nach gewünschter Taxonomie

- `pos`: Alle nicht-Beam-Events (`TE_*` außer `TE_LIGHTNING*`/`TE_BEAM`).
- `dir`: Kein `TE_*` liefert explizit eine Richtung; bei Decals wird Normal clientseitig rekonstruiert (`ImpactTrace` oder Fallback aus Viewrichtung).
- `color`: Implizit bei `R_RunParticleEffect` (z. B. 0, 20, 226) sowie explizit bei `TE_EXPLOSION2` (`colorStart`, `colorLength`).
- `count`: `R_RunParticleEffect`-Count (z. B. 10, 20, 30).
- `model`: Nur Beam-Typen, fest verdrahtet über `Mod_ForName("progs/*.mdl")`.
- `sound`: `TE_SPIKE`, `TE_SUPERSPIKE`, `TE_WIZSPIKE`, `TE_KNIGHTSPIKE`, `TE_EXPLOSION`, `TE_TAREXPLOSION`, `TE_EXPLOSION2`.
- `decal`: `TE_SPIKE`, `TE_SUPERSPIKE`, `TE_GUNSHOT`, `TE_EXPLOSION`, `TE_TAREXPLOSION`, `TE_EXPLOSION2`.

## Hinweise für q3p-Migration

- Alle aktuellen `TE_*` sind aus Migrationssicht **q3p-geeignet**, weil jede Payload klar aus `pos` oder `ent+start+end` besteht und zusätzliche visuelle/soundseitige Ausprägungen clientseitig deterministisch abgeleitet werden.
- Gleichzeitig sollten die bestehenden Legacy-Pfade (insb. Sound-Auswahl, Particle-Looks, Decal-Normal-Fallback) für Demo-/Mod-Kompatibilität beibehalten werden.
