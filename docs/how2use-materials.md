# Material Workflow

Ironwail loads `materials/*.material` through the material pipeline. These files
drive texture metadata, stage behavior, emissive/bloom settings, and particle
compatibility.

## Console workflow

1. List loaded materials

   `materiallist`

2. Inspect one material

   `materialprint textures/common/nodraw`

3. Reload all materials

   `r_reloadmaterials 1`

4. Fuzz the parser

   `materialfuzz`

## Current CVars

- `r_materials` (`1`): master enable.
- `r_material_debug` (`0`): runtime debug toggle.
- `r_material_debug_parse` (`0`): parser trace output.
- `r_material_report` (`0`): emit a markdown support report during load.
- `r_tcgen_debug` (`0`): tcGen diagnostics.
- `r_particles_material_strict` (`0`): particle-stage compatibility policy.

## Public API

Include `mat_material.h` for:

- `Material_Init()` / `Material_Reload()` / `Material_Shutdown()`
- `Material_Count()` / `Material_GetByIndex()` / `Material_Find()`
- `Material_FindForTextureName()` / `Material_ApplyToTexture()`
- `Material_GetTextureFlags()` / `Material_Print()`

## Supported scope

- Top-level surface flags, emissive/bloom keywords, and the practical stage subset used by Ironwail are supported.
- Unknown tokens are tracked and reported instead of aborting the load.
- The current manifest reference lives in `docs/material_shader_manifest.md`.
