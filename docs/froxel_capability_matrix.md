# Froxel/Volumetric Fog Audit – Capability Matrix

Scope audited:
- Froxel grid setup (XY/Z resolution + logarithmic slice mapping)
- Density field (global height fog + per-volume noise)
- Lighting injection (sun + clustered dynamic lights)
- Sun shadow sampling in froxel integration
- In-scatter + transmittance integration and post-compose
- Temporal history blend (froxel history ping-pong)
- Reverse-Z/depth reconstruction paths in fog/shadow helpers
- Existing fog debug modes

## Capability Matrix

- ✅ Froxel grid setup is present and correctly parameterized via existing CVars (`r_fog_froxel_res`, `r_fog_zslices`) and log-depth slicing in build/integrate shaders.
- ✅ World-space density/noise sampling is already present (`viewPos -> worldPos`, then height/noise in world space).
- ✅ Sun lighting + shadowmap visibility are already integrated in the froxel integrate pass using existing shadow sampling helpers (`shadow_sample.glsl`).
- ✅ Transmittance (`exp(-sigma_t * ds)`) and in-scatter integration are present and composed in postprocess.
- ✅ Temporal accumulation exists (history ping-pong + neighborhood clamp) and uses existing frame-valid gating.
- ✅ Reverse-Z handling is centralized in existing depth/shadow helper code used by fog paths.
- ⚠️ Debug coverage gap: no dedicated froxel debug mode for **volume shadow visibility** despite requirement/useful validation path.

## Changes made in this patch

- ⚠️ Fixed/extended only debug instrumentation:
  - Added froxel integrate debug mode **7** = "Shadow visibility in volume".
  - Extended debug-mode clamping from `[0..6]` to `[0..7]` where fog debug mode is interpreted.

No new render path, no new CVar, no new texture/buffer/pass.
