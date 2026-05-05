# ref_gl Phase 5 Core Decontamination

## Scope
- Phase 5 focus: remove GL header/type dependencies from Engine/Core entry headers without changing renderer behavior.
- No compile test and no smoke test executed in this phase.

## Removed GL Includes
- `Quake/src/core/quakedef.h`
  - removed `SDL_opengl.h` (`<SDL2/SDL_opengl.h>` and `"SDL_opengl.h"`)
  - removed `gl_model.h`
  - removed `gl_texmgr.h`
  - added neutral replacements:
    - `core/model_types.h`
    - `render/texture_handles.h`

## New Neutral Headers
- `Quake/src/core/model_types.h`
  - extracted/hosted legacy model-facing type declarations previously reachable through `gl_model.h`
  - contains no GL headers and uses forward declaration `struct gltexture_s` only
  - marked with `TODO_CORE_DECONTAMINATION` for later tightening
- `Quake/src/render/texture_handles.h`
  - neutral texture handle typedefs (`render_texture_handle_t`, `render_texture_bindless_handle_t`)
  - forward declaration of `gltexture_t` only

## Compatibility Shims
- `Quake/src/render/gl_model.h`
  - converted to compatibility wrapper over `core/model_types.h`
  - legacy in-header body retained behind `#if 0` for controlled migration reference

## r_backend Neutrality Check
- `Quake/src/render/r_backend.c`
  - remains GL-header-free in direct includes
  - still uses neutral renderer contract headers (`render_api.h`, `renderer_plugin.h`, `renderer_host_bridge.h`, `r_framegraph.h` transitively)

## Remaining GL Leaks (Phase-5 accepted)
- `Quake/src/platform/gl_vidsdl.c`
  - direct GL usage (`GLuint`, `GLenum`, context/bootstrap calls) remains by design in platform+GL transitional unit
  - this file is explicitly Phase-5 split candidate, not fully migrated here
- `core/model_types.h`
  - model structs still contain legacy fields such as `meshvbo`/`meshindexesvbo` (`unsigned int` native IDs without GL typedef)
  - ownership cleanup deferred to later phase (resource boundary tightening)

## Blockers
- Full `gltexture_t` opacity is still blocked by broad read access across render units.
- `gl_vidsdl.c` still mixes host window lifecycle and GL context/state lifecycle.
- `model_types.h` is intentionally broad to preserve build stability; fine-grained split is deferred.

## Risk Assessment
- Low/medium:
  - `quakedef.h` is now GL-header-free, reducing transitive GL contamination.
  - compatibility risk exists because model declarations were relocated; shim keeps legacy include path stable.
- Medium:
  - deeper ownership cleanup still required for strict renderer-agnostic core semantics.

## Logical Validation (no compile)
- `quakedef.h` no longer references GL headers or `gl_model.h`/`gl_texmgr.h`.
- Core header path now reaches model/texture declarations through neutral headers.
- No renderer ABI struct layout changes were introduced in `render_api.h` / `renderer_plugin.h`.
