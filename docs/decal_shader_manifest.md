# Decal Shader Manifest

This file is the manifest-style reference for Ironwail decal shader syntax.
It documents the effective decal definition contract used by the engine.

## Scope
- Source format: `decals.material`
- Loaded from `decals.material` and `materials/decals.material`
- Defines runtime decal categories and textures

## Block form
```text
decal <name> {
  ...
}
```

## Required fields
- `texture` string
- `category` string

## Common fields
- `size` float range `min max`
- `alpha` float range `min max`
- `color` float `r g b`
- `lifetime` float
- `fade` float
- `blend` enum: `alpha`, `add`, `mul`
- `random_rotation` bool/int
- `priority` int
- `atlas_rect` float `u0 v0 u1 v1`
- `uvrect` alias of `atlas_rect`

## Rules
- Multiple decals may share the same category.
- Category lookup chooses from valid matching decals at runtime.
- Unknown keywords are currently ignored rather than hard-failing the script.

## Example
```text
decal bullet_hole_default {
  texture "decals/bullet/bhole_01"
  size 5 8
  alpha 0.65 0.90
  blend alpha
  random_rotation 1
  category bullet
}
```
