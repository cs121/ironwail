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
- `9`: godray shaft coupling contribution (`R=shaft energy, G=half-energy`, requires `r_fogvol_godray_coupling > 0`)

## Added guardrails
- Runtime depth conventions are now passed explicitly (`near/far/reverseZ/skyCutoff`) to fog shader.
- Linear depth reconstruction is clamped and NaN/Inf-safe.
- Fog sigma uses runtime density scaling and max clamp (`r_fogvol_density_scale`, `r_fogvol_sigma_max`).
- Sky integration distance now uses camera far range instead of hardcoded `1e6`.

## Useful cvars
- `r_fogvol_debug`
- `r_fogvol_density_scale`
- `r_fogvol_sigma_max`
- `r_fogvol_global_shadow` (default `0`): optional shadow march for the simplified global-fog pass. Keep this off for gameplay unless you specifically need sun occlusion in world fog.
- `r_fogvol_stats` (default `0`): per-frame/per-sample FogVol perf logging.
  - `0`: off
  - `1`: print every 30th frame
  - `2`: print every frame
  - Current log fields: `gpu_ms`, `passes`, `blits`, `global_passes`, `local_passes`, `clustered_passes`, `clustered_tiles`, `clustered_indices`, `steps`, `shadow_samples`
- `r_fogvol_preset` (default `2`): hard quality gate for FogVol feature combinations.
  - `0` low: local lighting detail off, shadows off, local occlusion off, lowest step budget
  - `1` medium: moderate steps, shadows capped to `1..2`, local occlusion capped to mode `1`
  - `2` high: current gameplay-quality default
  - `3` cinematic: highest controlled step budget, legacy full-feature path
- `r_fogvol_shadow` (default `1`): enables per-step shadow visibility term for volumetric scattering.
- `r_fogvol_shadow_samples` (default `2`, clamp `1..8`): shadow raymarch taps per fog step; higher values improve directional shadow stability at higher GPU cost.
- `r_fogvol_shadow_strength` (default `0.8`): scales shadow optical depth influence; `0` disables darkening, `1` is physical-ish, values `>1` exaggerate shadows.
- `r_fogvol_shadow_jitter` (default `1`): stochastic offset for shadow taps to reduce banding; may introduce light temporal noise.
- `r_fogvol_sun_scatter` (default `1`): scales directional sun radiance in volumetric scattering; independent from `r_fogvol_shadow` visibility so it can be tuned separately.
- `r_fogvol_quality` (default `1`): coarse quality tier for visual/perf tradeoff.
  - `0` low: fewer effective march steps, stronger noise subsampling.
  - `1` medium: balanced default.
  - `2` high: more effective steps and fuller noise detail.
- `r_fogvol_noise_detail_strength` (default `0.35`): controls medium/fine noise contribution for visible fog clumps.
- `r_fogvol_height_mist_strength` (default `0.3`): boosts near-ground layering from height fog.
- `r_fogvol_atmosphere` (default `0.8`): master boost for forward scattering and anti-milky compositing.
- `r_fogvol_sun_color` (default `0 0 0`): optional override for directional fog light color. When left at `0 0 0`, fog sun color defaults to `R_GetSun` worldspawn color/intensity, and falls back to sky average tint when no sun is defined.
- `r_fogvol_local_occlusion` (default `0`): optional per-local-light fog occlusion term for light-list shading (`FogLights` loop in `fogvol.frag`).
  - `0`: disabled (legacy behavior, fastest).
  - `1`: cheap signed depth test (single projected depth probe along sample→light ray).
  - `2`: multi-tap depth cone trace (higher quality, higher cost).
  - Interaction with `r_fogvol_lighting_mode` is explicit in CPU uniform setup:
    - Active only when local light-list shading runs (`mode 1` raymarch or `mode 3` froxel+detail).
    - Forced to `0` in pure froxel mode (`mode 2`) and lighting off (`mode 0`).
    - Mode `2` is downgraded to mode `1` when `r_fogvol_shadow 0`.
- `r_fogvol_godray_coupling` (default `1`): enables fogvol/godray coupling path.
  - Preferred path: inject previous-frame godray shafts into froxel lighting when `r_fogvol_froxel 1` and `r_fogvol_froxel_godrays > 0`.
  - Alternate path: sample godray shafts directly in `fogvol.frag` during march (depth-gated) when froxel path is unavailable.
- `r_fogvol_clustered` (default `0`): experimental tile-bucketed local-volume path.
  - `0`: legacy per-volume ping-pong path
  - `1`: allow clustered local rendering for supported low/medium local-volume scenes
  - Current rollout guardrails: local-only, no emissive volumes, no non-default blend modes, low/medium presets only. Unsupported cases fall back to the legacy path automatically.
- `r_fogvol_cluster_tile_size` (default `16`): screen-space tile size in pixels for clustered local-volume bucketing. Larger tiles reduce list-build overhead but increase overdraw inside each tile.
- Existing diagnostics retained: `r_fogvol_testvolumes`, `r_fogvol_testvolumes_dumpstate`

## Preset matrix

