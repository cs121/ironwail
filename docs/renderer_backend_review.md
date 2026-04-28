# Ironwail Renderer Backend Review (OpenGL -> Multi-backend Readiness)

Reviewed against the current framegraph/backend split and the still-GL-heavy execution path.

## 1. Executive summary

- The backend is **mixed quality**: there is a real backend interface and pass scheduler, but most rendering work still executes through OpenGL-centric global state and direct `gl*` calls in high-level passes.
- The current architecture is **not yet suitable** for a robust Vulkan/DX12 backend without staged refactoring.
- The most important observation: the codebase is in an **intermediate migration state** (framegraph + backend interface added, but legacy GL execution path still dominant).
- There is now a disabled emissive proxy-light collector in `r_realtimelight.c`, but it does not change the main backend conclusion.

## 2. Renderer architecture map

### Frontend / frame planning / pass registration
- `R_RenderView()` calls `R_FrameGraph_RenderView()`. (`Quake/gl_rmain.c`)
- Per-frame pass list is rebuilt dynamically via `R_RegisterFrameGraphPasses()` and `R_FrameGraph_AddPass()`. (`Quake/r_framegraph.c`, `Quake/r_passes.c`)
- Pass descriptors declare reads/writes and output/viewport policies. (`Quake/r_passes.c`)

### Pass scheduler / dependency model
- Setup-stage and main-stage passes are split and topologically reordered from declared read/write masks. (`Quake/r_framegraph.c`)
- Pruning keeps only passes that contribute to required resources or side-effects. (`Quake/r_framegraph.c`)
- Resource barriers are inferred from coarse resource states tracked per resource bit. (`Quake/r_framegraph.c`)

### Backend abstraction seam
- `IRenderBackend` defines backend hooks (pass begin/end, barrier, draw/dispatch, pipeline state, descriptors, pass callbacks, resource translation). (`Quake/r_framegraph.h`)
- Runtime backend selection and contract validation are implemented (`r_backend` cvar, registration/selection path). (`Quake/r_backend.c`)
- Only OpenGL backend is registered in `R_Backend_Init()` via `GL_Backend_Register()`. (`Quake/r_backend.c`)

### OpenGL backend implementation
- OpenGL backend translates framegraph resources to native GL IDs and exposes compatibility hooks (viewport/scissor/state/draw). (`Quake/gl_backend.c`)
- Pass callbacks (`pass_render_scene`, `pass_postprocess`, etc.) call legacy GL-heavy renderer functions directly. (`Quake/gl_backend.c`)

### Platform / context / present
- SDL window/context lifecycle is in `gl_vidsdl.c`; render target recreation is triggered there (`VID_RecreateRenderTargets`). (`Quake/gl_vidsdl.c`)
- Present path is still direct SDL swap (`SDL_GL_SwapWindow`). (`Quake/gl_vidsdl.c`)

### Ownership snapshot
- **What to draw**: mostly legacy scene/brush/alias systems.
- **How to draw**: mostly pass-local GL code (program bind + texture slots + mutable global state).
- **GPU resources**: global structs (`framebufs`, texture globals, program globals).
- **Render state**: global cached `glstate` + ad hoc local mutations.
- **Pass sequencing**: framegraph order + side-effect pruning logic.
- **Shader bindings**: mostly explicit fixed slots / uniform locations in pass code.

## 3. API coupling findings (OpenGL entanglement)

### Major blockers
1. **Direct OpenGL calls in high-level render passes**  
   - `glBindFramebuffer`, `glReadBuffer`, `glDrawBuffer`, `glBlitFramebuffer`, `glViewport` inside `R_WarpScaleView`. (`Quake/gl_rmain.c`)  
   - `glDrawElements` directly used in decals path. (`Quake/r_decals.c`)  
   - Risk: backend abstraction is bypassed; Vulkan/DX12 cannot reuse these paths.

2. **GL state-machine assumptions leak into gameplay-facing draw code**  
   - Many paths do `GL_UseProgram` + `R_Backend_SetPipelineState` + direct texture-unit binds in sequence. (`Quake/r_world.c`, `Quake/r_alias.c`, `Quake/r_part.c`)  
   - Risk: implicit ordering dependencies, hard to encode into explicit command buffers.

