# Async threading ownership rules

- OpenGL uploads and all `gl*` calls remain on the main/render thread.
- File I/O can run in the background worker through `FS_AsyncRead`; callbacks are pumped on the main thread via `FS_PumpAsyncCompletions()`.
- Sound asset preparation can run in worker jobs (`Jobs_Submit`), while cache commit stays on the main thread.

## CVars

- `host_async` (default `0`): master toggle for background jobs.
- `host_async_fs` (default `0`): enables async filesystem reads.
- `host_async_assets` (default `0`): enables async asset staging (currently sound pilot path).

## Testing hints

1. Baseline: keep all CVars at `0` and verify behavior matches single-thread path.
2. Enable `host_async 1; host_async_fs 1; host_async_assets 1`.
3. Switch maps quickly and trigger repeated weapon/fire sounds.
4. Watch for regressions and check `developer 1` logging if additional diagnostics are added.
