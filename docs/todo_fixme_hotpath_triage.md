# TODO/FIXME Burn-down (hotpath-first)

Snapshot date: 2026-04-20.

Source scan used:

```bash
rg -n "TODO|FIXME" Quake/src
```

Current total in `Quake/src`: **90** markers (includes third-party code).

## Hotpath priority buckets

### 1) Performance-critical (do first)

- `Quake/src/audio/snd_dma.c:1753` — callback-path blocking concern (`FIXME`), potential realtime audio jitter risk.
- `Quake/src/render/gl_screen.c:2227` — render-loop call frequency note (`FIXME`), likely per-frame overhead.
- `Quake/src/render/r_world.c:317` — water-surface cull strategy (`TODO`), broad scene traversal impact.
- `Quake/src/core/cmd.c:131` — command buffer copy cost (`FIXME`), high-frequency command processing path.

### 2) Correctness / stability

- `Quake/src/server/sv_phys.c:897` and `:923` — entity push/link concerns (`FIXME`), gameplay/physics correctness.
- `Quake/src/client/view.c:197` and `:1074` — noclip angle hack sync (`FIXME`), networked behavior divergence.
- `Quake/src/server/sv_main.c:916` — protocol limits (`FIXME`), potential overflow/compat edge cases.
- `Quake/src/render/gl_sky.c:574` — impossible-state handling (`FIXME`), robustness gap.

### 3) Cleanup / maintainability

- UI/UX polish notes in `Quake/src/ui/menu_common.c`.
- Misc conversion/comment debt in `Quake/src/assets/*`, `Quake/src/core/common.c`, `Quake/src/gamecode/*`.
- Third-party TODO/FIXME markers in `Quake/src/thirdparty/*` (track separately; avoid local churn unless upgrading vendor code).

## Burn-down workflow

1. **Track only first-party files for sprint debt metrics** (`Quake/src/thirdparty` excluded from rate).
2. **Label each marker** as `perf`, `correctness`, or `cleanup` in issue tracker.
3. **Budget each milestone**: at least 1 perf and 1 correctness marker removed before any cleanup-only work.
4. **Require a micro-benchmark or smoke check** for perf-marker closures in render/audio/physics loops.
5. **Keep marker count stable or lower**: no new TODO/FIXME in hotpath files without linked issue ID.
