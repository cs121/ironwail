# Unified Fog CVARs (Phase 1)

## Neuer Namespace (`r_fog_*`)

| CVAR | Default | Bedeutung |
|---|---:|---|
| `r_fog_enable` | `1` | Master-Gate für Fog-System |
| `r_fog_backend` | `1` | `0=legacy`, `1=froxel`, `2=fogvol-fallback` |
| `r_fog_density` | `1.0` | Globaler Multiplikator auf map/legacy fog density |
| `r_fog_quality` | `1` | Qualitätsstufe (`0..3`), steuert primär Integrationsschritte |
| `r_fog_froxel_res` | `1` | Froxel XY-Auflösung (`0..3`) |
| `r_fog_zslices` | `64` | Froxel Z-Slices (`8..128`) |
| `r_fog_temporal` | `1` | Temporal resolve on/off |
| `r_fog_history_weight` | `0.9` | Temporal history weight |
| `r_fog_jitter` | `1` | Jitter für Reprojection/Sampling |
| `r_fog_anisotropy` | `0.2` | HG-artiger g-Parameter (`-0.9..0.9`) |
| `r_fog_debug` | `0` | Debug level/gating |
| `r_fog_validate` | `0` | Schaltet harte Validation/Hazard checks zu |

## Migrationstabelle: Alt -> Neu

| Legacy CVAR | Neuer CVAR | Strategie |
|---|---|---|
| `r_atmos_froxel` | `r_fog_backend=1` | kompatibel, deprecated warning once |
| `r_fogvol` | `r_fog_backend=2` | kompatibel, deprecated warning once |
| `r_fogvol_temporal` | `r_fog_temporal` | kompatibel, deprecated warning once |
| `r_fogvol_jitter` | `r_fog_jitter` | kompatibel, deprecated warning once |
| `r_fogvol_validate` | `r_fog_validate` | kompatibel, deprecated warning once |

Hinweis: Laufzeitsteuerung für Froxel-Fog erfolgt über `r_fog_*`.
