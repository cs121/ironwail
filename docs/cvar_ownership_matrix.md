# Renderer CVar Ownership Matrix

This matrix documents who owns registration, validation, and runtime consumption for renderer-facing CVars.

## Ownership + registration tables

| Subsystem | Canonical registration entry point | Validation hook style | Runtime consumers |
|---|---|---|---|
| Base render toggles (`r_norefresh`, lightmap switches, visibility toggles) | `R_Init()` in `gl_rmisc.c` via `base_cvars[]` table | per-CVar callback when needed | `gl_rmain.c`, draw passes, lightmap upload path |
| Water/underwater controls (`r_wateralpha`, `r_litwater`) | `R_Init()` in `gl_rmisc.c` via `water_cvars[]` table | `R_SetWateralpha_f` callback | water/material alpha selection + warp decision path |
| Color pipeline (`r_srgb_*`, tone controls) | `R_Init()` in `gl_rmisc.c` via `color_pipeline_cvars[]` table | texture callback (`TexMgr_SRGBTextures_f`) + runtime capability clamp | framebuffer output transform + texture upload |
| PostFX core (`r_postfx*`) | `R_PostFX_RegisterCvars()` in `r_postfx.c` | internal clamp helpers in postfx tick/update | postfx graph and event stack |
| SSAO (`r_ssao*`) | `R_SSAO_RegisterCvars()` in `r_ssao.c` | per-frame validity checks + disable fallback | SSAO generate/blur/composite passes |
| Godrays (`r_godray*`, `r_godrays*`) | `R_Godrays_RegisterCvars()` in `r_godrays.c` | enable predicates + fallback texture path | godray source/mask/shaft passes |

## Rules for adding renderer CVars

1. Add the CVar declaration close to its owning subsystem implementation.
2. Register in a table owned by the same subsystem (or subsystem-local `*_RegisterCvars` function).
3. Attach callback validation only when value changes imply immediate resource/state rebuild.
4. Keep hard capability fallbacks in runtime code paths (for example, no sRGB framebuffer support), not in registration.
5. If a CVar crosses subsystem boundaries, document the single owner here before wiring additional consumers.
