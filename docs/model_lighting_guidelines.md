# Model Lighting Guidelines (Legacy-safe)

This document captures map/content practices for stable item/monster/weapon lighting
while keeping classic Quake lightmaps + dlights as the core system.

## Goals
- Keep `r_model_light_multisample 0` behavior available as an exact fallback.
- Reduce visible popping/flicker for moving alias entities.
- Avoid "black hole" lightgrid cells on items and pickups.

## Recommended defaults
- `r_model_light_multisample 1`
- `r_model_light_smooth 0.18`
- `r_model_lightgrid_assist 1`
- `r_model_lightgrid_assist_threshold 0.03`
- `r_dlight_models_directional 0.35`
- `r_bmodel_relight 0`

These values stay close to legacy visuals but stabilize edge cases.

## Lightgrid density
- Keep probe spacing tighter in gameplay-critical spaces:
  - stairs, door thresholds, narrow corridors, lift transitions.
- Avoid very sparse probes near high-contrast light transitions.
- When testing, inspect moving items and monsters at walking speed first.

## Model bounds and sampling behavior
- Very large alias bounds amplify single-point sampling artifacts.
- Prefer sensible model bounds in content:
  - avoid oversized invisible extents unless required for gameplay.
  - avoid tiny clipped bounds that miss obvious body volume.
- If a model must be large, multisample (`r_model_light_multisample 1`) should remain enabled.

## Fullbright and emissive textures
- Fullbright textures bypass part of model-light contrast by design.
- Emissive textures should be used for local glow accents, not as a replacement for world lighting.
- For readability under dynamic lights:
  - keep emissive intensity moderate,
  - avoid combining extreme emissive + high overbright + strong minlight.

## Brush model relight note
- `r_bmodel_relight` is intentionally opt-in.
- Enable it when moving doors/plats look implausible in strongly different destination lighting.
- Keep it off for strict vanilla behavior matching.

## Debug workflow
- Use `r_debug_itemlight 1` to print per-entity static source + dynamic/final contributions.
- Use `r_model_light_stats 1` for periodic per-frame timing/sample counters.
- Raise to `r_model_light_stats 2` for per-frame output in short profiling runs only.

