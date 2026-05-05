# ref_gl Phase 5.5 Stabilization Audit

## Scope
- Audit baseline after Phase-5 decontamination prep.
- Internal legacy GL path remains intact by design.
- Stabilization pass executed with compile + smoke verification on 2026-05-05.

## 1) Core GL include removal status

### Removed from Core entry header
- `Quake/src/core/quakedef.h`
  - removed `SDL_opengl.h` include path(s)
  - removed `gl_model.h`
  - removed `gl_texmgr.h`
  - now uses:
    - `core/model_types.h`
    - `render/texture_handles.h`

### Neutral contract headers check
- `Quake/src/render/render_api.h`: no GL types, no GL headers.
- `Quake/src/render/renderer_plugin.h`: no GL types in ABI structs.
- `Quake/src/render/renderer_host_bridge.h`: no native GL handle types in bridge contract.

## 2) Remaining GL leaks

### A. Must be cleaned before legacy-path removal
1. `Quake/src/render/r_backend.c`
   - still contains explicit `-legacy_gl` startup path (`IW_RendererBuiltinGL_RegisterInternal`).
   - blocker for safe legacy-path deletion.
2. `Quake/src/platform/gl_vidsdl.c`
   - mixed ownership: SDL window + GL context/state + backend bootstrap.
   - legacy and plugin context assumptions share one unit.
3. `Quake/src/render/r_world.c`, `r_part.c`, `r_part_q3p.c`, `r_postfx.c`
   - direct native GL handles/GL calls in transitional non-`gl_*` units.
   - must be relocated or wrapped behind backend/resource services before final legacy removal.
4. `Quake/src/render/render_dispatch.c`
   - broad `Draw_*`/`GL_*` compatibility fanout still active.
   - required for compatibility now, but blocks final strict plugin-only contract.

### B. Can remain after ref_gl-only (ref_gl-private)
- `Quake/src/render/gl_texmgr.h/.c`
- `Quake/src/render/gl_model.h/.c`
- `Quake/src/render/gl_backend*.c`
- `Quake/src/render/gl_rmain.c`, `gl_shadow_runtime.c`, `gl_draw.c`, `gl_screen.c`, `gl_warp.c`, `gl_oit.c`

### C. Transitional / compat (accepted for now)
- `Quake/src/platform/gl_vidsdl.c` wrapper bridge while split is in progress.
- `Quake/src/render/render_dispatch.c` legacy entrypoint adapter.
- `Quake/src/render/renderer_host_bridge.h` legacy-named callbacks (`GL_Set2D`, etc.) as contract compatibility names.

### D. Harmless comment/name artifacts
- String literals like `"gl"`, `"ref_gl"`, comments containing `gl*` tokens.
- Identifier names without native GL API/type dependency by themselves.

## 3) gltexture_t Opaque Migration Plan

### Current direct users (high-signal)
- `r_world.c`, `r_alias.c`, `r_brush.c`, `r_decals.c`, `r_sprite.c`, `gl_draw.c`, `gl_model*.c`, `gl_rmain.c`, `gl_ktx2.c`.

### Fields commonly read
- `texnum`, `target`, `bindless_handle`, `internal_format`
- metadata: `width/height/depth`, `flags`, `name`, `visframe`, owner/source fields

### Fields commonly written
- texture creation/update path in `gl_texmgr.c` (owner of writes)
- selected runtime uploads in `gl_model*`/`gl_ktx2` initialization paths

### Safe accessor-first sequence (no struct break)
1. Add read-only accessors in `gl_texmgr` for native fields:
   - `TexMgr_GetNativeHandle`
   - `TexMgr_GetTarget`
   - `TexMgr_GetBindlessHandle`
   - `TexMgr_GetInternalFormat`
2. Replace read sites in transitional files (`r_world.c`, `r_part_q3p.c`) first.
3. Keep write authority in `gl_texmgr.c`.
4. Only then consider partial opacity.

## 4) platform/gl_vidsdl split progress

### Existing wrappers
- `VID_GetWindow()`
- `VID_GetDrawableSize()`
- `VID_SwapBuffers()`
- `VID_SetSwapInterval()`

### Still mixed in same unit
- SDL window lifecycle + display mode logic
- GL context creation/destruction
- GL init/state/bootstrap/debug callback
- backend frame integration hooks

### Missing for later clean split
- isolated context-create/destroy wrapper boundary callable from ref_gl runtime side
- isolated GL-proc-loader ownership handoff
- explicit host-only API surface document in platform header set

## 5) r_backend.c neutrality status
- Direct includes remain GL-free.
- Marker added: `R_BACKEND_MUST_REMAIN_GL_FREE`.
- No GL types in function signatures.
- Legacy policy still references OpenGL backend names and legacy toggle, which is expected for this phase.

## 6) Future ref_gl.dll mandatory switch checklist

Preconditions before making `ref_gl.dll` mandatory:
1. Remove/disable `-legacy_gl` internal registration path.
2. Ensure plugin ABI/version diagnostics are deterministic and early (already mostly present).
3. Ensure plugin discovery directories/messages are complete across platforms.
4. Ensure fallback policy is explicit:
   - plugin missing
   - plugin load failure
   - plugin ABI mismatch
   - plugin registered but no `OpenGL` backend
5. Remove remaining non-`gl_*` GL-handle ownership in transitional render units.
6. Finalize platform/context split so host no longer owns GL state bootstrap.
7. Keep compatibility adapter behavior documented until legacy entrypoints are retired in a dedicated phase.

## 7) Current plugin fallback policy (documented, unchanged)
- Plugin scan runs at backend init.
- If `-legacy_gl` is set, internal OpenGL backend registration path is still available.
- Without legacy fallback, missing/failing `ref_gl` path hard-fails startup with diagnostics.
- Runtime backend selection rejects non-ready/stub backends and reverts when possible.

## 8) Verification status (2026-05-05)
- `build.bat` executed successfully (`C:\Projects\ironwail\build.bat`).
- Solution build succeeded (`Release|x64`) and deploy completed to `C:\Quake\rerelease`.
- Timed smoke executed with:
  - `ironwail.exe -condebug -nosteamapi`
  - process started successfully and was stopped after timeout (5s).
