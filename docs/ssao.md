# SSAO Debugging & Conventions

## View-space conventions
- Ironwail uses a **+X forward** view-space (camera looks down +X).
- SSAO comparisons operate in view-space using `viewPos.x` as depth.
- Depth is reconstructed via inverse projection from NDC.
- Reverse-Z is supported (see below) with a single source of truth in the SSAO shaders.

## Reverse-Z handling
- `u_depthParams.z` indicates reverse-Z.
- `u_reversedZMode` supports validation:
  - `0`: default
  - `1`: invert raw depth (before NDC)
  - `2`: invert NDC
- Debug mode 1/2/3 should remain monotonic when toggling these.

## AO resolution mapping
- The SSAO pass can run at full or half resolution.
- AO UVs are derived from AO buffer size, while depth sampling is derived from screen size.
- AO pixel centers are mapped to screen pixel centers to avoid half-res sampling seams.

## Debug modes (`r_ssao_debug`)
- `-1`: off
- `0`: final AO
- `1`: depth_raw (0..1)
- `2`: view-space Z (linearized)
- `3`: viewPos.xyz (each channel)
- `4`: normals (reconstructed)
- `5`: AO raw (pre-blur)
- `6`: AO blurred
- `7`: noise/rotation
- `8`: texel grid (AO vs screen)

## Noise controls
- `r_ssao_noise` toggles per-pixel rotation (IGN-based, no tiling).
- `r_ssao_freeze_noise` freezes the noise seed for debugging.

## Normal source
- `r_ssao_normalsource 0`: neighbor-based reconstruction from depth.
- `r_ssao_normalsource 1`: derivative-based reconstruction (for comparison).

## Test matrix
<!--
SSAO_TEST_MATRIX
Maps: e1m1, start
Resolutions: 1080p, 1440p
Toggles:
  r_ssao_halfres 0/1
  r_ssao_noise 0/1
  r_ssao_normalsource 0/1
  r_ssao_reversedz_mode 0/1/2
Expect: no stripes/banding; debug views are stable.
-->