3. **Framebuffer model is GL/FBO-centric**  
   - Pass output targets map to specific FBO slots (`SCENE_FBO`, `COMPOSITE_FBO`, etc.). (`Quake/r_framegraph.h`, `Quake/gl_backend.c`)  
   - Risk: explicit APIs need attachment/load/store and transitions per subpass/pass, not global FBO rebinding semantics.

4. **OpenGL enum types and concepts leak across layers**  
   - Internal pass code manipulates GL depth funcs, blend funcs, texture targets, draw buffers directly. (`Quake/r_world.c`, `Quake/gl_rmain.c`)  
   - Risk: portability friction and difficult testability.

### Moderate risks
5. **Legacy fallback path keeps high-level logic GL-dependent**  
   - `supports_legacy_pass_fallbacks` allows calling legacy GL implementations if backend-specific pass hook missing. (`Quake/r_passes.c`, `Quake/gl_backend.c`)  
   - Risk: helps migration now, but delays true backend decoupling.

6. **Resource references are opaque IDs over GL objects, but semantic model is thin**  
   - Framegraph resources mostly map to existing GL handles with no richer metadata beyond slot/type/lifetime. (`Quake/gl_backend.c`, `Quake/r_framegraph.h`)  
   - Risk: insufficient for explicit APIs requiring usage, queue ownership, and transition scopes.

## 4. State-management findings

### Current model
- Core state cache is `glstate`, mutated through `GL_SetStateEx` and exposed via `GL_SetState`. (`Quake/gl_vidsdl.c`)
- Program binding cache is `gl_current_program` with `GL_UseProgram` wrapper. (`Quake/gl_shaders.c`)
- Texture binding cache is per-texture-unit `currenttexture[]` with `GL_BindNative`. (`Quake/gl_texmgr.c`)

### Issues
1. **State ownership is distributed, not centralized per pass**  
   - Passes set state locally; no strict pass baseline object beyond ad hoc reset in frame begin. (`Quake/gl_vidsdl.c`, `Quake/r_world.c`)
2. **Mixed cached + raw GL mutation can desync cache**  
   - Many direct GL calls bypass wrappers (`glDrawElements`, direct FBO/viewport/read/draw buffer ops). (`Quake/r_decals.c`, `Quake/gl_rmain.c`)
3. **Global restore patterns are brittle**  
   - Q3P particles save/restore entire `glstate`. (`Quake/r_part_q3p.c`)  
   - Risk: restoring bitmask does not restore all non-modeled state (e.g., depth func, stencil, draw buffers).
4. **Non-modeled state is frequently mutated ad hoc**  
   - `glDepthFunc`, stencil ops, polygon offset, draw/read buffer state are modified in pass bodies. (`Quake/r_world.c`, `Quake/gl_oit.c`, `Quake/gl_rmain.c`)

## 5. Resource ownership/lifetime findings

### Strengths
- Framegraph resource handles introduce a typed indirection layer (resource ref + registry). (`Quake/r_framegraph.h`)
- RT recreation is centralized in `VID_RecreateRenderTargets()`. (`Quake/gl_vidsdl.c`)

### Risks / blockers
1. **Resource lifetime is still mostly implicit global lifetime**  
   - `framebufs` is globally shared and read from many systems. (`Quake/gl_rmain.c`, `Quake/gl_backend.c`)
2. **Recreate/update coupling crosses platform and renderer layers**  
   - SDL/video callbacks trigger renderer RT teardown/create directly. (`Quake/gl_vidsdl.c`)
3. **No explicit device-loss model**  
   - Context loss assumptions are minimal; explicit APIs would require robust recreate and descriptor invalidation strategy.
4. **Transient uploads are GL convenience-oriented**  
   - Widespread `GL_Upload`/`GL_ReserveDeviceMemory` temporary usage with immediate draw/dispatch consumption. (`Quake/r_world.c`, `Quake/r_alias.c`, `Quake/r_decals.c`)  
   - Needs ring-buffer fencing model for Vulkan/DX12.

