# Working with Material Shader Files in Ironwail

> Note: this file keeps its historical filename, but the runtime feature is now the
> **material shader system** implemented in `mat_shader.*` (not the old `q3shader_*`
> console/API names).

Ironwail loads shader definitions from `scripts/*.shader` through the material
shader pipeline. These definitions are used for texture metadata and selected
render-stage behavior (including particle material integration).

## Console workflow

1. **List loaded shader materials**

   ```
   shaderlist
   ```

   Optional argument: `shaderlist <limit>` to clamp printed rows.

2. **Inspect one material**

   ```
   shaderprint textures/common/nodraw
   ```

   Prints source file, surface/render/content flags, emissive/bloom/godray
   settings, and stage details.

3. **Reload all material shaders**

   ```
   r_reloadshaders 1
   ```

   Setting this cvar triggers a full reload and then resets the cvar to `0`.

4. **Developer-only parser fuzzing**

   ```
   shaderfuzz
   ```

   Or set `r_matshader_fuzz 1` for callback-based fuzz runs.

## Relevant CVars

- `r_shaders` (`1`): master enable for loading and applying material shaders.
- `r_shader_debug` (`0`): runtime shader debug toggle.
- `r_matshader_debug_parse` (`0`): print parser debug information.
- `r_matshader_report` (`0`): write markdown parser support report during load.
- `r_particles_shader_strict` (`0`): strict compatibility policy for particle
  stage support.

## C API overview

Include `mat_shader.h` for public APIs.

- Lifecycle/load:
  - `void Mat_Shader_Init(void);`
  - `void Mat_Shader_Reload(void);`
  - `void Mat_Shader_Shutdown(void);`
- Registry/query:
  - `size_t Mat_Shader_Count(void);`
  - `const shader_material_t *Mat_Shader_GetByIndex(size_t index);`
  - `const shader_material_t *Mat_Shader_Find(const char *name);`
  - `const shader_material_t *Mat_Shader_FindForTextureName(const char *texname, const char *mapname);`
- Texture integration:
  - `void Mat_Shader_ApplyToTexture(texture_t *tex, const char *mapname);`
  - `unsigned int Mat_Shader_GetTextureFlags(const shader_material_t *material);`
- Debug/introspection:
  - `void Mat_Shader_Print(const shader_material_t *material);`

## Current support scope

The parser currently focuses on a practical subset (for engine needs), including
material-level directives and stage fields used by Ironwail. Unknown tokens are
tracked and summarized in parser reporting instead of aborting load.
