# Render Backend Bring-Up Readiness Checklist

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

- [ ] `Quake/r_backend.c`: implement `resource_services.register_external_resource` against a backend-owned handle registry (currently explicit stub + warning).
- [ ] `Quake/r_backend.c`: replace temporary upload epoch shim in `upload_services.query_upload_epoch` / `is_transient_resource_alive` with real transient allocator lifetime tracking.
- [ ] `Quake/r_backend.c` + `Quake/gl_shaders.c`: add a backend-neutral shader/pipeline metadata registry for `pipeline_services.get_shader_metadata` and `get_pipeline_metadata`.
- [ ] `Quake/ref_gl_plugin.c`: switch from `builtin_opengl_backend` handoff to a self-owned backend implementation once host services are sufficient.
- [ ] `Quake/r_framegraph.c`: keep pass data production backend-neutral; continue moving any remaining renderer-global assumptions behind backend service adapters.