## 6. Render-pass and submission findings

### Pass structure
- Pass declarations are explicit and readable (`Setup view`, `Shadow maps`, `Render scene`, `Warp/resolve`, `Postprocess`, overlays). (`Quake/r_passes.c`)
- Decals update has its own framegraph pass writing `RENDER_RES_DECALS`. (`Quake/r_decals.c`)

### Weaknesses
1. **Pass contracts are only partially enforceable**
   - Scheduler tracks coarse resource bits, but shader resources, depth func changes, stencil behavior, and draw-buffer side effects are not represented.
2. **Pass execution still immediate mode**
   - Pass bodies issue GL commands directly rather than recording backend-agnostic draw packets.
3. **Barrier model is too coarse for explicit APIs**
   - GL backend emits memory barrier only for shader read/write heuristics; no color/depth transition semantics. (`Quake/gl_backend.c`)
4. **Topological ordering can hide missing dependency declarations**
   - Because many side effects are not modeled as resources, ordering still relies on implicit behavior.

## 7. Shader/binding findings

### Positives
- GLSL uses explicit `layout(binding=...)` and many explicit uniform locations. (`Quake/shaders/world.frag`, `Quake/shaders/ssao.frag`)
- There is a frame data UBO/SSBO pattern used in several paths (`GL_BindBufferRange` to fixed slots). (`Quake/gl_rmain.c`, `Quake/r_world.c`)

### Risks
1. **Binding contracts are spread across C and GLSL without central schema**
   - Hardcoded texture units and SSBO/UBO binding slots are repeated in multiple paths.
2. **High volume of direct `GL_Uniform*` traffic**
   - Post/SSAO/godrays and particle paths set many scalar uniforms directly per pass/draw. (`Quake/gl_rmain.c`, `Quake/r_part_q3p.c`)
3. **Program/slot assumptions are brittle**
   - Some paths require exact unit reservations (e.g., alias path explicitly unbinds texture6). (`Quake/r_alias.c`)
4. **Descriptor-style abstraction is not yet in place**
   - `R_Backend_BindDescriptors` exists in interface but is mostly unused by current pass code. (`Quake/r_framegraph.h`, `Quake/r_backend.c`)

## 8. Bug and fragility findings

1. **Potential state leak via non-modeled state**
   - `R_Backend_SetPipelineState` does not cover depth func/stencil/read-draw-buffer, but passes mutate those directly.
2. **Duplicate/messy control flow in world pass code**
   - Repeated `GL_UseProgram(program)` and mixed indentation in `R_DrawBrushModels` region increases regression risk. (`Quake/r_world.c`)
3. **Legacy + framegraph duality can mask invalid dependencies**
   - Some pass side effects are declared via `side_effects`, but non-resource dependencies are implicit.
4. **GL-only behavior can hide explicit-API hazards**
   - GL permits many ordering/state shortcuts that Vulkan/DX12 would require explicit barriers/transitions for.

## 9. Vulkan readiness assessment

### Reusable today
- Framegraph pass descriptors and scheduling skeleton.
- Backend registry/selection framework.
- Some shader binding explicitness (`layout(binding=...)`) and typed resource refs.

### Must refactor first
1. Remove direct GL calls from pass bodies (route through backend command encoder).
2. Introduce explicit pass-local state baselines (depth/blend/stencil/raster/viewport/scissor).
3. Replace global texture-unit/program binding conventions with descriptor-set style binding plans.
4. Replace coarse barrier model with real resource-usage transitions and attachment states.
5. Move transient upload model to frame-allocator + fence timeline semantics.

### Verdict (Vulkan)
- Feasible incrementally, but only **after cleanup under GL backend**.  
- Current code is not a safe direct launch point for a production Vulkan backend.

## 10. DX12 readiness assessment

### Reusable today
- Same as Vulkan: pass scheduler + backend interface skeleton.

### Additional DX12 pressure points
- Root signature / descriptor heap management would expose today’s scattered bindings quickly.
- Explicit PSO model clashes with dynamic `glstate` mutation style.
- Command list recording requires stronger separation between scene collection and backend execution.

