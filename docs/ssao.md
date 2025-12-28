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
- AO pixels are mapped to integer screen texels via `texelFetch` to avoid half-res UV drift and banding.

## Debug modes (`r_ssao_debug`)
- `0`: off
- `1`: depth raw (0..1)
- `2`: depth linear (view-space depth from raw)
- `3`: view-space Z (reconstructed)
- `4`: normals (reconstructed)
- `5`: AO raw (pre-blur)
- `6`: AO blurred
- `7`: AO upscaled (post-process sample)
- `8`: noise/rotation
- `9`: source texel coords (debug overlay)

## Noise controls
- `r_ssao_noise` toggles per-pixel rotation.
- `r_ssao_noise_mode` selects noise source (`1`=IGN hash, `2`=noise texture).
- `r_ssao_freeze_noise` freezes the noise seed for debugging.

## Format + blur controls
- `r_ssao_format` selects AO target precision (`0`=R8, `1`=R16F).
- `r_ssao_blur_bilateral` toggles depth-aware blur weighting.
- `r_ssao_upscale_nearest` toggles nearest-neighbor AO upsampling (diagnostic).

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

## Changelog
- SSAO: fix half-res depth sampling by snapping to integer depth texels; add upscaling toggle + expanded debug views.
