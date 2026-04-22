# Render Backend Bring-Up Readiness Checklist

Current baseline assumes `ref_gl` remains the only registered backend.

This checklist is the Phase 3 baseline for bringing up a second renderer backend (stubbed or full) without pass-graph orchestration changes.

## 1) Registration + selection contract

- [ ] Backend registers with a unique `IRenderBackend.name`.
- [ ] `get_caps`, `resolve_resource_id`, `is_resource_valid`, `bind_render_target`, `set_viewport`, `draw`, and `populate_framegraph_resources` are implemented (required contract).
- [ ] `can_activate` reports startup/runtime viability correctly.
- [ ] Backend can be selected via `r_backend`.

## 2) Pass execution contract

- [ ] New backend either implements per-pass callbacks (`pass_setup_view`, `pass_render_scene`, etc.) **or** explicitly opts into legacy fallback behavior with `RenderBackendCaps.supports_legacy_pass_fallbacks`.
- [ ] `begin_pass_ex`/`end_pass_ex` are wired (or legacy `begin_pass`/`end_pass` are intentionally used as a compatibility bridge).
- [ ] `resource_barrier` handles shader-write/read hazards where relevant.

## 3) Resource declarations + validation

- [ ] All `RenderPassDesc` read/write resource bits are mapped in framegraph resource mapping.
- [ ] Attachment resources are backend-resolvable and also declared in pass `writes` masks.
- [ ] Non-backend handoff resources (state-only edges like fog handoff) stay declared in read/write masks for ordering, even when they do not resolve to backend objects.

## 4) Wrapper migration priority (OpenGL debt burn-down)

- [ ] `R_Backend_SetPipelineState` high-frequency callsites reduced in favor of `bind_pipeline` + `set_dynamic_state`.
- [ ] Direct `glDraw*`/`GL_Draw*` hot paths migrated to `R_Backend_Draw` + descriptor binding.
- [ ] Per-pass texture/program binding logic migrated to `bind_descriptors`-style data flow.
- [ ] Compute-capable paths use `R_Backend_Dispatch` and explicit barriers.

## 5) Smoke + diagnostics

- [ ] `r_backend_wrapper_audit` command prints migration priorities.
- [ ] Framegraph debug validation (`r_framegraph_debug`, `r_gl_state_validate`) reports no attachment/resource declaration warnings.
- [ ] Frame launches and scene render complete with expected pass timings.

## 6) TODOs for `ref_gl` autonomous extraction

- [x] `Quake/r_backend.c`: `resource_services.register_external_resource` now uses a host-side external resource registry with stable assigned IDs and duplicate-handle dedup.
- [x] `Quake/r_backend.c`: replaced framecount upload shim with host-managed transient upload epochs + liveness tracking tied to external resource registry lifetime.
- [x] `Quake/r_backend.c` + `Quake/gl_shaders.c`: added shader metadata query and host pipeline metadata registry used by `pipeline_services.get_shader_metadata` / `get_pipeline_metadata`.
- [x] `Quake/ref_gl_plugin.c`: switched to direct `register_backend` using `GL_Backend_GetInterface()`; host now treats external `ref_gl` as primary path, with optional built-in fallback via `r_backend_allow_builtin_gl`.
- [ ] `Quake/r_framegraph.c`: keep pass data production backend-neutral; continue moving any remaining renderer-global assumptions behind backend service adapters.
