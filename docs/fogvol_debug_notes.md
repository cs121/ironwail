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

## Added guardrails
- Runtime depth conventions are now passed explicitly (`near/far/reverseZ/skyCutoff`) to fog shader.
- Linear depth reconstruction is clamped and NaN/Inf-safe.
- Fog sigma uses runtime density scaling and max clamp (`r_fogvol_density_scale`, `r_fogvol_sigma_max`).
- Sky integration distance now uses camera far range instead of hardcoded `1e6`.

## Useful cvars
- `r_fogvol_debug`
- `r_fogvol_density_scale`
- `r_fogvol_sigma_max`
- Existing diagnostics retained: `r_fogvol_testvolumes`, `r_fogvol_testvolumes_dumpstate`
