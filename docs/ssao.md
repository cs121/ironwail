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
- `0`: off
- `1`: AO raw (pre-blur)
- `2`: AO blurred
- `3`: noise/rotation
- `4`: view-space Z (linearized)
- `5`: normals (reconstructed)
- `6`: AO UVs (uv.x/uv.y gradient)

## Noise controls
- `r_ssao_noise` toggles per-pixel rotation.
- `r_ssao_noise_mode` selects noise source (`1`=IGN hash, `2`=noise texture).
- `r_ssao_freeze_noise` freezes the noise seed for debugging.

## Format + blur controls
- `r_ssao_format` selects AO target precision (`0`=R8, `1`=R16F).
- `r_ssao_blur_bilateral` toggles depth-aware blur weighting.

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
