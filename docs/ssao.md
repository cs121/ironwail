# SSAO Debugging & Current Conventions

## View-space conventions
- Ironwail uses a **+X forward** view-space (camera looks down +X).
- SSAO depth comparisons operate in view-space via `viewPos.x`.
- Depth is reconstructed from NDC using inverse projection.

## Reverse-Z handling
- `u_depthParams.z` carries reverse-Z state.
- `r_ssao_reversedz_mode` can override decode mode for diagnostics:
  - `0`: default path
  - `1`: invert raw depth before NDC mapping
  - `2`: invert NDC depth

## Resolution behavior
- SSAO can run half-res (`r_ssao_halfres 1`) or full-res.
- AO texels map to integer screen-depth texels via `texelFetch` to avoid
  half-res UV drift/banding.
- `r_ssao_force_fullres 1` forces full-res path while keeping other settings.

## Debug modes (`r_ssao_debug`)
- `0`: off
- `1`: raw AO
- `2`: AO * fog
- `3`: fog factor
- `4`: depth raw
- `5`: view-space Z
- `6`: reconstructed view-space position
- `7`: reconstructed normals
- `8`: noise/rotation visualization
- `9`: sample hit ratio
- `10`: AO raw (pre-blur)
- `11`: blur debug
- `12`: AO mask
- `13`: fog transmittance
- `14`: fog-damped AO

## Key controls
- Core: `r_ssao`, `r_ssao_radius`, `r_ssao_intensity`, `r_ssao_bias`,
  `r_ssao_power`, `r_ssao_min`, `r_ssao_samples`.
- Blur: `r_ssao_blur`, `r_ssao_blur_radius`, `r_ssao_blur_sigma`,
  `r_ssao_blur_bilateral`.
- Quality: `r_ssao_halfres`, `r_ssao_force_fullres`, `r_ssao_format`,
  `r_ssao_upscale_nearest`.
- Noise: `r_ssao_noise`, `r_ssao_noise_mode`, `r_ssao_noise_scale`,
  `r_ssao_freeze_noise`.
- Normals: `r_ssao_normalsource` (`0` neighbor reconstruction, `1` derivative).
- Fog coupling: `r_ssao_fog_strength`, `r_ssao_fog_power`.
- Safety/validation: `r_ssao_max_distance`, `r_ssao_validate`, `r_ssao_debug_far`.

## Quick verification matrix
1. `r_ssao 1; r_ssao_halfres 0/1`
2. `r_ssao_noise 0/1; r_ssao_normalsource 0/1`
3. `r_ssao_reversedz_mode 0/1/2`
4. Sweep debug modes `4`, `5`, `7`, `9`, `10`, `11`, `14` for stability.

Expected: no striping/banding regressions, stable debug outputs, and monotonic
reversed-Z diagnostic modes.
