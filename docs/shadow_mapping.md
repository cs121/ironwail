# Shadow Mapping

## Pass order

The framegraph renders shadows before the main scene passes:

1. `R_SetupView`
2. `R_RenderShadowMaps`
   - `R_RenderSunShadowMap`
   - `R_RenderDLightShadowMaps`
3. Scene and lighting passes

## Current resources

- Sun shadows use a `GL_TEXTURE_2D` depth map.
- DLight shadows use a `GL_TEXTURE_CUBE_MAP_ARRAY` depth map.
- Resource recreation is tied to the framebuffer lifecycle and `vid_restart`.

## Casters and receivers

- Opaque world/brush geometry is shadow-casting today.
- Opaque alias models also cast shadows.
- Viewmodels, sprites, particles, water, and alpha-tested geometry are excluded.
- World and alias receivers both consume shadow factors in their lighting paths.

## DLight selection

- `r_shadow_dlight_max` defaults to `4`.
- Candidate selection is deterministic and scores lights by radius, luminance, and camera distance.

## Current CVars

- `r_shadow`
- `r_shadow_sun`
- `r_shadow_dlight`
- `r_shadow_dlight_max`
- `r_shadow_sun_size`
- `r_shadow_dlight_size`
- `r_shadow_sun_distance`
- `r_shadow_sun_bias`
- `r_shadow_dlight_bias`
- `r_shadow_receiver_bias`
- `r_shadow_sun_pcf`
- `r_shadow_dlight_pcf`
- `r_shadow_sun_snap`
- `r_shadow_sun_cascades`
- `r_shadow_sun_split1`
- `r_shadow_sun_split2`
- `r_shadow_sun_split3`
- `r_shadow_sun_split_mode`
- `r_shadow_sun_split_lambda`
- `r_shadow_mark_mode`
- `r_shadow_profile`
- `r_shadow_cull_vis`
- `r_shadow_cull_backface`
- `r_shadow_cull_frustum`
- `r_shadow_cull_sphere`
- `r_shadow_debug`
- `r_shadow_log`

## Limits

- One sun path with cascades.
- Up to four shadowed DLights.
- Alpha-tested casters remain unsupported in the current V1 path.
