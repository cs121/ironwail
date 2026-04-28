# Render Backend Bring-Up Readiness Checklist

Current baseline (April 24, 2026) uses strict external OpenGL plugin policy:
- Renderer plugins are scanned/loaded on every startup.
- `ref_gl.dll` is the required OpenGL renderer path.
- Built-in OpenGL fallback policy has been removed (`r_backend_allow_builtin_gl` removed).

This checklist is the Phase 3 baseline for bringing up a second renderer backend (stubbed or full) without pass-graph orchestration changes.

## 1) Registration + selection contract

- [x] Backend registers with a unique `IRenderBackend.name`.
- [x] `get_caps`, `resolve_resource_id`, `is_resource_valid`, `bind_render_target`, `set_viewport`, `draw`, and `populate_framegraph_resources` are implemented (required contract).
- [x] `can_activate` reports startup/runtime viability correctly.
- [x] Backend can be selected via `r_backend`.

## 2) Pass execution contract

- [x] New backend either implements per-pass callbacks (`pass_setup_view`, `pass_render_scene`, etc.) **or** explicitly opts into legacy fallback behavior with a validated `begin_pass`/`end_pass` compatibility bridge.
- [x] `begin_pass_ex`/`end_pass_ex` are wired (or legacy `begin_pass`/`end_pass` are intentionally used as a compatibility bridge).
- [x] `resource_barrier` handles shader-write/read hazards where relevant.

## 3) Resource declarations + validation

- [x] All `RenderPassDesc` read/write resource bits are mapped in framegraph resource mapping.
- [x] Attachment resources are backend-resolvable and also declared in pass `writes` masks.
- [x] Non-backend handoff resources (state-only edges like fog handoff) stay declared in read/write masks for ordering, even when they do not resolve to backend objects.

## 4) Wrapper migration priority (OpenGL debt burn-down)

- [x] `R_Backend_SetPipelineState` high-frequency callsites reduced in favor of `bind_pipeline` + `set_dynamic_state`.
- [x] Direct `glDraw*`/`GL_Draw*` hot paths migrated to `R_Backend_Draw` + descriptor binding.
- [x] Per-pass texture/program binding logic migrated to `bind_descriptors`-style data flow.
- [x] Compute-capable paths use `R_Backend_Dispatch` and explicit barriers.

## 5) Smoke + diagnostics

- [x] `r_backend_wrapper_audit` command prints migration priorities.
- [x] Framegraph debug validation (`r_framegraph_debug`, `r_gl_state_validate`) reports no attachment/resource declaration warnings during startup smoke.
- [x] Frame launches and scene render complete with expected pass timings.
- [x] `r_backend_api help` reflects final registration state after plugin loading (`gl=implemented` when `ref_gl` is loaded).
- [x] Build-topology guardrail script exists: `python/check_renderer_topology.py`.

## 6) TODOs for `ref_gl` autonomous extraction

- [x] `Quake/r_backend.c`: `resource_services.register_external_resource` now uses a host-side external resource registry with stable assigned IDs and duplicate-handle dedup.
- [x] `Quake/r_backend.c`: replaced framecount upload shim with host-managed transient upload epochs + liveness tracking tied to external resource registry lifetime.
- [x] `Quake/r_backend.c` + `Quake/gl_shaders.c`: added shader metadata query and host pipeline metadata registry used by `pipeline_services.get_shader_metadata` / `get_pipeline_metadata`.
- [x] `Quake/ref_gl_plugin.c`: uses direct `register_backend` with strict ABI checks (`host_api >= V4`) and entry-point registration.
- [x] `Quake/r_backend.c`: plugin loading no longer depends on `r_refgl_debug`; debug cvar now only enables verbose plugin-load diagnostics.
- [x] `Quake/render_dispatch.c` + host call sites: dispatch is now plugin-entrypoint-first without internal renderer function fallback table.
- [x] `python/check_no_direct_gl_draw_calls.py` and `python/check_no_legacy_pipeline_state_calls.py`: scan root fixed to recursive `Quake/src` traversal.
- [x] `Quake/r_framegraph.c` + `Quake/r_backend.c`: resource-bit to backend-slot mapping moved behind `R_Backend_GetFrameGraphResourceBinding()` adapter to reduce framegraph-side renderer-global assumptions.
- [x] `Quake/gl_backend_runtime.c`: GL proc-loader and cached fixed-function state helpers split out of `gl_backend.c` so backend interface code and runtime utility code are decoupled.
- [x] `Quake/gl_backend_resources.c`: GL resource registry split from `gl_backend.c`; Visual Studio host target no longer compiles `gl_backend.c` (backend interface stays plugin-only).
- [x] `Quake/gl_vidsdl.c`: framebuffer/DRS/godrays reset hooks now go through renderer dispatch entrypoints in host build (`R_CreateFrameBuffers`, `R_DeleteFrameBuffers`, `R_ResetDRSState`, `R_ResetGodraysStabilization`) instead of direct renderer symbol calls.

## 7) Migration closeout status (external `ref_gl.dll`)

- [x] Loader policy: hard-fail startup if required external OpenGL plugin is unavailable.
- [x] Hard-fail diagnostics now distinguish:
  plugin file not found vs plugin discovered but load/registration failed vs plugin loaded without `OpenGL` backend registration.
- [x] Runtime diagnostics: `r_backend_api help` reflects final post-registration state (`gl=implemented` observed in Debug smokes).
- [x] Dispatch activation: host-critical renderer entrypoints (`R_Init`, `R_RenderView`, `R_NewMap`) run through plugin dispatch.
- [x] Negative-path runtime check (April 24, 2026): with `ref_gl.dll` removed, startup emits deterministic warning including searched directories before fatal exit.
- [ ] Build-system convergence:
  Visual Studio path validated (Debug x64 build + normal and `-refgl_debug` smoke passed on April 24, 2026).
  CMake plugin topology moved to `ref_gl` module target with plugin implementation units removed from host source list.
  Linux/MinGW Makefiles switched to `ref_gl` plugin target and no longer reference legacy `renderer_builtin_gl_plugin`.
  Remaining work: full cross-build runtime validation + removing internal GL renderer units from host targets.