| Preset | Steps | Shadows | Local occlusion | Godray coupling | Local lighting path |
| --- | --- | --- | --- | --- | --- |
| `0` low | low (`8` global / `12` local) | forced off | forced off | forced off | at most one path (`froxel` or `light-list`) |
| `1` medium | medium (`12` global / `18` local) | on if enabled, capped to `1..2` samples | capped to mode `1` | allowed | never `froxel + light-list` together |
| `2` high | high (`>=16` global / `>=24` local) | normal | current behavior | current behavior | controlled full path |
| `3` cinematic | highest controlled budget | normal | current behavior | current behavior | controlled full path |

Notes:
- Global fog now renders on its own simplified baseline pass before local/entity fog volumes.
- The clustered local path is intentionally excluded from `high`/`cinematic` until lighting/lightgrid/occlusion parity is validated.

## Recommended settings

- Gameplay:
  - `r_fogvol_preset 1`
  - `r_fogvol_global_shadow 0`
  - `r_fogvol_shadow 0` or `1` with `r_fogvol_shadow_samples 1`
  - `r_fogvol_clustered 1` for local-volume-heavy scenes that do not rely on emissive/additive fog materials
- Cinematic / capture:
  - `r_fogvol_preset 3`
  - `r_fogvol_global_shadow 1` if the scene needs shadowed world fog
  - `r_fogvol_shadow 1`
  - `r_fogvol_shadow_samples 2..4`
  - `r_fogvol_clustered 0` until the experimental path reaches feature parity

## Known trade-offs

- Global fog is now decoupled from the local-volume loop and uses a simpler shader variant by default. The result is intentionally close rather than bit-identical; the biggest default change is that global-fog shadow marching is now opt-in via `r_fogvol_global_shadow`.
- Presets are hard gates, not hints. Some cvar combinations are intentionally clamped or disabled to avoid the old "everything at once" cost explosion.
- `r_fogvol_clustered 1` currently targets draw/blit reduction for many overlapping local volumes. It does not yet replace the legacy path for emissive fog, non-default blend modes, or high/cinematic lighting paths.

## Performance implications of fog shadows
- Cost scales roughly with `r_fogvol_steps * r_fogvol_shadow_samples` because each fog integration step performs an additional short visibility march.
- Recommended baseline: keep `r_fogvol_shadow_samples=2` for gameplay, use `4` for captures, and avoid `8` unless fog coverage is limited.
- If performance is tight, first reduce `r_fogvol_shadow_samples`, then disable jitter, and finally disable `r_fogvol_shadow`.
- Directional sun radiance adds only a few ALU ops per fog step and no extra texture taps; it is effectively free compared to shadow marching. Keep `r_fogvol_sun_scatter=0` for exact legacy output.

## Froxel lighting validation checklist
1. Enable fog lighting before profiling froxel work:
   - `r_fogvol_light 1`
   - `r_fogvol_froxel 1`
2. Ensure there are active dynamic lights intersecting fog volumes (weapon flashes, explosions, scripted dlights, or fog-only dlights).
3. Turn on froxel diagnostics and confirm injection activity:
   - `r_froxel_debug 1` (visualize first-hit froxel RGB)
   - Check console for `FROXEL inject: lights=... nonzero_voxels=... aggregate_energy=...`.
4. Compare with heat/debug mode:
   - `r_froxel_debug 2` (max-energy heat view)
   - If mode 1/2 shows little-to-no signal and counters stay near zero, froxel acceleration will not help in that scene.
5. Verify configuration sanity:
   - If `r_fogvol_froxel=1` while `r_fogvol_light=0`, froxel injection is intentionally skipped and cannot provide performance wins.

## Local-light occlusion quality/performance trade-offs
- `r_fogvol_local_occlusion 0`: no added depth work in local-light path; use as baseline for perf captures.
- `r_fogvol_local_occlusion 1`: adds one depth sample + one depth reconstruction per contributing light at each marched fog step. This is the recommended low-cost default when you need basic blocker awareness near geometry edges.
- `r_fogvol_local_occlusion 2`: adds multiple projected depth taps per contributing light for a cone-like blocker test. This reduces false positives/negatives from a single depth sample, but cost scales with both light count and fog step count; reserve for capture-quality settings or scenes with few active local lights.
- Since cost multiplies with local light-list evaluation, prefer `r_fogvol_lighting_mode 2` (pure froxel) for performance-sensitive gameplay and use modes `1`/`3` when local occlusion detail is important.

## GPU Offload Flags (neu)

- `r_fogvol_gpu_light_select 0|1`
  - `0`: CPU-Scoring/Top-K fuer Froxel-Dlight-Injection.
  - `1`: GPU-Scoring + GPU-Top-K (Compute `fogvol_dlight_score.comp` + `gpu_bitonic_pairs.comp`) mit CPU-Fallback.
- `r_fogvol_gpu_static_build 0|1`
  - `0`: CPU-Summarization des statischen Fog-Lightfields.
  - `1`: Compute-Batch fuer die Field->Light Summarization (`fogvol_static_build.comp`) mit CPU-Fallback.

Fallback:
- Bei fehlenden Compute-Ressourcen oder Buffer-Fehlern wird automatisch der bestehende CPU-Pfad verwendet.
- `r_fogvol_light_stats 1` zeigt weiterhin die bekannten Stats; zusaetzlich werden GPU-Dlight-Selection-Zaehler protokolliert.
