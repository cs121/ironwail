# ref_gl Phase 2 Resource Findings

Stand: 2026-05-05
Basis: `docs/renderer_resource_ownership.md`

## Plugin Boundary Audit (`ref_gl_plugin.c`)

| extern Symbol | Herkunft (primär) | Zweck | ref_gl-private möglich? | braucht neutralen Contract? | Legacy-Compat akzeptabel? | Risiko |
|---|---|---|---|---|---|---|
| `R_Init`, `R_RenderView`, `R_NewMap` | `gl_rmisc.c`, `gl_rmain.c` | core render lifecycle | teilweise | ja (core renderer contract) | vorerst ja | mittel |
| `R_ClearEfrags`, `R_CheckEfrags`, `R_AddEfrags`, `R_AddStaticModels` | `gl_refrag.c` | world entity linking | nein kurzfristig | ja | ja | mittel |
| `R_PushDlights`, `R_ParseDlightEntities` | `gl_rlight.c` | dynamic light ingest | teilweise | ja | ja | mittel |
| `R_GetLightgridSample` | `gl_rlight.c` | lighting query | teilweise | ja | ja | mittel |
| Particle/Decal gameplay hooks (`R_ParseParticleEffect`, `R_RunParticleEffect`, `R_RocketTrail`, `R_EntityParticles`, `R_*Explosion`, `R_LavaSplash`, `R_TeleportSplash`, `R_SpawnImpactDecal*`) | `r_part.c`, `r_decals.c` | gameplay->renderer fx ingress | nein kurzfristig | eher legacy extension | ja | mittel |
| `R_TranslatePlayerSkin`, `R_TranslateNewPlayerSkin` | `gl_rmisc.c` | player skin updates | teilweise | optional | ja | niedrig |
| `R_ClearBoundingBoxes`, `R_StorePrevFrameState` | `gl_rmain.c` | debug/temporal state | teilweise | optional | ja | niedrig-mittel |
| `R_Set/Get/GetEffectiveAlphaMode` | `gl_rmain.c` | transparency mode | teilweise | ja | ja | mittel |
| `GL_CreateFrameBuffers`, `GL_DeleteFrameBuffers` | `gl_rmain.c` | render target lifecycle | ja (ref_gl scope) | langfristig behind neutral resize/resource service | ja | mittel |
| `R_ResetDRSState`, `R_ResetGodraysStabilization` | `gl_rmain.c` | temporal/postfx reset | ja | optional | ja | niedrig |
| `SCR_UpdateScreen`, `SCR_Init`, `SCR_CenterPrint`, `GL_SCR_*` | `gl_screen.c` | UI/screen path | teilweise | compatibility extension | ja | mittel |
| `Draw_*`, `GL_Set2D`, `GL_SetCanvas*` | `gl_draw.c` | 2D/HUD/menu draw | teilweise | compatibility extension | ja | mittel |
| `Bridge_DrawFlush`, `Bridge_DrawInit` | `ref_gl_bridge_stubs.c` | bridge fallback | nein | ja (clear compat split) | ja | mittel |
| `CL_RunParticles` | `r_part.c` | particle sim tick | teilweise | optional | ja | niedrig |
| `GL_Backend_GetInterface` | `gl_backend.c` | backend vtable export | ja | core plugin contract | ja | niedrig |
| `r_refdef` global | `gl_rmain.c` | view state sync in wrapper | nein kurzfristig | ja | ja | hoch |

Kurzfazit Boundary:
- funktional stabil, aber hohe Legacy-Kopplung.
- Phase-2-konform: sichtbar gemacht, Diagnostik verbessert, keine große Umstellung.

## Resource Ownership Check

### textures
- CPU-Daten: asset/wad/image loader.
- GPU-Daten: `gl_texmgr.c` (`glGenTextures`, `GL_TexImage*`, sampler/bindless).
- Native IDs entstehen: `gl_texmgr.c`, teils `gl_rmain.c` (offscreen textures).
- Delete: `GL_DeleteTexture`, `GL_DeleteNativeTexture`, cleanup paths.
- Native IDs außerhalb ref_gl-only Zielbereich: sichtbar via `gltexture_t` in breiteren Includes (bekanntes Phase-3-Thema).
- Phase-3 Kandidat: `gltexture_t` entkoppeln (opaque handle + backend resolve).

