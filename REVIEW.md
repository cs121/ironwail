# Ironwail Code Review: Render Architecture & Feature Set

## Executive Summary

Ironwail is a high-performance Quake engine fork descended from QuakeSpasm, targeting
OpenGL 4.3+. It transforms the classic fixed-function Quake renderer into a modern
GPU-driven pipeline while preserving gameplay compatibility. The codebase comprises
~145K lines of C across 198 source/header files, with 58 GLSL shaders.

**Verdict**: Impressive engineering. The renderer is among the most advanced in the
Quake source-port ecosystem, with GPU culling, indirect rendering, bindless textures,
compute shaders, shadow mapping, SSAO, volumetric fog, god rays, OIT, and a Q3-style
material system. The architecture is sound but shows signs of rapid feature accretion,
with some files becoming monolithic and coupling between subsystems growing complex.

---

## 1. Render Architecture

### 1.1 Pipeline Overview

The frame rendering pipeline is orchestrated by `R_FrameGraph_RenderView()`
(`r_framegraph.c`), which sequences passes in a clean, linear order:

```
R_SetupView()           -- Camera, frustum, matrices
R_RenderShadowMaps()    -- Sun shadow (2D depth) + DLight cube-map array
R_UpdateDecals()        -- Decal state update
Fog_EnableGFog()        -- Global fog setup
R_RenderScene()         -- Main scene rendering
R_WarpScaleView()       -- MSAA resolve, warp/scale, compositing
R_FogVol_BuildList()    -- Volumetric fog primitive gathering
R_FogVol_Render()       -- Volumetric fog compositing
R_SSAO_CaptureFogState() -- Fog params for SSAO (bug fix #1)
Fog_DisableGFog()       -- Disable fog for 2D overlays
```

Within `R_RenderScene()` (`gl_rmain.c:5451`), the draw order is:

```
R_SetupScene / R_SetupGL / R_Clear
R_UploadFrameData       -- UBO with per-frame matrices, fog, lights
R_DrawViewModel          -- Weapon model (drawn first, dedicated depth)
R_DrawEntitiesOnList(false) -- Opaque entities (alias, brush, sprite)
R_DrawDecals            -- Surface decals
R_DrawDLightPass        -- Dynamic light contribution buffer
R_DrawParticles(false)  -- Opaque/additive particles
Sky_DrawSky             -- Skybox/sky layers
R_DrawWater(false)      -- Opaque water
R_BeginTranslucency     -- OIT begin
R_DrawWater(true)       -- Transparent water
R_DrawEntitiesOnList(true) -- Translucent entities
R_DrawParticles(true)   -- Translucent particles
R_EndTranslucency       -- OIT resolve
R_ShowTris / R_ShowBoundingBoxes / etc.  -- Debug overlays
```

**Strength**: The framegraph abstraction (`r_framegraph.h`) is a good structural
improvement, cleanly separating pass orchestration from pass implementation. The
callback-based state capture pattern (for SSAO fog state) solves a real ordering
dependency elegantly.

**Concern**: `gl_rmain.c` at 5,693 lines is the largest file in the codebase and
handles shadow mapping, scene rendering, entity sorting, dlight passes, warp/scale,
OIT, and the main render view. This is a candidate for decomposition.

### 1.2 GPU-Driven Rendering

The world rendering path (`r_world.c`) uses a fully GPU-driven pipeline:

1. **PVS Upload**: Visibility data uploaded to SSBO
2. **Compute Culling** (`cull_mark.comp`): GPU-side frustum + PVS culling of surfaces
3. **Indirect Draw Buffer** (`gather_indirect.comp`): Compute shader populates
   `GL_DRAW_INDIRECT_BUFFER` with per-texture draw commands
4. **Multi-Draw Indirect**: `GL_MultiDrawElementsIndirectFunc()` issues all world
   geometry in a single call per texture group

This is state-of-the-art for a BSP renderer. Surface marking, which was historically
a CPU bottleneck in Quake engines, is fully offloaded to the GPU.

**Strength**: The compute-based culling eliminates the classic Quake world-traversal
CPU bottleneck. Combined with indirect multi-draw, this allows rendering maps with
hundreds of thousands of polygons efficiently.

**Concern**: The `clear_indirect` + `cull_mark` + `gather_indirect` pipeline uses
three compute dispatches with barriers between them. For extremely high surface counts,
this could be consolidated into fewer passes. However, the current approach is clear
and maintainable.

### 1.3 Shader System

