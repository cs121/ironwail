# Fog Audit (Phase 0)

## Fog-Pipeline heute (Ist-Zustand)

```text
Map fog (gl_fog.c / Fog_*)
  -> optional fixed-function scene fog during world render

Atmosphere orchestration (gl_rmain.c / R_Atmosphere_Render)
  -> volumetric backend
     - mode=froxel: atmos_froxel_build.comp -> atmos_froxel_integrate.comp -> atmos_froxel_temporal.comp
       + optional fog-volume injection via R_FogVol_InjectBuiltIntoFroxel()
     - mode=fogvol fallback: fixed-function fallback path (no dedicated fogvol GLSL files)
  -> optional godrays pass (godrays_*.frag)
  -> post composite (postprocess.frag)
```

## Fog-Pfade / Module

- Legacy/global fog: `Quake/gl_fog.c` (`Fog_ParseServerMessage`, `Fog_GetDensity`, `Fog_EnableGFog`).
- Fog volume backend: `Quake/r_fogvol.c`, `Quake/r_fogvol.h`.
- Atmos/froxel orchestration and gating: `Quake/gl_rmain.c` (`Atmosphere_*`, `R_Atmosphere_Render`).
- Postprocess-Unterwasser-Fog-Farbanteil: `Quake/cl_postfx.c`, `Quake/r_postfx.c`.
- Godrays coupling: `Quake/gl_rmain.c`, shaders in `Quake/shaders/godrays*.frag`.

## Shader-Inventar (Fog-relevant)

- Froxel build/integration/history:
  - `Quake/shaders/atmos_froxel_build.comp`
  - `Quake/shaders/atmos_froxel_integrate.comp`
  - `Quake/shaders/atmos_froxel_temporal.comp`
- Final post composite path:
  - `Quake/shaders/postprocess.frag`
- Atmos shafts:
  - `Quake/shaders/godrays_mask.frag`
  - `Quake/shaders/godrays.frag`

## FBOs / Texturen / History

- Fogvol: `framebufs.fogvol.*` (ping-pong color, history, composite, finalcopy).
- Froxel: `framebufs.atmos_froxel.*` (3D scatter/transmittance + history/resolved textures).
- Composite target: `framebufs.composite.*`.
- Godrays: `framebufs.godrays.*`.

## Truth Table (Ist, nach Refactor-Gates)

| `r_fog_enable` | `r_fog_backend` | Resultierender Pfad |
|---|---:|---|
| 0 | any | Nur legacy/map fog (falls map fog aktiv), keine volumetrics |
| 1 | 0 | Legacy-only kompatibel (kein volumetric pass) |
| 1 | 1 | Einziger volumetric Hauptpfad: Froxel (`Atmosphere_Froxel_*`) |
| 1 | 2 | Fallback volumetric: Fogvol (`R_FogVol_Render`) |

**Mutual exclusion:** Froxel und Fogvol laufen nicht parallel mehr; Backend-Gate entscheidet eindeutig.

## Hauptprobleme (vor Fix)

- Mehrere Namensräume (`r_atmos_*`, `r_fogvol_*`, `r_godrays_*`) mit teilweise überlappender Semantik.
- Dichte-/History-/Jitter-Steuerung an mehreren Stellen, inkonsistente Defaults.
- Bereits vorhandene Hazard-/Validate-Guards nur im Fogvol-Pfad, nicht sauber über zentrales Fog-Gate gebündelt.
