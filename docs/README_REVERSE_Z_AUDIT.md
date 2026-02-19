# Reverse-Z / Normal-Z Audit (static code review)

This document records where **Reverse-Z** is explicitly handled in the codebase, including GLSL shader files, and where **Normal-Z** behavior still exists.

## Summary

- Reverse-Z is actively supported and appears to be the default path when `GL_ARB_clip_control` is available (`gl_clipcontrol_able == true`).
- Normal-Z fallback paths are still present in multiple places (CPU-side setup, projection/depth helpers, and shader logic).
- Any location listed under **To be checked (Normal-Z paths present)** should be validated for correctness/runtime parity.

---

## Reverse-Z found

## Engine / renderer C code

- `Quake/opengl/gl_vidsdl.c`
  - `GL_SetupState`: enables clip-control (`GL_ZERO_TO_ONE`), sets `glClearDepth(0.f)` and `glDepthFunc(GL_GEQUAL)` for reversed depth when `gl_clipcontrol_able` is true.
- `Quake/opengl/gl_rmain.c`
  - `GL_DepthRange`: switches depth ranges according to reversed vs normal depth.
  - `GL_FrustumMatrix`: builds a reversed-Z projection matrix when `gl_clipcontrol_able` is true.
  - SSAO / blur / godrays / DoF setup passes `reversed_z` flags into shader uniforms.
- `Quake/renderer/r_world.c`
  - `R_MapDepthFunc`: remaps material depth funcs based on `gl_clipcontrol_able` (reverse-depth aware).
- `Quake/renderer/r_shadow.c`
  - `R_Shadow_OrthoMatrix` and `R_Shadow_PerspectiveMatrix`: use reversed-depth matrix forms when clip-control is enabled.
- `Quake/opengl/gl_shaders.c`
  - Shader compile header injects `#define REVERSED_Z <0|1>` from `gl_clipcontrol_able`.

## GLSL / shader files

- `Quake/shaders/world.vert`
  - `#if REVERSED_Z` changes polygon-offset Z bias sign.
- `Quake/shaders/world_dlight.vert`
  - `#if REVERSED_Z` changes polygon-offset Z bias sign.
- `Quake/shaders/shadow_depth.vert`
  - `#if REVERSED_Z` changes polygon-offset Z bias sign.
- `Quake/shaders/shadow_sample.glsl`
  - `#if REVERSED_Z` branches in shadow depth compare logic (sun and dlight paths).
- `Quake/shaders/world.frag`
  - `DepthToCanonical` converts depth to a canonical form using `#if REVERSED_Z`.
- `Quake/shaders/ao_common.glsl`
  - `AO_DepthToNdc` handles depth conversion differently based on `reversedZ` runtime value.
- `Quake/shaders/ssao.frag`
  - `DepthToNdcZ`, `IsSkyDepth`, and reconstruction logic explicitly account for reversed-Z.
- `Quake/shaders/ssao_blur.frag`
  - `DepthToNdcZ`, `IsSkyDepth`, and view-depth reconstruction account for reversed-Z.
- `Quake/shaders/godrays_mask.frag`
  - `DecodeLinearDepth` and `skyVis` branch using a reversed-Z uniform flag.
- `Quake/shaders/postprocess.frag`
  - `SampleLinearDepth` uses a reversed-Z branch when `DoFParams1.z > 0.5`.

---

## To be checked (Normal-Z paths present)

These locations include explicit fallback logic for non-reversed depth (`gl_clipcontrol_able == false`, `REVERSED_Z == 0`, or equivalent runtime branches):

- `Quake/opengl/gl_vidsdl.c`
  - `GL_SetupState` normal path: `glClearDepth(1.f)`, `glDepthFunc(GL_LEQUAL)`.
- `Quake/opengl/gl_rmain.c`
  - `GL_DepthRange` normal path (`ZRANGE_VIEWMODEL` / `ZRANGE_NEAR` map to low range values).
  - `GL_FrustumMatrix` standard projection matrix branch.
- `Quake/renderer/r_world.c`
  - `R_MapDepthFunc` normal-Z return values for all mapped depth funcs.
- `Quake/renderer/r_shadow.c`
  - Non-clip-control branches in orthographic and perspective shadow matrix builders.
- `Quake/shaders/world.vert`
  - `#else` branch of `ZBIAS` sign.
- `Quake/shaders/world_dlight.vert`
  - `#else` branch of `ZBIAS` sign.
- `Quake/shaders/shadow_depth.vert`
  - `#else` branch of `ZBIAS` sign.
- `Quake/shaders/shadow_sample.glsl`
  - `#else` branch in `ShadowCompare`.
- `Quake/shaders/world.frag`
  - `DepthToCanonical` normal branch.
- `Quake/shaders/ao_common.glsl`
  - `AO_DepthToNdc` normal conversion (`depth * 2 - 1`).
- `Quake/shaders/ssao.frag`
  - `DepthToNdcZ` and `IsSkyDepth` normal-Z branches.
- `Quake/shaders/ssao_blur.frag`
  - `DepthToNdcZ` and `IsSkyDepth` normal-Z branches.
- `Quake/shaders/godrays_mask.frag`
  - normal-Z depth decode and sky threshold branch.
- `Quake/shaders/postprocess.frag`
  - normal-Z linear-depth reconstruction branch.

---

## Notes

- This audit is based on static inspection only.
- `docs/ssao.md` also documents reversed-Z handling and matches the shader implementation direction.