GLSL shaders live in `Quake/shaders/` (58 files) and are loaded by `gl_shaders.c`
with a simple file-based include system (with path traversal safety checks). The
shader set includes:

- **World**: `world.vert/frag`, `world_dlight.vert/frag`, `world_shadow.vert/frag`
- **Alias Models**: `alias.vert/frag`, `alias_shadow.vert/frag`
- **Sky**: `sky_cubemap`, `sky_layers`, `sky_boxside`, `skystencil`
- **Water**: `water.vert/frag` with bindless textures
- **Particles**: `particles.vert/frag` with Q3-material support
- **Post-Processing**: `postprocess.vert/frag`, `bloom_extract/blur`, `ssao.frag`,
  `ssao_blur.frag`, `godrays.frag`, `viewblend`, `warpscale`, `oit_resolve`
- **Volumetric**: `fogvol.frag`, `fogvol_froxel_inject.comp`, `fogvol_temporal.frag`
- **Compute**: `clear_indirect.comp`, `cull_mark.comp`, `gather_indirect.comp`,
  `palette_init.comp`, `palette_postprocess.comp`
- **Shared includes**: `frame_uniforms.glsl`, `postprocess_dof.glsl`,
  `postprocess_motion.glsl`

**Strength**: Clean separation of shader stages. The include system with
`GL_NormalizeShaderIncludePath()` has proper security validation against path
traversal. Shared uniforms via `frame_uniforms.glsl` reduce duplication.

**Concern**: Shader variants are managed by compiling multiple program objects
(e.g., `glprogs.world_dlight[0]` and `[1]`). There's no preprocessor-based
permutation system, which could lead to shader program proliferation as features
grow. Consider uber-shaders with `#define` specialization.

### 1.4 Bindless Textures

When `GL_ARB_bindless_texture` is available (checked at init, `-nobindless` opt-out),
texture handles are made resident and passed via SSBOs/uniforms rather than binding
to texture units. This is used for world, water, sky layers, and god rays source
extraction.

The `bmodel_bindless_gpu_call_t` structure (`r_world.c:280`) packs `GLuint64` handles
for diffuse, fullbright, and emissive textures alongside per-draw state, enabling
the entire world to be rendered without texture rebinding.

**Strength**: Properly handles fallback to traditional binding when bindless is
unavailable. The `bmodel_bound_gpu_call_t` variant (`r_world.c:294`) provides
the non-bindless path.

### 1.5 Shadow Mapping

Implemented in `gl_rmain.c` (lines 132-470+), the shadow system provides:

- **Sun shadows**: Single cascade, orthographic projection with texel-snapping
  stabilization (lines 431-441)
- **Dynamic light shadows**: Cube-map array (up to 4 lights x 6 faces),
  perspective projection per face
- **DLight selection**: Score-based ranking using `radius * (0.35 + luminance) /
  (1 + dist * 0.0025)` with deterministic tie-breaking
- **PCF filtering**: Configurable PCF texel size for both sun and dlights
- **Casters**: World/brush opaque + alias models (no alpha-test casters in V1)
- **Receivers**: World and alias models via `ShadowSunViewProj` uniform

The uniform caching system (`R_Shadow_GetReceiverUniforms`,
`R_Shadow_GetCasterUniforms`) uses fixed-size arrays with overflow-reset
semantics.

**Strength**: The texel-snapping for shadow stabilization is well-implemented,
preventing shadow swimming as the camera moves. The score-based dlight selection
is a pragmatic approach.

**Concerns**:
- The uniform cache overflow resets the entire array (`r_shadow_receiver_uniforms_count = 0`),
  which could cause a burst of `glGetUniformLocation` calls. An LRU eviction would
  be more graceful.
- Shadow-related code (~500 lines) embedded in `gl_rmain.c` should be extracted to
  a dedicated `r_shadow.c`.
- Single cascade for sun shadows will show aliasing on large maps. Cascaded Shadow
  Maps (CSM) would be a natural V2 improvement.

### 1.6 SSAO

Implemented across `r_ssao.c` (utility functions), `ssao.frag` and `ssao_blur.frag`.
Highly configurable with 33+ cvars controlling radius, intensity, bias, power,
samples, blur parameters, noise, resolution, and fog coupling.