### Verdict (DX12)
- Slightly harder than Vulkan given current architecture because state/binding centralization is still immature.

## 11. Recommended phased refactor plan

### Phase 0: Instrumentation and invariants
- Targets: `r_framegraph.c`, `gl_backend.c`, `gl_vidsdl.c`
- Add validation: pass baseline checks, state leak reporting, resource transition debug traces.
- Smoke: existing startup + map-load + postprocess toggles.

### Phase 1: Isolate raw GL calls behind backend wrappers
- Targets: `gl_rmain.c`, `r_world.c`, `r_alias.c`, `r_decals.c`, `r_part*.c`, `gl_oit.c`
- Replace direct framebuffer/draw/state calls with backend wrapper equivalents.
- Keep OpenGL behavior identical.

#### Phase-1 migration note (April 20, 2026)
- Completed wrapper migration for direct state mutations in:
  - `Quake/src/render/gl_rmain.c`
  - `Quake/src/render/gl_sky.c`
  - `Quake/src/render/gl_oit.c`
  - `Quake/src/render/gl_shadow_runtime.c`
- Added cached backend wrappers for viewport/color/depth/stencil state in `gl_backend` to reduce redundant driver calls.
- Remaining intentional exceptions (not migrated yet) are direct framebuffer attachment/read-draw-buffer operations that still depend on legacy FBO plumbing semantics; these are deferred until pass-local render-target contracts are fully explicit.

### Phase 2: Pass baselines and explicit state objects
- Targets: `r_framegraph.h/c`, backend interface, `gl_backend.c`
- Introduce pass baseline descriptors (depth/stencil/blend/raster/viewport/scissor).
- Enforce pass begin sets full baseline.

### Phase 3: Resource usage contracts
- Targets: `r_framegraph.h/c`, `r_passes.c`, `gl_backend.c`
- Expand resource model to include usage/access and attachment semantics.
- Track read/write usages beyond coarse bitmasks.

### Phase 4: Draw packet abstraction
- Targets: world/alias/particle/decal submission paths
- Split collect/sort/build from execute; emit backend-agnostic draw packets.
- Backend translates packets to API-specific command encoding.

### Phase 5: Shader/binding contract consolidation
- Targets: `gl_shaders.c`, shader includes, render pass code
- Centralize binding layouts per pass/material domain.
- Migrate ad hoc uniforms toward structured buffers/push-constant equivalents.

### Phase 6: Backend command encoder interface
- Targets: `IRenderBackend` in `r_framegraph.h`, concrete GL backend
- Add command list style API (begin pass, bind pipeline set, bind descriptors, draw/dispatch).
- Implement with GL first as proof.

### Phase 7: Prototype secondary backend bootstrap
- Start with minimal path: clear + scene depth/color + present.
- Keep GL backend as reference and fallback.

## 12. Top risks

1. Hidden state coupling across passes.
2. Resource transition under-modeling.
3. Global mutable renderer data (`framebufs`, `glstate`) used everywhere.
4. Shader binding scatter and slot collisions.
5. Immediate-mode draw calls bypassing backend interface.
6. Refactor churn in historically complex world/material paths.

## 13. Final verdict

1. Architecture health: **mixed** (promising scaffolding, heavy GL entanglement).
2. Suitable today for Vulkan/DX12 addition: **no, not safely**.
3. Top weaknesses: GL leakage, global state model, weak resource model, scattered bindings, immediate submission.
4. Top improvements: isolate GL calls, pass baselines, robust resource usage model, draw packetization, binding contract centralization.
5. Blockers vs cosmetic: direct GL calls + state/resource implicitness are blockers; formatting/duplication issues are secondary.
6. Must-fix before serious Vulkan/DX12: state ownership, resource transitions, pass contracts, submission abstraction.
7. Incremental path viability: **yes**, if GL remains first-class during phased migration.
8. Confidence: **medium-high** for architecture-level findings; **medium** for runtime-correctness edge cases without exhaustive runtime tracing.

## 14. ref_gl.dll status after remediation (April 24, 2026)

