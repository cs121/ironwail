# Clustered Forward+ Perf Audit Checklist

## Setup
- `r_clustered_lights 1`
- `r_clustered_debug 1`
- `r_clustered_sanity_debug 1`
- `developer 1`

Expected steady-state log signature:
- `CLUSTER frame=<n> dirty=0x0 builds=0 uploads=0 ...`
- `CLSANITY ... created_allocs=<stable> ... build_ran=0`
- `RPERF ... cluster_build=0 cluster_build_ms=0.000 ...`

## A) Static camera + static light
1. Load a map and stand still for 10s with one visible dynamic light.
2. Check logs over contiguous frames.

Expected:
- First frame after entering view: one build/upload.
- Following frames: `dirty=0x0`, no build/upload, bind-only behavior.

## B) Camera motion only
1. Keep lights unchanged.
2. Rotate view slowly 360°.

Expected:
- Rebuild only when quantized view/proj keys cross threshold.
- No per-frame rebuild storm from micro float jitter.

## C) Add/remove dynamic light
1. Spawn a temporary dlight (rocket/explosion), then let it expire.

Expected:
- Build/upload only when light hash changes.
- At zero lights, clustered sampling disabled and no heavy clustered work.

## D) Resolution/viewport change
1. Change `r_mode` (or resize window).

Expected:
- One rebuild with viewport/grid dirty reasons.

## E) Cluster settings change
1. Change `r_clustered_tilesize` or `r_clustered_zslices`.

Expected:
- One rebuild with settings/grid dirty reasons.

## F) Reverse-Z / Normal-Z validation
1. Exercise both clip-control paths available on your platform.

Expected:
- Key changes are deliberate; no oscillation rebuilds.
- Lighting stays visually stable across depth mode.

## G) Stress case (many lights)
1. Spawn ~25 dynamic lights.

Expected:
- Build cost rises, but builds/uploads remain event-driven (not per-pass/per-frame by default).
- `created_allocs` remains stable (no continuous realloc loop).
