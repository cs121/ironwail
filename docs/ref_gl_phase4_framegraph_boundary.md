# ref_gl Phase 4 Framegraph Boundary

Stand: 2026-05-05

## Pass Inventory (registered from core)

Source:
- `R_RegisterFrameGraphPasses()` in `Quake/src/render/r_passes.c`
- `R_Decals_RegisterFrameGraphPasses()` in `Quake/src/render/r_decals.c`

| Pass | Reads | Writes | Clears / Attachments | Depth/Color/Resolve | View/Scene data | Execute path | GL dependency | Implizite State-Abhängigkeiten | Zielzustand | Risiko |
|---|---|---|---|---|---|---|---|---|---|---|
| Setup view | none | none | none | none | camera/refdef/global view build | `R_Pass_SetupView -> backend->pass_setup_view -> R_SetupView` | indirekt hoch | erwartet baseline reset + valid viewport convention | core-deklarativ + backend-owned exec | mittel |
| Shadow maps | none | `SHADOW_SUN_DEPTH` | depth attachment clear/store | separate shadow depth FBO | frusta, lights, shadow cvars | `R_Pass_RenderShadowMaps -> GLBackend_PassShadowMaps -> R_RenderShadowMaps` | hoch | reverse-Z/clip-control toggle + depth func/mask assumptions | backend-owned | hoch |
| Render scene | `DECALS` | `SCENE_COLOR`, `SCENE_DEPTH`, `VELOCITY` | 2 color clear/store + depth clear/store | scene FBO render target | view rect, scene scale, world/entity/light globals | `R_Pass_RenderScene -> GLBackend_PassRenderScene -> R_RenderScene` | hoch | relies on baseline + transitional `r_world/r_alias/r_part` GL state | core-deklarativ; execution backend-owned but transitional | hoch |
| Warp/resolve | `SCENE_COLOR`, `SCENE_DEPTH` | `COMPOSITE_COLOR`, `COMPOSITE_DEPTH` | composite color/depth clear/store | resolve/blit/composite path | scene size + view rect + postfx flags | `R_Pass_WarpResolve -> GLBackend_PassWarpResolve -> R_WarpScaleView` | hoch | framebuffer and texture-unit assumptions across resolve path | backend-owned | hoch |
| Update decals | none | `DECALS` | none | none | cpu sim/update only | `R_Decals_ExecFrameGraphPass -> R_UpdateDecals` | niedrig | no explicit GL, but consumed by scene pass | core-deklarativ (CPU sidefx) | niedrig |
| Capture fog handoff | none | `SSAO_FOG_STATE` | none | none | global fog snapshot for SSAO gating | `R_Pass_CaptureSSAOFogHandoff -> R_CaptureSSAOFogHandoffState` | indirekt mittel | depends on fog having been updated in scene path | core-deklarativ (side effect) | mittel |
| Postprocess | `COMPOSITE_COLOR`, `COMPOSITE_DEPTH`, `SSAO_FOG_STATE` | `COMPOSITE_COLOR` | color attachment load/store | fullscreen post chain + backbuffer output policy | scene size, composite_written flag, postfx toggles | `R_Pass_PostProcess -> GLBackend_PassPostProcess -> GL_PostProcess` | hoch | expects composite availability + deterministic baseline | backend-owned | hoch |
| Draw viewmodel | `COMPOSITE_COLOR` | none | none | overlay draw to backbuffer/composite output | refdef/model globals | `R_Pass_DrawViewmodel -> GLBackend_PassOverlayViewmodel -> R_DrawViewModel` | hoch | depth range/depth clear assumptions for viewmodel | transitional | mittel-hoch |
| Polyblend | `COMPOSITE_COLOR` | none | none | overlay blend | blend globals | `R_Pass_DrawPolyblend -> GLBackend_PassOverlayPolyblend -> V_PolyBlend` | mittel | blend state correctness depends on baseline | transitional | mittel |
| Store previous frame | `COMPOSITE_COLOR`, `SCENE_DEPTH` | none | none | temporal history side effect | current frame outputs | `R_Pass_StorePrevFrame -> R_StorePrevFrameState` | indirekt mittel | ordering dependency after scene/warp/post | core-deklarativ (CPU/history sidefx) | mittel |

## Out-of-Framegraph render-relevant paths (boundary risks)

- 2D/UI/HUD/Console/Menu: `render_dispatch` + `gl_screen.c`/`gl_draw.c` (`GL_SCR_UpdateScreen`, `GL_Set2D`) are compatibility-layer paths, not framegraph passes.
- Screenshot/readback: `SCR_ScreenShot_f` (`glReadPixels` in `gl_screen.c`) and auto-exposure readback in `gl_rmain.c`.
- Resize/recreate: `GL_CreateFrameBuffers` / `GL_DeleteFrameBuffers` + shadow recreates in `gl_shadow_runtime.c`.

