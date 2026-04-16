# Decal Shader Manual

This manual describes the current decal system in Ironwail: runtime behavior,
shader syntax, and debugging.

## Runtime flow

- Temp-entity impacts in `cl_tent.c` spawn decal categories such as `bullet` and `scorch`.
- `R_SpawnImpactDecalEx()` handles blood variants with hit direction and heavy-blood weighting.
- `R_FindDecalDefByCategory()` collects valid matching definitions and picks one at random.

## Script loading

- Decal definitions are loaded from `decals.material` and `materials/decals.material`.
- The engine keeps the loaded `decal` blocks in memory and uses them as the live manifest.

## Definition syntax

```shader
decal my_decal_name {
  texture "decals/my/path"
  category bullet
  size 5 8
  alpha 0.6 0.9
  color 1 1 1
  lifetime 32
  fade 8
  blend alpha
  random_rotation 1
  priority 4
  atlas_rect 0.0 0.0 0.5 0.5
}
```

Required keys:

- `texture`
- `category`

Common keys:

- `size`
- `alpha`
- `color`
- `lifetime`
- `fade`
- `blend`
- `random_rotation`
- `priority`
- `atlas_rect` / `uvrect`

## Rules

- Multiple decals may share the same category.
- Unknown keywords are tolerated and ignored.
- The instance pool is fixed; `r_decals_max` is clamped to the pool size.
- `r_decals_instanced` controls the instanced draw path.

## Debugging

- `r_decals` enables or disables the system.
- `r_decals_debug 1` prints spawn/reject reasons.
- `r_decals_debug 2` adds frame stats.

Common reject reasons include `decals_disabled`, `category_missing`, `projection_empty`, and `instance_pool_exhausted`.

