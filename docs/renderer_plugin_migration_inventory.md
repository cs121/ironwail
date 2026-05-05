# Renderer Plugin Migration Inventory

Stand: 2026-05-05
Scope: `Quake/src`

## GL Dependency Matrix

| Area | Current GL Exposure | Classification | Notes |
|---|---|---|---|
| `core/quakedef.h` | Includes `gl_model.h`, `gl_texmgr.h`, `SDL_opengl.h` | transitional/legacy | Global transitive GL contamination source.
| `platform/gl_vidsdl.c` | SDL + GL context, function loading, GL state, swap/present, debug callback | transitional/legacy | Mixed host and renderer responsibilities.
| `render/r_backend.c` | No direct GL token usage, but transitively polluted by `quakedef.h` | renderer-neutral target | Candidate for header decontamination.
| `render/render_dispatch.c` | Legacy wide entrypoint adapter (`Draw_*`, `SCR_*`, `GL_Set2D`) | transitional/legacy | Keep as compatibility adapter.
| `render/ref_gl_plugin.c` | Plugin entrypoint + broad extern bridge to legacy render symbols | transitional/legacy | Functional, but tightly coupled to engine globals.
| `render/r_passes.c` | Mostly declarative pass plan | renderer-neutral target | Execution still reaches GL-centric implementations downstream.
| `render/gl_*` family | Direct GL API/types | ref_gl-only target | Target location for long-term GL ownership.

## Files with GL Includes / Types / Calls / Native Handles

Legend:
- includes: `glquake.h`, `gl_texmgr.h`, `gl_model.h`, `SDL_opengl.h`, `<OpenGL/OpenGL.h>`
- types/calls: `GL*` types, `gl*` calls
- native handles: `GLuint`, `GLenum`, `GLuint64`, FBO/VAO/program IDs

### Engine/Core and Platform contamination

| File | Evidence | Classification |
|---|---|---|
| `Quake/src/core/quakedef.h` | Includes `gl_model.h`, `gl_texmgr.h`, SDL OpenGL headers | engine/core neutral target |
| `Quake/src/platform/gl_vidsdl.c` | `gl*` calls, `GL*` types, `globalvao`, GL state | engine/core neutral target |
| `Quake/src/platform/pl_win.c` | limited GL token presence | transitional/legacy |

### Renderer-neutral and transitional files

| File | Evidence | Classification |
|---|---|---|
| `Quake/src/render/r_backend.c` | No direct GL symbol usage found | renderer-neutral target |
| `Quake/src/render/render_dispatch.c` | Legacy compatibility entrypoints (`Draw_*`, `SCR_*`, `GL_Set2D`) | transitional/legacy |
| `Quake/src/render/r_passes.c` | No direct GL tokens in unit | renderer-neutral target |
| `Quake/src/render/r_framegraph.h` | Neutral API surface | renderer-neutral target |
| `Quake/src/render/r_framegraph.c` | Neutral execution graph plumbing | renderer-neutral target |
| `Quake/src/render/ref_gl_plugin.c` | Plugin glue to broad legacy renderer exports | transitional/legacy |

### ref_gl-only target set (current GL-heavy set)

- `Quake/src/render/glquake.h`
- `Quake/src/render/gl_backend.c`
- `Quake/src/render/gl_backend_resources.c`
- `Quake/src/render/gl_backend_runtime.c`
- `Quake/src/render/gl_dlight.c`
- `Quake/src/render/gl_draw.c`
- `Quake/src/render/gl_fog.c`
- `Quake/src/render/gl_ktx2.c`
- `Quake/src/render/gl_lightgrid.c`
- `Quake/src/render/gl_mesh.c`
- `Quake/src/render/gl_model.c`
- `Quake/src/render/gl_model_glb.c`
- `Quake/src/render/gl_oit.c`
- `Quake/src/render/gl_refrag.c`
- `Quake/src/render/gl_rlight.c`
- `Quake/src/render/gl_rmain.c`
- `Quake/src/render/gl_rmisc.c`
- `Quake/src/render/gl_screen.c`
- `Quake/src/render/gl_shaders.c`
- `Quake/src/render/gl_shadow.c`
- `Quake/src/render/gl_shadow_runtime.c`
- `Quake/src/render/gl_sky.c`
- `Quake/src/render/gl_texmgr.c`
- `Quake/src/render/gl_warp.c`
- plus GL-coupled `r_*` files with direct GL tokens: `r_alias.c`, `r_brush.c`, `r_decals.c`, `r_part.c`, `r_part_q3p.c`, `r_postfx.c`, `r_sprite.c`, `r_world.c`

## Hotspot Analysis

### `core/quakedef.h`
- Hard includes `gl_model.h` and `gl_texmgr.h` force GL-typed structs and APIs into nearly all translation units.
- Includes SDL OpenGL headers globally.
- Main blocker for core agnosticism.

