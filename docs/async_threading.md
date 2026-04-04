# Async threading ownership rules

- OpenGL uploads and all `gl*` calls stay on the main/render thread.
- File I/O can run in background workers through `FS_AsyncRead`; completion
  callbacks are pumped on the main thread via `FS_PumpAsyncCompletions()`.
- Sound asset preparation can run in worker jobs (`Jobs_Submit`), while cache
  commit remains on the main thread.

## CVars

Core toggles:
- `host_async` (default `0`): master async enable.
- `host_async_fs` (default `0`): enables async filesystem reads.
- `host_async_assets` (default `0`): enables async asset staging paths.

Worker/pending queue controls:
- `host_async_workers` (default `1`): worker thread count (clamped to CPU count).
- `host_async_max_pending` (default `128`): max queued jobs before overflow policy.
- `host_async_overflow_policy` (default `sync`): overflow strategy.
- `host_async_overflow_block_ms` (default `2`): block time used by blocking policy.

## Testing hints

1. Baseline: keep all async CVars at defaults and verify single-thread behavior.
2. Enable async path, e.g.:
   `host_async 1; host_async_fs 1; host_async_assets 1; host_async_workers 2`
3. Stress map changes and repeated sound triggers.
4. Watch for regressions and use `developer 1` logging if extra diagnostics are enabled.
