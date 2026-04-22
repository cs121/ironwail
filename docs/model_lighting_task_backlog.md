# Model Lighting Backlog

This is the current status list for model/entity lighting work. Items that are
already present in the code are marked as done; the rest remain open ideas.

## Done

- Multi-point static sampling for alias/sprite entities via `r_model_light_multisample`.
- Static smoothing via `r_model_light_smooth`.
- Lightgrid assist for dark/unstable cells via `r_model_lightgrid_assist`.
- Brush-model relight via `r_bmodel_relight`.

## Still open

- Directional dlight shading for models. Current alias shading is still additive.
- Robust inverse-transpose normal handling for non-uniform scale.
- Unified lighting debug overlay/logging for alias/sprite/bmodel paths.
- Additional content-side guidance for map authors when grid density is poor.

## Notes

- `r_minlight_models` remains the fallback for very dark model samples.
- The weapon model (`cl.viewent`) still gets separate boost/minlight treatment in the alias path.
