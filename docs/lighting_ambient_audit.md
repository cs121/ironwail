# Lighting/Ambient Audit (World, Models, Weapons)

Static code review of current renderer behavior.

## 1) Ambient/Minlight behavior

### World (brush surfaces)
- **Shader path:** `Quake/shaders/world.frag`
- **Core logic:** static lightmaps + optional lightgrid modulation + dynamic lights + optional shadow split.
- `in_lightgrid` is multiplied into static lighting (`lightgrid = mix(vec3(1.0), in_lightgrid, LightgridParams.x)`).
- Directional ambient shaping is optional via `r_lightgrid_directional` (`EvaluateDirectionalAmbient`, fixed `ambientBias=0.25`).
- No global minlight clamp in world shader.

### Models / entities (alias)
- **CPU lighting setup:** `Quake/opengl/gl_rlight.c::R_EntityStaticLight`, `Quake/renderer/r_alias.c::R_SetupAliasLighting`
- Path:
  1. Static sample from lightgrid (if `r_model_lightgrid` + lightgrid available), else lightmap ray sample (`R_LightPointNoGrid`).
  2. Add dynamic lights.
  3. Apply model-specific boosts/minimums in `R_SetupAliasLighting`.
- Model minlight cvar:
  - `r_minlight_models` (`gl_rlight.c`) applies only if sampled intensity is zero-ish and entity is not viewmodel.
- Player minimum light:
  - hardcoded floor to sum=24 (`r_alias.c`).

### Weapons / viewmodel
- **Special-case in `R_SetupAliasLighting`:**
  - boost: `new_L = max(L*1.5, L+40)`
  - hard minimum sum=72
- This is independent of `r_minlight_models` and hardcoded.

---

## 2) Lightmaps (sampling, encoding, gamma/linear, HDR)

### Sampling
- **CPU sample path:** `gl_rlight.c::InterpolateLightmap` (bilinear in lightmap texel space), combines up to `MAXLIGHTMAPS` styles via `LightStyleValue`.
- **GPU world path:** `world.frag::SampleLightmap` with style blending in shader.
- **Developer note:** The clustered world renderer path (`Quake/shaders/world.vert` + `Quake/shaders/world.frag`) is canonical. Legacy split world dlight fragment variants were removed from the repository during shader cleanup because they are excluded from normal runtime shader loading.

### Encoding / storage
- Surface sample pointers in BSP load:
  - `gl_model.c` uses 3 bytes/sample for RGB lightdata, 4-byte stride when `MOD_HDRLIGHTING` is set.
- Lightmap texture upload:
  - `gl_texmgr.c::TexMgr_LoadLightmap`.
  - If `r_lightmap16f=1`, uploads as `GL_RGBA16F`.
  - Else uploads `GL_SRGB8_ALPHA8` or `GL_RGBA8` depending on `r_lightmap_colorspace`.

### Gamma/linear control
- Main controls:
  - `r_lightmap_colorspace` (`srgb|linear`) + compat alias `r_lightmap_linear`.
  - Sync callbacks in `gl_texmgr.c` (`TexMgr_LightmapColorspace_f`, `TexMgr_LightmapLinearCompat_f`).
- Engine policy comment in `gl_rmain.c` states linear HDR compositing with one output transform in postprocess.

### HDR yes/no
- Yes, renderer composites in HDR targets (`GL_RGBA16F` framebuffers in `gl_rmain.c`).
- BSPX HDR lighting (`E5BGR9`) is recognized in loader (`gl_model.c`).

---

## 3) Lightgrid / lightvolumes

### Data format
- Struct definitions: `common/lightgrid.h`.
- Runtime load/parse from BSPX `LIGHTGRID_OCTREE`: `gl_model.c::LightgridOctree_LoadBSPX`.
- Sampling backend: `gl_lightgrid.c` (octree traversal + nearest-cell sample, style 0 preference).
- Probe currently effectively gives `rgb` + `ao`, direction defaults to `(0,0,1)`.

### Renderer sampling
- CPU-side model/entity sampling:
  - `gl_rlight.c::R_GetLightgridSample`, `R_LightgridLighting`, `R_EntityStaticLight`.
- World shader usage:
  - `world.frag` consumes per-vertex `in_lightgrid` and applies it as ambient/static modulation.

### Entity usage
- Alias models: yes (`R_EntityStaticLight` + `R_ApplyLightgridLighting`).
- If lightgrid invalid, fallback to lightmap ray sampling (`R_LightPointNoGrid`).

---

## 4) Specular / envmap tricks