### Resolved in this remediation
1. Loader policy is now strict external-plugin-first with hard failure when OpenGL plugin registration is missing; debug cvar no longer gates plugin scanning.
2. Built-in OpenGL fallback contract was removed from plugin host API (ABI major bumped to 5); legacy built-in registration callbacks were removed from host/plugin interfaces.
3. Host callsites for critical render entrypoints were moved to `g_rend` dispatch (`R_Init`, `R_RenderView`, `R_NewMap`, plus additional skin/particle/decal-related callsites).
4. Dispatch initialization was hardened to plugin-only entrypoint tables (no internal renderer fallback table in `render_dispatch`).
5. Runtime diagnostics were validated in Debug x64 smoke runs (normal + `-refgl_debug`): plugin load lines and `r_backend_api help: gl=implemented, vulkan=experimental, dx12=experimental` are consistent.
6. Guardrail scripts (`check_no_direct_gl_draw_calls.py`, `check_no_legacy_pipeline_state_calls.py`) were aligned to explicit scan-root traversal under `Quake/src`.
7. Legacy builtin backend export function (`IW_RendererPlugin_GetBuiltinOpenGLBackend`) was removed from GL backend code.
8. Startup hard-fail diagnostics were hardened to persist in `qconsole.log` via warning-first emission before fatal exit paths.
9. `gl_backend.c` was split so GL proc-loader + cached fixed-function runtime helpers now live in `gl_backend_runtime.c` (reduced coupling in backend interface unit).
10. `gl_backend_resources.c` now owns GL resource registry functions; `ironwail.vcxproj` no longer compiles `gl_backend.c` (plugin interface implementation moved one step closer to plugin-only ownership).
11. `gl_vidsdl.c` host path now routes framebuffer recreation and DRS/godrays reset through dispatch entrypoints, removing direct host calls to those renderer implementation symbols.

### Verification performed
1. Multiple Debug x64 solution builds succeeded after remediation updates.
2. Per-phase smoke tests were executed repeatedly from `C:\Quake\rerelease`:
   normal: `-condebug -nosteamapi` (5s)
   debug plugin path: `-condebug -nosteamapi -refgl_debug` (5s)
3. Observed logs show successful plugin loads (`ref_gl`, `ref_vk`, `ref_dx12`) and no new assert/crash signatures in startup smoke.
4. Static migration guardrails passed:
   `python/check_gl_symbol_boundaries.py`
   `python/check_no_raw_gl_calls.py`
   `python/check_no_direct_gl_draw_calls.py`
   `python/check_no_legacy_pipeline_state_calls.py`
   `python/check_renderer_topology.py`
5. Negative-path runtime check (April 24, 2026): temporarily removing `C:\Quake\rerelease\ref_gl.dll` produced deterministic startup warning:
   missing required plugin + searched directories, followed by hard failure.

### Remaining open migration topics
1. Host build graph still compiles internal OpenGL renderer units in Visual Studio target (`ironwail.vcxproj`), so full binary-level externalization is not yet complete.
2. Build-system convergence is still incomplete in validation terms: CMake + Makefile topology is migrated to `ref_gl`, but cross-build runtime verification (Linux/MinGW) is still pending.
3. Platform/video ownership split is still partial; `gl_vidsdl.c` remains shared and continues to hold GL lifecycle responsibilities that should be fully renderer-module-owned in final architecture.
4. Renderer core still contains GL-centric pass/state logic that blocks full explicit-backend neutrality.

### Risk / regression assessment
1. Startup behavior is now stricter and clearer: if required OpenGL plugin registration is absent, host fails early with explicit diagnostics.
2. Dispatch-path regressions are reduced by explicit host-side null checks and hard-fail guards on required entrypoints.
3. Main residual risk is architectural, not immediate runtime stability: host/build topology still allows internal renderer code paths to exist in host binaries.

### Recommended next steps
1. Complete host build-graph extraction: remove renderer implementation units from `ironwail` host targets and leave them only in `ref_gl`.
2. Run full CMake/Linux/MinGW build + runtime smoke validation to close cross-build-system verification.
3. Add negative-path runtime matrix to CI (`ref_gl.dll` missing, bad ABI, failed registration) to enforce deterministic hard-fail diagnostics.
