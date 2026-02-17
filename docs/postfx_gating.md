# Fullscreen PostFX Gating Audit and Consolidation

## Scope and decision rules

Fullscreen post effects are now enabled only by renderer CVAR gates in `gl_rmain.c`.
Material or shader metadata may still contribute **input energy** (for example emissive), but it no longer gates whether a fullscreen pass exists.

### Fullscreen PostFX (CVAR-only gating)
- Bloom (`r_bloom`, plus threshold/intensity controls)
- Godrays / light shafts (`r_godrays`, debug/quality companions)
- Tonemap / exposure / LUT stack (`r_tonemap*`, `r_autoexposure*`, `r_postfx_lut*`)
- Screen-space post toggles already centralized (`r_ssao*`, motion blur, filmgrain, vignette, chromatic aberration)

### Material/surface features (remain material-driven)
- Emissive/glow maps and emissive stage output (as lighting/post input)
- Surface rendering semantics (`surfaceparm`, blend/depth/cull, alpha test, etc.)

## Fullscreen-PostFX gates found and removed

| Effect | Gate type | Previous behavior | Status |
|---|---|---|---|
| Bloom | Mat shader keyword (`bloom` top-level + stage) | Could tag material stages with bloom mask bits, which altered bloom extract selection via velocity mask | Removed |
| Bloom | Mat shader scalar keywords (`bloom_scale`, `bloomScale`) | Per-material/stage bloom scaling metadata flowed into stage bookkeeping | Removed |
| Bloom | Shader per-drawcall define/flag (`CF_MAT_BLOOM`) | World/water wrote bloom bit into velocity alpha mask; bloom extract sampled this bit | Removed |
| Godrays | Deprecated call flags / helper API | Historical texture/surface godray emit toggles retained as dead stubs | Removed |

## Breaking change

`bloom`/`bloom_scale`/`bloomScale` material keywords no longer control fullscreen bloom participation.
Bloom pass execution is controlled by `r_bloom` only, while emissive inputs still contribute energy when present.

