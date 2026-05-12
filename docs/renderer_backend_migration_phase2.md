# Renderer Backend Migration Phase 2

## 1. Goal of Phase 2

Phase 2 is a preparation-only phase for renderer core decontamination and later resource-handle migration. It makes the remaining OpenGL leaks measurable and documents the safest first texture-handle migration candidate without changing rendering behavior.

The intended output is:

- expanded `r_backend_migration_audit` diagnostics;
- a clearer `model_types.h` future split point;
- explicit public `gltexture_t` legacy-bridge labeling;
- a static inventory of `gltexture_t` usage classes;
- a ranked first neutral texture-handle migration candidate;
- comments only for future API-neutral resource descriptors.

## 2. Non-goals

Phase 2 explicitly does **not**:

- remove OpenGL;
- hard-refactor `quakedef.h`;
- replace `gltexture_t`;
- split or dismantle `gl_rmain.c`;
- enable Vulkan or DX12;
- change renderer output;
- change public renderer ABI or plugin structs;
- migrate world, alias, shadow, or FBO attachment resources.

## 3. Core GL leaks

Known core/header contamination remains intentional and measurable:

- `Quake/src/core/quakedef.h` includes `SDL_opengl.h`.
- `Quake/src/core/quakedef.h` includes `gl_model.h`.
- `Quake/src/core/quakedef.h` includes `gl_texmgr.h`.
- `Quake/src/core/model_types.h` remains a shim over `gl_model.h`.
- Render/frontend files still include `glquake.h` directly.

These are not fixed in Phase 2. They are audit targets for later core decontamination.

## 4. Public `gltexture_t` leak

`gltexture_t` remains a public legacy OpenGL bridge. The native GL fields that leak backend implementation details are:

- `target` (`GLenum`);
- `texnum` (`GLuint`);
- `bindless_handle` (`GLuint64`);
- `internal_format` (`GLenum`).

`texture_handles.h` already defines `render_texture_handle_t`, but current frontend and material/model paths still bridge through public `gltexture_t` pointers. Long-term frontend code should only see `render_texture_handle_t`; native GL data should move behind the GL backend or a GL resource layer.

## 5. Header dependency hotspots

Frontend/render files with direct GL-header dependency called out by the audit command:

- `r_world.c`
- `r_alias.c`
- `r_brush.c`
- `r_sprite.c`
- `r_part.c`
- `r_part_q3p.c`
- `r_decals.c`
- `r_postfx.c`

These are hotspots, not migration targets for Phase 2.

## 6. `gltexture_t` usage categories

Static inventory snapshot gathered with `rg` during Phase 2:

- `gltexture_t` references in `Quake/src`: 130 matching lines.
- Direct `texnum` field references: 36 matching lines.
- Direct `target` field references: 70 matching lines.
- Direct `internal_format` field references: 19 matching lines.
- Direct `bindless_handle` field references: 12 matching lines.

### A. Safe frontend reference

Code holds or forwards `gltexture_t *` without directly touching native GL fields in many call sites:

- `r_alias.c` player/model texture arrays;
- `r_brush.c` and sprite batching references;
- `r_sprite.c` batch texture selection;
- `r_part_q3p.c` material stage texture resolution;
- portions of `r_decals.c` and `r_world.c` where textures are selected or passed onward.

These are possible future adapter users, but they are not migrated in Phase 2.

### B. Native GL access

Direct backend-native access remains in GL-specific or tightly-coupled code:

- `texnum` object IDs;
- `target` bind targets;
- `internal_format` upload formats;
- `bindless_handle` world/material bindless paths.

The heaviest native access is expected in `gl_texmgr.c`, with additional coupling in `r_world.c`, post-processing/FBO paths, and backend resource glue.

### C. Upload/lifetime

Texture lifetime remains owned by the existing texture manager path:

- `TexMgr_NewTexture`
- `TexMgr_LoadImage`
- `TexMgr_LoadImageEx`
- `TexMgr_ReloadImage`
- `TexMgr_FreeTexture`
- `GL_DeleteTexture` / native `glDeleteTextures`

No ownership move occurs in Phase 2.

### D. Material/model binding

The following binding domains still depend on `gltexture_t`:

- world textures;
- alias model skins;
- sprite textures;
- particles;
- decals;
- UI/HUD/2D draw assets;
- post-processing textures/LUTs;
- lightmaps and other GL-managed renderer resources.

## 7. Candidate ranking for first neutral texture-handle migration

| Rank | Candidate | Assessment |
| --- | --- | --- |
| 1 | PostFX LUT | Best first adapter candidate: narrow scope, already conceptually a generated renderer resource, low material/model dependency, easier toggle/fallback. |
| 2 | Isolated UI/2D texture | Good fallback candidate: visible but usually simple binding path; choose one isolated texture, not the whole UI atlas/scrap system. |
| 3 | Decal texture | Medium risk: fewer dependencies than world/alias, but visible in scene and tied to material-style batching. |
| 4 | Particle texture | Medium risk: potentially simple texture references, but high visual sensitivity and particle batching/state coupling. |
| 5 | World texture | Do not start here: high dependency count, material stages, bindless, lightmaps, batching, and visible regression risk. |
| 6 | Alias skin | Do not start here: model skin/fb skin paths, player translations, animation, and material binding increase risk. |
| 7 | Shadow map | Do not start here: FBO/depth/sampler/compare-state coupling and high regression risk. |
| 8 | FBO attachment | Do not start here: ownership belongs to future framegraph/render-target resource work, not texture-handle adapter smoke tests. |

Recommendation: Phase 3 should start with the PostFX LUT. If that path proves too coupled, use a single isolated UI/2D texture as the backup candidate.

## 8. Go/No-Go criteria for Phase 3

Phase 2 is GO only if:

- the project compiles in the target Windows/MSBuild environment;
- no visible render path changed;
- `quakedef.h` still contains the old GL includes;
- `gl_texmgr.h` is functionally unchanged;
- `gltexture_t` is not replaced;
- Vulkan/DX12 remain blocked;
- `r_backend_migration_audit` names all known GL leaks;
- the first neutral texture-handle candidate is clearly recommended.

Phase 3 preview only, not implemented here:

- **Phase 3-A:** add a small texture-handle adapter for PostFX LUT or one isolated UI/2D texture; frontend sees `render_texture_handle_t`; GL backend keeps native `GLuint`.
- **Phase 3-B:** do not migrate world/alias/shadows; keep the legacy `gltexture_t` fallback path.
- **Phase 3-C:** use pixel equality or very tight visual tolerance; gate the new path behind a toggle/CVar; default to the legacy path first.