**Strength**: The fog-coupling fix (BUG FIX #1 at `gl_rmain.c:50-58`) is well-documented,
solving the SSAO-after-fog-disable ordering issue by capturing fog state into
`r_ssao_fog_state_t` before `Fog_DisableGFog()`. Half-res rendering option with
proper texelFetch-based upscaling.

**Concern**: 33 cvars for a single effect is excessive for end-user exposure. Consider
grouping into presets (low/medium/high/ultra) with individual cvars as advanced
overrides.

### 1.7 Volumetric Fog

`r_fogvol.c` (4,099 lines) implements a sophisticated volumetric fog system:

- Per-volume GPU data structure with mins/maxs, sphere bounds, color/density,
  noise parameters, wind/turbulence, and lighting data
- Dynamic light interaction per fog volume (broadphase + narrowphase culling)
- Lightgrid probe sampling for ambient lighting within volumes
- Froxel injection via compute shader (`fogvol_froxel_inject.comp`)
- Temporal filtering (`fogvol_temporal.frag`)

The `fog_volume_gpu_t` structure uses 16-byte aligned vec4 packing for UBO/SSBO
compatibility, verified by `COMPILE_TIME_ASSERT`.

**Strength**: The lighting pipeline (broadphase candidate gathering, narrowphase
intersection testing, lightgrid probe sampling) is thorough. Compile-time alignment
assertions prevent silent GPU data corruption.

**Concern**: At 4,099 lines, this is the second-largest source file. The light
interaction system alone could be a separate module.

### 1.8 God Rays

`r_godrays.c` (182 lines of utility) + `godrays.frag`, `godrays_mask.frag`,
`godrays_source.frag`, `godrays_source_sky.frag` implement radial light shafts.

Features per-sky-texel threshold, tint, emissive/lighttex intensity,
configurable samples/density/weight/decay/exposure, blur, and coupling with
volumetric fog (`r_godrays_vol_pow`).

**Strength**: Clean separation of source extraction, masking, and radial blur
stages. The fog coupling provides visual coherence between fog volumes and
light shafts.

### 1.9 Order-Independent Transparency (OIT)

The engine uses OIT for translucent rendering:

- `R_BeginTranslucency()` / `R_EndTranslucency()` bracket the transparent pass
- `oit_resolve.vert/frag` composites the accumulated transparency
- Separate `oit_fbo` in the `framesetup_t` structure
- Used for water, translucent entities, and translucent particles

**Strength**: OIT solves the classic Quake transparency sorting problem,
particularly important for overlapping water surfaces and translucent brush models.

### 1.10 Post-Processing Pipeline

`r_postfx.c` implements a comprehensive post-processing stack:

- **Bloom**: Extract + multi-pass Gaussian blur
- **Color grading**: LUT-based color correction (`r_postfx_lut`)
- **Damage effects**: Vignette, desaturation, exposure shift, double-vision
- **Pickup flash**: Exposure + bloom boost on item pickup
- **Powerup effects**: LUT strength modulation with ramp in/out
- **Underwater effects**: Color grade + fog tint per liquid type
- **Quad damage**: Emissive/bloom boost with pulse
- **Depth of field**: Shader-based (`postprocess_dof.glsl`)
- **Motion blur**: Velocity-buffer based (`postprocess_motion.glsl`)

**Strength**: The damage/powerup effect system hooks into gameplay events for
dynamic visual feedback. LUT-based grading allows artist-driven look development.

**Concern**: The sheer number of cvars (60+) for post-processing alone makes
the configuration surface area very large. A preset system would improve usability.

### 1.11 Material System (Q3-Style Shaders)

`mat_shader.c` (1,623 lines) + `mat_shader_parse.c` implement Quake 3 Arena-style
shader materials:

- Hash-table based material lookup (256 buckets)
- Sort keys: sky, opaque, see-through, decal, banner, underwater, additive, nearest
- Stage features: map/clampmap, blendFunc, rgbGen/alphaGen, animMap, tcMod
  (scroll, scale, rotate, turb, stretch)
- Particle material contract defined for quad/beam rendering paths
- Unknown keyword tracking with overflow protection

**Strength**: The keyword scope/status system provides graceful degradation for
unsupported Q3 shader directives. The particle shader contract is well-documented
in the header comment.

### 1.12 Decal System

`r_decals.c` (758 lines) implements surface decals:

- Definition-driven (`decaldef_t`): texture, size range, alpha range, color,
  lifetime, fade, rotation, priority, blend mode
- GPU-friendly vertex format with immediate-mode upload
- Compaction to handle vertex buffer fragmentation
- Configurable limits: 128 definitions, 1024 instances, 24K vertices

**Strength**: Clean, self-contained module with reasonable fixed limits.

---

## 2. Feature Set Assessment

### 2.1 Rendering Features (Modern)

| Feature | Status | Quality |
|---------|--------|---------|
| GPU-driven world rendering | Complete | Excellent |
| Compute culling (frustum + PVS) | Complete | Excellent |
| Indirect multi-draw | Complete | Excellent |
| Bindless textures | Complete (optional) | Good |
| Shadow mapping (sun + dlight) | V1 complete | Good |
| SSAO | Complete | Good |
| Volumetric fog | Complete | Excellent |
| God rays | Complete | Good |
| OIT | Complete | Good |
| Bloom | Complete | Good |
| Color grading (LUT) | Complete | Good |
| Depth of field | Complete | Good |
| Motion blur (velocity buffer) | Complete | Good |
| Q3 material system | Partial (MVP) | Good |
| Decals | Complete | Good |
| Lightgrid (ambient probes) | Complete | Good |
| Emissive maps | Complete | Good |
| MSAA resolve | Complete | Good |
| Warp/scale view | Complete | Good |
| Real-time palettization | Complete | Good |

### 2.2 Rendering Features (Classic, Preserved)

| Feature | Status |
|---------|--------|
| BSP rendering | Complete |
| Lightmaps (atlas-based) | Complete |
| Alias model rendering (MDL) | Complete with instancing |
| Sprite rendering | Complete |
| Particle system (classic) | Complete |
| Q3-style particles | Complete |
| Sky rendering (layers + cubemap) | Complete |
| Water warp | Complete |
| Fog (global) | Complete |
| Dynamic lights | Complete (GPU buffer) |
| Fullbright textures | Complete |
| Overbright lighting | Complete |
| Model interpolation (lerp) | Complete |

### 2.3 Platform & Compatibility

- **Graphics API**: OpenGL 4.3+ (no Vulkan, no legacy GL)
- **Window/Input**: SDL2
- **Platforms**: Windows, Linux (no macOS due to GL 4.3 requirement)
- **Audio**: SDL2 + codecs (MP3, Opus, FLAC, Vorbis, MikMod, XMP)
- **Network**: Classic Quake protocol (datagram-based)
- **Async I/O**: Optional async filesystem and asset staging
- **Build**: CMake (primary), GNU Make, MSVC, CodeBlocks

### 2.4 Unique Features vs. Standard Quake/QuakeSpasm

1. GPU-driven world rendering with compute culling
2. Shadow mapping (sun + dynamic lights)
3. SSAO with fog coupling
4. Volumetric fog with dynamic light interaction
5. God rays with sky/emissive sources
6. OIT for correct transparency
7. Q3-style material/shader system
8. Decal system
9. Lightgrid-based model lighting
10. Post-processing stack (bloom, LUT grading, DoF, motion blur, damage FX)
11. Async worker threading
12. Emissive map support
13. KTX2 texture format support
14. BC7 texture compression
15. 2021 Steam Quake remaster integration

---

## 3. Architectural Concerns & Recommendations

### 3.1 File Size and Decomposition

| File | Lines | Recommendation |
|------|-------|----------------|
| `gl_rmain.c` | 5,693 | Extract shadow mapping (~500L), OIT setup, dlight pass |
| `r_fogvol.c` | 4,099 | Extract light interaction into `r_fogvol_light.c` |
| `gl_texmgr.c` | 3,086 | Consider splitting bindless management |
| `r_world.c` | 1,968 | Acceptable, but GPU draw path could be its own file |

### 3.2 Global State Coupling

The rendering pipeline relies heavily on global state:
- `r_framedata` (UBO data), `r_shadow_state`, `r_lightbuffer`, `framebufs`,
  `framesetup`, `glprogs`, `r_refdef`
- Many modules use `extern cvar_t` to reach across boundaries

This works for a single-threaded renderer but complicates reasoning about data flow.
The framegraph callback pattern (`r_framegraph_state_t`) is a step in the right
direction; extending this pattern to other inter-pass data would improve modularity.

### 3.3 Cvar Explosion

The engine exposes 100+ rendering cvars. While configurability is valued in the
Quake community, the surface area creates:
- Testing combinatorial explosion
- User confusion
- Maintenance burden for default tuning

**Recommendation**: Introduce a `r_quality` master cvar (0-4) that sets sensible
defaults for groups of settings, while preserving individual cvar overrides for
power users.

### 3.4 Duplicated Utility Patterns

`R_Godrays_SanitizeValue()` and `R_SSAO_SanitizeValue()` are identical functions.
Similar `isfinite` checks with MSVC workarounds appear in multiple places.

**Recommendation**: Consolidate into a shared `R_SanitizeFloat()` in a common
rendering utility.

### 3.5 Shadow Uniform Cache

The fixed-size uniform cache with full-reset overflow
(`r_shadow_receiver_uniforms[]`) is pragmatic but fragile. If shader program IDs
are recycled by the GL driver, stale entries could return wrong uniform locations.

**Recommendation**: Clear the cache on `vid_restart` / shader reload events, not
just on overflow. Consider using a generation counter.

### 3.6 Memory Safety in Render Path

`r_world.c` includes a `R_ValidPtr()` function (line 138) that checks for NULL,
-1, and low addresses (< 0x10000). This defensive check appearing in the hot
render path suggests past crashes from dangling/corrupt pointers.

**Recommendation**: Investigate the root cause. Defensive pointer validation in
the render loop is a symptom, not a fix. The rate-limited warning logging is good,
but the underlying model data integrity should be guaranteed at load time.

### 3.7 Hardcoded Limits

Several systems use compile-time limits:
- `MAX_ALIAS_INSTANCES` = 256
- `MAX_DECAL_DEFS` = 128, `MAX_DECAL_INSTANCES` = 1024
- `MAX_FOGVOLUMES`, `MAX_FOGLIGHTS`
- `SHADOW_DLIGHT_MAX` = 4

These are reasonable for Quake content but could be hit by ambitious modern maps.
The limits are well-defined and documented, which is positive.

### 3.8 Missing Features (Relative to State of Art)

- **Cascaded Shadow Maps**: Single cascade limits shadow quality on large maps
- **Alpha-test shadow casters**: Fences, grates don't cast shadows (documented V1 limit)
- **Vulkan backend**: Would unlock macOS support and potentially better performance
- **Temporal Anti-Aliasing (TAA)**: Motion vectors are already available from the
  velocity buffer
- **Screen-Space Reflections**: The depth + normal data from SSAO could support SSR

---

## 4. Code Quality Observations

### 4.1 Positive Patterns

- **Compile-time assertions**: `COMPILE_TIME_ASSERT` used extensively for GPU struct
  alignment (e.g., `r_fogvol.c:48`, `r_alias.c:96-97`)
- **Debug infrastructure**: Comprehensive debug cvars and visualization modes for every
  major subsystem (SSAO has 14 debug modes, shadows have 4)
- **Documentation**: Technical docs in `docs/` cover shadow mapping, SSAO, threading,
  fog volumes, shaders, and emissive maps -- written in both English and German
- **Rate-limited warnings**: Console warnings are throttled by frame count to prevent
  spam
- **Security**: Shader include path validation prevents directory traversal
- **Graceful fallback**: Features degrade cleanly when GPU capabilities are absent
  (bindless, compute, etc.)

### 4.2 Style Consistency

The codebase shows its heritage: mixed indentation (tabs in original Quake code,
spaces in newer additions), some functions use `//johnfitz` attribution comments.
New code generally follows a consistent C99/C11 style. Function-level `GL_BeginGroup`
/ `GL_EndGroup` calls provide GPU debugger (RenderDoc) friendliness.

### 4.3 Testing

No automated test suite is present. The extensive debug cvar infrastructure serves as
the primary validation mechanism, supplemented by CI builds on Linux, Windows, MinGW,
and macOS.

---

## 5. Summary

Ironwail represents a remarkable transformation of a 1996 engine into a modern
GPU-driven renderer. The GPU culling, indirect rendering, and compute shader
integration are particularly well-executed, delivering the performance needed for
today's demanding custom Quake maps.

The primary architectural risk is the growing complexity of `gl_rmain.c` and the
coupling between rendering subsystems via global state. The framegraph abstraction
is a good foundation that should be extended.

The feature set is comprehensive and competitive with any Quake source port. The
post-processing and volumetric effects bring the visual quality close to modern
indie-game standards while respecting the aesthetic of the original game.

**Priority recommendations**:
1. Decompose `gl_rmain.c` -- extract shadow mapping, OIT, and dlight pass
2. Introduce quality presets to tame the cvar surface area
3. Consolidate duplicated utility functions
4. Add cascaded shadow maps for sun lighting
5. Investigate root cause of defensive pointer checks in render path
