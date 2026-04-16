# Model Lighting Guidelines

This file captures current best-practice settings for alias models, viewmodels,
monsters, and moving brush entities.

## Current goals

- Keep `r_model_light_multisample 0` as the exact fallback.
- Reduce popping across hard lightgrid boundaries.
- Avoid black samples on items and pickups.

## Recommended defaults

- `r_model_light_multisample 1`
- `r_model_light_smooth 0.18`
- `r_model_lightgrid_assist 1`
- `r_model_lightgrid_assist_threshold 0.03`
- `r_minlight_models 0.02`
- `r_bmodel_relight 0`

## Content guidance

- Keep lightgrid probes denser in stairs, doorways, narrow corridors, and lift transitions.
- Avoid very sparse probes around sharp light/dark edges.
- Use sensible model bounds; oversized invisible extents make sampling less stable.
- If a model must be large, keep multisample enabled.

## Viewmodel note

- The weapon model uses the same static-light pipeline, but it gets a separate boost/minlight treatment in `r_alias.c`.
- World light direction data is not applied to `cl.viewent` the same way it is for world alias models.

## Emissive and fullbright

- Fullbright textures still bypass some contrast by design.
- Emissive textures should be used as local glow accents, not as a replacement for scene lighting.
- Avoid stacking extreme emissive, overbright, and minlight values together.

## Debug workflow

- `r_debug_itemlight 1` prints static source, dynamic contribution, and final color.
- `r_model_light_stats 1` reports per-frame timing/sample counters.
- `r_model_light_stats 2` is useful for short profiling runs only.

