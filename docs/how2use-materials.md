# Working with Material Files in Ironwail

Ironwail loads material definitions from `materials/*.material` through the
material pipeline. These definitions are used for texture metadata and selected
render-stage behavior (including particle material integration).

## Console workflow

1. **List loaded materials**

   ```
   materiallist
   ```

   Optional argument: `materiallist <limit>` to clamp printed rows.

2. **Inspect one material**

   ```
   materialprint textures/common/nodraw
   ```

   settings, and stage details.

3. **Reload all materials**

   ```
   r_reloadmaterials 1
   ```

   Setting this cvar triggers a full reload and then resets the cvar to `0`.

4. **Developer-only parser fuzzing**

   ```
   materialfuzz
   ```

   Or set `r_material_fuzz 1` for callback-based fuzz runs.

## Relevant CVars

- `r_materials` (`1`): master enable for loading and applying materials.
- `r_material_debug` (`0`): runtime material debug toggle.
- `r_material_debug_parse` (`0`): print parser debug information.
- `r_material_report` (`0`): write markdown parser support report during load.
- `r_particles_material_strict` (`0`): strict compatibility policy for particle
  stage support.

## C API overview

Include `mat_material.h` for public APIs.

- Lifecycle/load:
  - `void Material_Init(void);`
  - `void Material_Reload(void);`
  - `void Material_Shutdown(void);`
- Registry/query:
  - `size_t Material_Count(void);`
  - `const material_t *Material_GetByIndex(size_t index);`
  - `const material_t *Material_Find(const char *name);`
  - `const material_t *Material_FindForTextureName(const char *texname, const char *mapname);`
- Texture integration:
  - `void Material_ApplyToTexture(texture_t *tex, const char *mapname);`
  - `unsigned int Material_GetTextureFlags(const material_t *material);`
- Debug/introspection:
  - `void Material_Print(const material_t *material);`

## Current support scope

The parser currently focuses on a practical subset (for engine needs), including
material-level directives and stage fields used by Ironwail. Unknown tokens are
tracked and summarized in parser reporting instead of aborting load.