## Implicit GL-State dependencies

Observed from backend/runtime execution:
- Depth:
  - shadow pass toggles clip control/depth func/depth clear conventions.
  - viewmodel and scene rely on depth-range/depth-mask conventions.
- Blend:
  - overlay/polyblend/post rely on baseline reset and expected blend mode setup.
- Cull/front-face:
  - scene/shadow/viewmodel expect consistent cull/front-face state.
- Viewport/scissor:
  - framegraph binds viewport mode per pass, but passes still assume this is correct on entry.
- Framebuffer binding:
  - warp/resolve/post/shadow rely on correct target already bound.
- Program/buffer/VAO-like assumptions:
  - transitional render units (`r_world`, `r_part`, `r_part_q3p`) still bind programs/buffers directly.
- Texture unit assumptions:
  - several passes assume fixed texture units for lightmaps/shadows/postfx inputs.

Markers added in code:
- `TODO_PASS_BOUNDARY`
- `TODO_STATE_BASELINE`
- `REF_GL_PASS_EXECUTION`
- `LEGACY_IMPLICIT_STATE`

## Depth / Reverse-Z / Clear Policy

Current behavior:
- Reverse-Z decision is backend/runtime-driven (`gl_clipcontrol_able` + clip control mode toggles).
- Shadow pass temporarily switches to `GL_NEGATIVE_ONE_TO_ONE` + `GL_LEQUAL` and restores main path to `GL_ZERO_TO_ONE` + expected depth func.
- Main scene depth is cleared in scene pass attachments (`SCENE_DEPTH`, load=clear/store=store).
- Shadow depth is separate resource (`SHADOW_SUN_DEPTH`) with independent clear/store policy.
- Postprocess and warp/resolve read scene/composite depth depending on plan.

Per-pass depth expectations:
- Writes depth: Shadow maps, Render scene, Warp/resolve (composite depth target path).
- Reads depth: Warp/resolve, Postprocess, Store previous frame.
- Separate depth domains: shadow depth targets vs main scene/composite depth targets.

Future contract need (backend-neutral):
- explicit depth policy flags per pass (read/write/clear/reverse-z space)
- explicit separation of shadow-depth vs scene-depth classes in contract docs.

## Resource Read/Write dependencies and transition needs

| Resource | Writer pass | Reader pass | Lifetime | transient/persistent | future barrier/transition need |
|---|---|---|---|---|---|
| `SCENE_COLOR` | Render scene | Warp/resolve | frame | transient | attachment write -> sampled/blit read |
| `SCENE_DEPTH` | Render scene | Warp/resolve, Postprocess, Store previous frame | frame | transient | attachment write -> sampled read |
| `VELOCITY` | Render scene | (post/motion consumers backend-side) | frame | transient | attachment write -> sampled/storage read |
| `COMPOSITE_COLOR` | Warp/resolve, Postprocess | Postprocess, overlays, Store previous frame | frame | transient | attachment write -> sampled/present |
| `COMPOSITE_DEPTH` | Warp/resolve | Postprocess | frame | transient | attachment write -> sampled read |
| `SHADOW_SUN_DEPTH` | Shadow maps | Render scene (shadow sample path) | frame | transient | depth attachment write -> sampled read |
| `DECALS` | Update decals | Render scene | frame | transient CPU sidefx | ordering barrier (CPU update before render) |
| `SSAO_FOG_STATE` | Capture fog handoff | Postprocess | frame | transient CPU sidefx | ordering dependency only |
| readback buffers (non-FG) | Postprocess/autoexposure path | CPU readback | frame | transient | explicit readback sync point |
| temporal history (non-FG explicit resource) | Store previous frame | next frame scene/post | multi-frame | persistent | explicit history resource declaration later |

## Backend-pass execution ownership snapshot

Backend-owned execution callbacks (OpenGL backend):
- `GLBackend_PassSetupView`
- `GLBackend_PassShadowMaps`
- `GLBackend_PassRenderScene`
- `GLBackend_PassWarpResolve`
- `GLBackend_PassPostProcess`
- `GLBackend_PassOverlayViewmodel`
- `GLBackend_PassOverlayPolyblend`

Key global resource touchpoints:
- `framebufs.*` in `gl_rmain.c`
- shadow resources in `gl_shadow_runtime.c`
- transitional world/particle units for direct draw execution.

Phase-5 candidates from this boundary:
1. move more overlay/2D presentation paths under framegraph-driven pass descriptors
2. isolate transitional direct GL state in `r_world/r_part/r_part_q3p` behind backend-owned adapters
3. declare temporal history/readback as explicit framegraph resources/services