### `platform/gl_vidsdl.c`
- Owns both host windowing and GL context/state/present bootstrap.
- Contains GL extension probing, context-version checks, debug callback, VAO bootstrap, state masks.

### `render/r_backend.c`
- Does not directly use `glquake.h` symbols.
- Transitive GL reachability comes from `quakedef.h` only.

### `render/r_world.c`
- Direct GL resource and draw-path coupling (`GLuint`, `GLuint64`, `GL_BindBufferRange`, GL vertex attrib setup).

### `render/gl_texmgr.c`
- Native texture ownership, upload/bind, and bindless handle logic is GL-native.

### `render/gl_shadow_runtime.c`
- Shadow texture/FBO lifecycle and runtime uniforms are GL-native.

### `render/ref_gl_plugin.c`
- Dynamic plugin boundary exists, but large extern dependency surface to legacy renderer symbols.

### `render/render_dispatch.c`
- Compatibility adapter currently intentionally broad; should stay transitional until contract split stabilizes.

### `render/r_passes.c`
- Declarative pass model trend is good; execution target still mostly GL backend specific.

### `render/r_framegraph.*`
- Neutral graph layer exists; resource/type boundary not yet strict enough to prevent GL leakage through surrounding headers.

## Phase 1 Baseline: Header-Decontamination Preparation

### Why `quakedef.h` still includes GL headers
- Legacy convenience umbrella header pattern.
- `gl_model.h` carries common model/surface/world structs used broadly by renderer and gameplay-adjacent code.
- `gl_texmgr.h` exposes `gltexture_t` and texture manager APIs directly.

### Compile dependencies created by `quakedef.h`
- Any TU including `quakedef.h` receives GL-native types (`GLenum`, `GLuint`, `GLuint64`) via `gl_texmgr.h`.
- Any TU including `quakedef.h` receives renderer model internals via `gl_model.h`.
- Any TU including `quakedef.h` receives SDL OpenGL headers.

### Neutral replacement candidates (proposal only)
- New `core/model_types.h`: move renderer-neutral model structs and enums required by core/shared code.
- New `render/texture_handles.h`: opaque texture handles/types for engine-facing APIs.
- Keep `gl_texmgr.h` and GL-native fields private to ref_gl implementation units.

### Forward declaration candidates
- `typedef struct gltexture_s gltexture_t;` for transitional interfaces where pointer-only usage is enough.
- Opaque renderer handle typedefs in neutral headers (no GL naming).

### Current blockers
- `gl_model.h` is both structural model contract and GL-adjacent extension point.
- `gl_texmgr.h` embeds GL-native fields directly in `gltexture_t`.
- `quakedef.h` is consumed extremely broadly, so include surgery must be incremental and layer-split first.

## Phase 2 Baseline: `r_backend.c` GL-leak preparation

`r_backend.c` symbol usage from `glquake.h`:
- Direct usage: none identified.
- Dependency class:
  - neutralisierbar: include dependence is transitive via `quakedef.h`.
  - ref_gl-private: none in this file.
  - legacy fallback: none in this file.
  - unsicher verschiebbar: none needed for this TU.

Safe next step:
- After `quakedef.h` decontamination scaffolding exists, keep `r_backend.c` including only neutral headers (`r_framegraph.h`, `renderer_plugin.h`, `renderer_host_bridge.h`, `render_dispatch.h`) and avoid any reintroduction of GL headers.

## Phase 3 Baseline: `platform/gl_vidsdl.c` split plan snapshot

Host/platform responsibilities (keep in platform):
- SDL init/shutdown
- Window creation/destruction
- Display mode enumeration and switching
- Input focus/minimize/window title/query
- Surface/drawable size query
- Event integration hooks

ref_gl responsibilities (migrate into ref_gl path):
- GL context creation and current-context ownership
- GL proc loading bootstrap
- GL extension and version validation
- GL debug callback and markers
- GL state bootstrap (`GL_SetupState`, state masks)
- GL-specific swap interval policy
- VAO/FBO bootstrap and GL frame resources

Minimal host API needed by ref_gl (target):
- `void *vid_get_window(void)`
- `qboolean vid_make_context_current(void)`
- `qboolean vid_swap_buffers(void)`
- `qboolean vid_get_surface_info(out)`
- `qboolean vid_set_swap_interval(int)` (optional extension)

Planned migration slices:
1. Extract interface boundaries as no-op wrappers (no logic move yet).
2. Move GL context/bootstrap logic behind ref_gl bridge calls.
3. Leave pure SDL window/mode logic in platform.
4. Move GL state and debug callbacks out of platform.
5. Final cleanup: rename `gl_vidsdl.c` to host-centric naming once GL-free.