### World
- **Specular:** in `world.frag` from clustered dynamic lights, fixed `SPECULAR_POWER=16`, quality-scaled multiplier.
- **Reflection probes:** `EvaluateReflectionProbe` samples `ReflectionTex` with LOD from roughness param.
  - Current roughness is hardcoded (`surface_roughness = 0.4` in world shader path).

### Models
- Alias shader (`alias.frag`) has rim-light terms and direct/ambient blending, but no physically based roughness pipeline.
- No material roughness map for alias in this path.

### Legacy envmap tcgen
- World material system still has `TCGEN_ENVIRONMENT` plumbing in shader interface (`world.frag` call flags/tcgen), but modern reflection-probe path is separate.

---

## 5) Skybox / sun direction & color sources

### Skybox
- Loader/draw: `gl_sky.c` (`Sky_LoadSkyBox`, cubemap path, layered fallback).
- Skybox content does **not** define sun direction/color automatically.

### Sun direction/color source
- Resolved in `gl_rlight.c::R_UpdateSunFallback`:
  1. map/worldspawn keys (`sunlight`, `sunlight_color`, `sun_mangle`) and sun-like entities (`light_environment`, `sun`, `env_sun`)
  2. cvars (`r_shadow_sun_dir`, `r_shadow_sun_color`, `r_shadow_sun_intensity`)
  3. fallback (`r_sun_fallback`, `r_sun_force_fallback`, etc.)
- Shadow system writes sun vector into frame UBO: `renderer/r_shadow.c::R_Shadow_SunPass`.

---

## 6) Fog / godrays / volumetrics: ambient/sun color usage

### Godrays
- Mask: `shaders/godrays_mask.frag` uses sky-depth visibility and sun screen position.
- Shafts: `shaders/godrays.frag` multiplies accumulated shafts by `SunTint` (`r_sun.color * r_sun.intensity` from `gl_rmain.c`).

### Volumetric fog (froxel)
- Build pass: `shaders/atmos_froxel_build.comp` uses `Fog_GetColor()` + density/albedo/noise and local fog volumes.
- Light integration: `shaders/atmos_froxel_integrate.comp` uses `ShadowSunDir` and `LightParams1` (sun color), plus shadow map and clustered lights.
- CPU setup: `gl_rmain.c::Atmosphere_Froxel_LightIntegrate` uploads sun color and fog-sun strength.

### Legacy fog
- Classic fog path remains (`world.frag` / `alias.frag` `ApplyFog` via `Fog` uniform), but volumetric system is unified by cvars in `gl_rmain.c`.

---

## 7) Problem list (current risk points)

1. **Entity brightness model is mixed hardcoded + sampled:** viewmodel/player hard minimums + boosts can diverge from world shading.
2. **Lightgrid directional data underused:** probe direction is effectively placeholder, directional ambient in world uses fixed up-vector heuristic.
3. **Specular not truly roughness/material-driven:** world roughness hardcoded, alias has rim/spec hacks only.
4. **Potential gamma inconsistency edge cases:** multiple compatibility cvars (`r_lightmap_colorspace` + `r_lightmap_linear`) and mixed legacy paths can confuse tuning.
5. **Skybox/sun decoupling:** bright skyboxes can visually imply sun direction not matching actual sun lighting.
6. **Reverse-Z branch complexity in post/fog/godrays:** many runtime branches, parity risk between reversed/normal depth paths.

---

## 8) CVAR plan (use, merge/alias)

### Keep as primary knobs
- Lightmap space: `r_lightmap_colorspace` (primary), `r_lightmap16f`.
- Entity ambient/minlight: `r_model_lightgrid`, `r_minlight_models`.
- Sun: `r_shadow_sun_dir`, `r_shadow_sun_color`, `r_shadow_sun_intensity`, `r_sun_fallback` (+ force/allow options).
- Lightgrid: `r_lightgrid`, `r_lightgrid_force`, `r_lightgrid_directional`.
- Reflections/spec: `r_reflection_probes`, `r_reflection_probe_debug`, `r_lighting_debug`.
- Fog/godrays: `r_fog_*` family, `r_godrays*` family.

### Alias / deprecate candidates
1. Keep `r_lightmap_linear` as **compat alias only** (already synchronized) and document `r_lightmap_colorspace` as canonical UI-facing control.
2. Consider unifying viewmodel/player hardcoded minlight with cvars:
   - e.g. `r_minlight_viewmodel`, `r_minlight_players`, `r_viewmodel_light_boost`.
3. Consider grouping sun fallback cvars under one mode cvar for easier presets (map/cvar/fallback/off).

### Suggested migration order
1. Document canonical cvars in `docs/cvar_use_cases.md`.
2. Add warnings when legacy alias cvars are used in configs.
3. Introduce new cvarized model/viewmodel minlight controls while preserving old defaults.
4. Keep old names as aliases for one release cycle.
