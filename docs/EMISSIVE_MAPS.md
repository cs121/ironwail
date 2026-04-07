# Emissive (glow) maps

Ironwail supports separate emissive textures alongside the base diffuse map. These
textures are added after lighting and are useful for light sources, screens, or
other details that should glow regardless of scene lighting.

## Naming conventions
- Prefer the `_emissive` suffix: a base texture named `brick` looks for
  `brick_emissive`.
- For compatibility, `_glow` and `_luma` are also accepted suffixes.

## BSP world textures
- Emissive maps are loaded from the same locations as external replacement
  diffuse textures: `textures/<map>/<name>` first, then `textures/<name>`.
- Use the same directory and filename as the base replacement texture, with one
  of the accepted emissive suffixes appended (for example, `textures/e1m1/door_emissive`).

## Model textures
- MD5 mesh skins expect emissive maps alongside their diffuse textures using the
  same `progs/<shader>_<skin>_<frame>` naming used for the base image
  (e.g. `progs/ogre_00_00_emissive`).
- Classic alias models currently only expose emissive data that ships with the
  model; they do not look for external glow files.

After adding emissive textures, launch the game normally. The renderer will
automatically bind and apply the glow contribution when the matching base
texture is loaded.

## Runtime lighting proxy budget
- `r_ppdlights_emissive` enables emissive proxy lights that feed into the shared
  realtime frame-light list.
- `r_ppdlights_emissive_budget` controls how many emissive proxy lights can be
  accepted per frame (default `24`, clamped to `0..DLIGHT_GPU_MAX`).
- Use `r_ppdlights_stats` and `r_ppdlights_emissive_debug 1` to inspect accepted
  emissive proxies and budget-based rejections.

  path: when set to `1` (default), the material is used only if one of these names contains
  a light token (`light`, `lamp`, `glow`, `flare`, `neon`, `torch`, `lantern`):
  - BSP texture name,
  - resolved shader map path (`map`),
  - stage map path, or
  - shader material name.
  stages to emit via the light-texture path.
