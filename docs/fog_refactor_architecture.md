# Fog/Froxel Refactor Architecture

## Pipeline (single backend)

Ironwail now uses one froxel pipeline with three explicit stages:

1. **Density build** (`atmos_froxel_build.comp`)
   - Inputs: global map fog (`Fog_GetDensity`/`Fog_GetColor`), unified `r_fog_*` controls, parsed `fog_volume` entities.
   - Produces:
     - `scatter_tex` (`RGBA16F`): `rgb = sigma_s`, `a = sigma_t`
     - `transmittance_tex` (`RG16F`): `r = per-froxel transmittance`, `g = packed local phase g`
2. **Light injection + phase** (`atmos_froxel_integrate.comp`)
   - Reuses clustered dynamic lights + sun/shadow data.
   - Computes single-scattering term with HG phase and writes froxel scattering/transmittance.
3. **Integration/composite** (`postprocess.frag`)
   - Ray-integrates froxel slices up to scene depth and composites `scene * T + scatter`.

No separate fog backend is introduced.

## Fog volume authoring (map entities)

Supported entities in BSP entity lump:
- `classname` = `fog_volume` or `env_fog_volume`

Supported keys:
- `shape` = `box` (default) / `sphere`
- `origin`
- box: `mins`, `maxs`
- sphere: `radius`
- `density`
- `color` / `albedo`
- `height_falloff`
- `noise_amount`
- `noise_scale`
- `anisotropy` / `g`
- `blend` = `add` (default) / `override`
- `flags` (reserved)

Parsing is done once on map load (`R_Fog_ParseVolumes`) and uploaded to GPU SSBO each frame with configurable cap (`r_fog_volume_max_active`).

## Coordinate spaces and formats

- Froxel coords: clip-space XY + logarithmic Z slice.
- Medium eval: world-space froxel center (stable camera-independent noise).
- Lighting: world-space shading, clustered light lookup using existing cluster buffers.

## Extending volume types

Additions are localized:
1. Extend CPU `fog_volume_shape_t` parser.
2. Extend GPU packed struct encoding.
3. Extend `ApplyVolumes()` shape test in `atmos_froxel_build.comp`.

No second fog pipeline should be added.

## Perf/scaling knobs

- `r_fog_froxel_res`, `r_fog_zslices`, `r_fog_quality`
- `r_fog_temporal`, `r_fog_history_weight`, `r_fog_jitter`
- `r_fog_volume_max_active` (caps per-frame uploaded volume count)

Expected scaling is primarily froxel resolution (`W*H*Z`) and active volume count (inside density build shader loop).

## Debug/validation

- `r_fog_debug` gate
- `r_fog_debug_mode`:
  - 0 off
  - 1 density
  - 2 transmittance
  - 3 lighting
  - 4 sliceZ
  - 5 selected Z slice (`r_fog_debug_slice`)
  - 6 froxel_grid
- `r_fog_debug_volume_bounds`: draw parsed volume bounds as wire boxes
- `r_fog_validate`: keep existing validation path for fog passes/state