### lightmaps
- CPU-Daten: lightmap bytes/float conversion in texmgr/world prep.
- GPU-Daten: tex upload via `TexMgr_LoadLightmap` + optional PBO.
- Native IDs: in texmgr-managed textures.
- Delete: texmgr destroy/free paths.
- Leak außerhalb Zielbereich: lightmap texture pointers in non-private render units.
- Phase-3 Kandidat: lightmap resource slots als primäre Schnittstelle.

### shadow maps
- CPU-Daten: matrices/split params runtime-side.
- GPU-Daten: `gl_shadow_runtime.c` (`glGenTextures`, shadow FBOs).
- Native IDs: `framebufs.shadow.*`.
- Delete: `R_Shadow_DeleteFrameBuffers`.
- Phase-3 Kandidat: shadow targets nur über backend resource registry exponieren.

### FBOs
- CPU-Daten: framebuffer descriptors/sizing in `gl_rmain.c`.
- GPU-Daten: `GL_CreateFBO*` + `GL_DeleteFrameBuffers`.
- Native IDs: many `framebufs.*.fbo`.
- Delete: centralized in `GL_DeleteFrameBuffers` and shadow delete.
- Phase-3 Kandidat: stricter slot/registry usage, no direct global FBO touch outside backend-facing modules.

### buffers
- CPU-Daten: staging arrays / frame data structs.
- GPU-Daten: `GL_Upload`, `GL_CreateBuffer`, SSBO/UBO/VBO/IBO paths.
- Native IDs: in world/particle/q3p/gl_backend state.
- Delete: module-specific teardown (e.g. Q3P delete buffers).
- Leak: multiple non-`gl_*` render units still GL-buffer aware (`r_world.c`, `r_part*.c`).
- Phase-3 Kandidat: buffer handles behind backend descriptor-binding layer.

### shader programs
- CPU-Daten: shader metadata/debug labels.
- GPU-Daten: `glprogs.*`, `GL_UseProgram` etc.
- Native IDs: `GLuint program` across GL modules.
- Delete: shader subsystem teardown (not refactored in phase 2).
- Phase-3 Kandidat: pipeline/shader IDs consumed through neutral metadata APIs only.

### VAOs
- CPU-Daten: none meaningful.
- GPU-Daten: global VAO bootstrap (currently in platform GL path, not moved in this phase).
- Native IDs: `globalvao` and resource key registration.
- Delete: shutdown path unregister+context teardown.
- Leak: ownership crosses platform/ref_gl boundary currently.
- Phase-3 Kandidat: VAO bootstrap ownership into ref_gl-only runtime context layer.

### postfx targets
- CPU-Daten: postfx cvar/state + LUT CPU payload.
- GPU-Daten: LUT texture creation/upload in backend, postfx FBO chain in `gl_rmain.c`.
- Native IDs: `r_postfx_lut_tex`, `framebufs.*`.
- Delete: frame buffer delete paths; texture via GL lifecycle.
- Phase-3 Kandidat: explicit postfx resource service abstractions.

### screenshots/readbacks
- CPU-Daten: screenshot buffer allocations.
- GPU-Daten/readback: `glReadPixels` in `gl_screen.c` and `gl_rmain.c` auto-exposure/readback paths.
- Native IDs: readback tied to currently bound FBO/PBO handles.
- Delete: PBO lifecycle managed in auto-exposure helpers.
- Phase-3 Kandidat: neutral readback request API.

## Native GL IDs outside strict ref_gl-only target (observed)

- `r_world.c`: `GLuint`/`GLuint64` and direct GL buffer usage.
- `r_part.c` / `r_part_q3p.c`: direct GL buffers/program dispatch helpers.
- `r_postfx.c`: backend texture upload integration (still acceptable in Phase 2).

Diese Punkte sind dokumentierte Übergangslasten; in Phase 2 nicht tief umgebaut.

## Phase-3 Resource Boundary Cleanup Kandidaten (konkret)

1. `r_world.c`: bmodel buffer IDs + bindless texture handles zuerst kapseln.
2. `r_part_q3p.c`: SSBO lifecycle hinter backend resource/buffer helper ziehen.
3. `gl_texmgr.h` exposure reduzieren (opaque texture handle an neutralen Grenzen).
4. `gl_rmain.c` framebuffer globals in klarere backend-owned structs + accessors bündeln.
5. Readback (`gl_screen.c`/`gl_rmain.c`) via neutral request path auslagern.

## Phase-2 sichere Maßnahmen umgesetzt

- Keine ABI-Änderung.
- Keine Feature-Entfernung.
- Keine Core-Decontamination.
- Verbesserte Diagnostics + Boundary-Marker in plugin/dispatch.
