# Renderer Backend Migration — Phase 1 Safety Net

## Current State

Ironwail already has the first pieces needed for external renderer backends:

- `IRenderBackend` exists as the renderer backend vtable.
- The renderer plugin ABI exists.
- `ref_gl` exists as an external OpenGL renderer project.
- Builtin OpenGL remains available as the safe fallback path.
- Vulkan and DX12 exist only as blocked bring-up stubs.
- The framegraph exists, but execution still routes into legacy/OpenGL callbacks.

The engine/core is still OpenGL-contaminated through public headers and legacy renderer ownership. Phase 1 does **not** remove those dependencies; it makes the policy and diagnostics explicit so Phase 2 can proceed with less risk.

## New CVars

| CVar | Default | Purpose |
| --- | --- | --- |
| `r_backend_allow_builtin_gl` | `1` | Allows the current builtin OpenGL fallback when an external `ref_gl` plugin is not loaded. Default preserves historical behavior. |
| `r_backend_debug` | `0` | Enables backend-selection diagnostics with `R_Backend:` prefixes. |
| `r_renderer_migration_debug` | `0` | Enables migration-focused diagnostics with `R_Migration:` prefixes. |

Existing selection CVars keep their defaults:

- `r_backend` remains `gl`.
- `r_backend_api` remains `gl`.

Existing `r_refgl_*` debug CVars remain intact.

## Default Behavior

Default startup continues to use the existing OpenGL path:

1. External renderer plugins are not loaded unless requested by command line.
2. `r_backend_allow_builtin_gl` defaults to `1`.
3. Builtin OpenGL registers as the fallback backend.
4. Vulkan/DX12 stubs remain blocked from activation.

If `r_backend_allow_builtin_gl` is set to `0` and no external `ref_gl` backend is loaded, startup follows the existing clean error path with explicit warnings explaining that builtin OpenGL fallback is disabled.

## Known GL Leaks / Migration Blockers

### Core-Level

- `Quake/src/core/quakedef.h` includes `SDL_opengl.h`.
- `Quake/src/core/quakedef.h` includes `gl_model.h`.
- `Quake/src/core/quakedef.h` includes `gl_texmgr.h`.
- `Quake/src/core/model_types.h` is currently a shim over `gl_model.h`.

### Public GL Texture Leak

- `Quake/src/render/gl_texmgr.h` exposes `GLenum`, `GLuint`, and `GLuint64` in `gltexture_t`.
- `texture_handles.h` exists, but `gltexture_t` remains the bridge type.

### Legacy GL Orchestrator

- `Quake/src/render/gl_rmain.c` owns `R_RenderView`, FBOs, PostFX, SSAO, Bloom, Godrays, and readbacks.

### Framegraph / Legacy

- Framegraph declarations are increasingly declarative.
- Pass execution still calls legacy/OpenGL callbacks.
- `r_world`, `r_alias`, and `r_part` still rely on GL state baselines.

### Shader Leak

- `glprogs_t` stores `GLuint` program IDs.
- Shader metadata currently treats `shader_id` effectively as a GL program handle.

### Resource Leak

- Framegraph resource slots are still resolved to GL-native IDs.

## Non-Goals for Phase 1

- No Vulkan or DX12 implementation.
- No Vulkan or DX12 activation.
- No removal of OpenGL from `quakedef.h`.
- No removal of `glquake.h`, `gl_texmgr.h`, or `SDL_opengl.h`.
- No large split of `gl_rmain.c`.
- No draw-code migration.
- No shader, FBO, or texture refactor.
- No visible render-output change.

## Go / No-Go Criteria for Phase 2

GO only if:

1. Project compiles.
2. Default startup still uses the existing GL path.
3. `r_backend gl` remains the default.
4. `r_backend_api gl` remains the default.
5. Builtin GL fallback is allowed with `r_backend_allow_builtin_gl 1`.
6. `r_backend_allow_builtin_gl 0` produces a clear error when no external `ref_gl` backend is loaded.
7. Vulkan/DX12 stubs remain disabled/blocked.
8. No visible render output has changed.
9. No `glquake.h` / `gl_texmgr.h` / `quakedef.h` refactor has been performed.
10. `r_backend_migration_audit` lists the known GL migration blockers.

## Phase 2 Preview Only

### Phase 2-A

- Inventory the `quakedef.h` GL leak.
- Prepare a real neutral split for `model_types.h`.
- Count `gltexture_t` usage.
- Do not remove headers yet.

### Phase 2-B

- Choose the first small neutral texture-handle path.
- Candidate: PostFX LUT or isolated UI/2D texture.
- Do not start with world, alias, or shadow resources.

### Phase 2-C

- Prepare framegraph resource ownership.
- Long-term: move FBO lifetime ownership out of `gl_rmain.c`.
