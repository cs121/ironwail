# FogVol milky/white-screen debug notes

## Likely root cause
The dominant issue was **A + C combined**:

1. Fog depth handling used only compile-time `REVERSED_Z` assumptions and an effectively infinite sky distance (`1e6`), which can drive integration distances far beyond sane scene ranges in some camera/depth setups.
2. Extinction had no runtime scale/upper guard, so wrong distances quickly forced `transmittance -> 0` (white/milky overlay depending on blend path).

## Added debug modes (`r_fogvol_debug`)
- `0`: off (normal rendering)
- `1`: legacy volume debug coloring + verbose pass logs
- `2`: raw depth sample
- `3`: linear eye depth visualization (normalized)
- `4`: fog mask/path-length visualization
- `5`: integrated extinction (sigma/tau-like visualization)
- `6`: transmittance visualization
- `7`: temporal history contribution (`R=alpha, G=history valid, B=motion factor`)
- `8`: fog shadow debug (`R=shadowed scattering, G=unshadowed estimate, B=visibility ratio`)

## Added guardrails
- Runtime depth conventions are now passed explicitly (`near/far/reverseZ/skyCutoff`) to fog shader.
- Linear depth reconstruction is clamped and NaN/Inf-safe.
- Fog sigma uses runtime density scaling and max clamp (`r_fogvol_density_scale`, `r_fogvol_sigma_max`).
- Sky integration distance now uses camera far range instead of hardcoded `1e6`.

## Useful cvars
- `r_fogvol_debug`
- `r_fogvol_density_scale`
- `r_fogvol_sigma_max`
- `r_fogvol_shadow` (default `1`): enables per-step shadow visibility term for volumetric scattering.
- `r_fogvol_shadow_samples` (default `2`, clamp `1..8`): shadow raymarch taps per fog step; higher values improve directional shadow stability at higher GPU cost.
- `r_fogvol_shadow_strength` (default `0.8`): scales shadow optical depth influence; `0` disables darkening, `1` is physical-ish, values `>1` exaggerate shadows.
- `r_fogvol_shadow_jitter` (default `1`): stochastic offset for shadow taps to reduce banding; may introduce light temporal noise.
- `r_fogvol_sun_scatter` (default `0`): adds directional sun radiance to volumetric scattering using the existing anisotropic phase; independent from `r_fogvol_shadow` visibility so it can be tuned separately.
- `r_fogvol_sun_color` (default `0 0 0`): optional override for directional fog light color. When left at `0 0 0`, fog sun color defaults to `R_GetSun` worldspawn color/intensity, and falls back to sky average tint when no sun is defined.
- Existing diagnostics retained: `r_fogvol_testvolumes`, `r_fogvol_testvolumes_dumpstate`

## Performance implications of fog shadows
- Cost scales roughly with `r_fogvol_steps * r_fogvol_shadow_samples` because each fog integration step performs an additional short visibility march.
- Recommended baseline: keep `r_fogvol_shadow_samples=2` for gameplay, use `4` for captures, and avoid `8` unless fog coverage is limited.
- If performance is tight, first reduce `r_fogvol_shadow_samples`, then disable jitter, and finally disable `r_fogvol_shadow`.
- Directional sun radiance adds only a few ALU ops per fog step and no extra texture taps; it is effectively free compared to shadow marching. Keep `r_fogvol_sun_scatter=0` for exact legacy output.
