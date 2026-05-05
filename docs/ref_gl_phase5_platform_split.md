# ref_gl Phase 5 Platform Split Plan (`gl_vidsdl.c`)

## Goal
Prepare a safe host/platform vs ref_gl split without removing legacy path in this phase.

## Function Ownership Mapping

### Host / Platform responsibilities (target)
- SDL window lifecycle
  - `VID_CreateWindowIfNeeded`
  - `VID_ApplyWindowMode`
  - `VID_SetWindowTitle`
  - `VID_GetWindow`
- display modes / monitor queries
  - `VID_InitModelist`
  - `VID_SDL2_GetDisplayMode`
  - `VID_GetCurrentWidth/Height/RefreshRate/BPP`
- input/event-facing helpers
  - focus/minimize helpers (`VID_HasMouseOrInputFocus`, `VID_IsMinimized`)
- drawable/swap wrappers (added in Phase 5)
  - `VID_GetDrawableSize`
  - `VID_SwapBuffers`
  - `VID_SetSwapInterval`
  - `VID_PlatformCreateGLContext` (transitional local helper)
  - `VID_PlatformMakeContextCurrent` (transitional local helper)
  - `VID_PlatformDeleteGLContext` (transitional local helper)
  - `VID_PlatformSetGLAttribute` / `VID_PlatformGetGLAttribute`
  - `VID_PlatformResetGLAttributes`

### ref_gl responsibilities (target)
- GL context creation/teardown
  - currently `VID_EnsureGLContext` + parts of `VID_Shutdown`
- GL proc loading and extension binding
  - `VID_GetGLProcAddress`, `GL_CheckExtensions`, `GL_Backend_SetProcAddressLoader`
- GL initial state/bootstrap
  - `GL_Init`, `GL_SetupState`, `GL_ResetState`
  - VAO bootstrap (`globalvao` setup)
- GL debug callback plumbing
  - `GL_DebugCallback`
- GL-specific swap interval/policy
  - `VID_ApplyVSync` (GL context dependent)

## Minimal Host API Needed by ref_gl
- `void *VID_GetWindow(void)`
- `void VID_GetDrawableSize(int *width, int *height)`
- `void VID_SwapBuffers(void)`
- `qboolean VID_EnsureGLContextCurrent(void)` (already present for plugin path)

## Phase-5 Safe Changes Implemented
- Added wrappers in `gl_vidsdl.c`:
  - `VID_GetDrawableSize`
  - `VID_SwapBuffers`
  - `VID_SetSwapInterval`
- Updated plugin-side swap callback to call host wrapper:
  - `R_Backend_SwapBuffers` now delegates to `VID_SwapBuffers`
- Updated plugin runtime resize path to use `VID_GetDrawableSize` wrapper.
- Centralized direct context ops in local platform helpers:
  - `SDL_GL_CreateContext` call-sites now route through `VID_PlatformCreateGLContext`
  - `SDL_GL_MakeCurrent` call-sites now route through `VID_PlatformMakeContextCurrent`
  - `SDL_GL_DeleteContext` call-site now routes through `VID_PlatformDeleteGLContext`
- Centralized GL attribute configuration/queries in local helpers:
  - context attribute setup (`major/minor/profile/debug`)
  - depth/stencil/sRGB attribute setup/query
  - context fallback path reset (`SDL_GL_ResetAttributes`)

## Deferred Split Steps (Phase 6+)
1. Move GL context create/destroy functions from `gl_vidsdl.c` into ref_gl-owned runtime unit.
2. Keep SDL window creation/event paths in platform unit; expose only host bridge functions.
3. Move GL debug callback registration to ref_gl init path.
4. Move swap-interval policy entirely behind backend callback contract.
5. Reduce `gl_vidsdl.c` include surface to SDL/platform headers only.

## Risks / Blockers
- `gl_vidsdl.c` currently owns both `viddef_t` updates and GL lifecycle; split must preserve resize/order assumptions.
- Context lifetime is intertwined with framebuffer/resource teardown (`GL_Delete*`/`GL_Create*`) sequencing.
- Existing fallback/compat paths rely on shared globals; require staged extraction, not one-shot move.
