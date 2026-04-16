# Async Threading

Current ownership rules:

- `gl*` calls and renderer uploads stay on the main/render thread.
- `FS_AsyncRead()` runs file reads on worker jobs and finishes on the main thread via `FS_PumpAsyncCompletions()`.
- Asset staging can run off-thread, but publish/commit remains main-thread owned.

## Core CVars

- `host_async` (`0`): master async enable.
- `host_async_fs` (`0`): async filesystem reads.
- `host_async_assets` (`0`): async asset staging.
- `host_async_workers` (`1`): worker count, clamped to CPU count.
- `host_async_max_pending` (`128`): queued job cap.
- `host_async_overflow_policy` (`sync`): overflow strategy for the async job queue.
- `host_async_overflow_block_ms` (`2`): block time when overflow policy waits.

## Queue watchdog CVars

- `host_asyncqueue_overflow_policy` (`block`): behavior when the queue is full.
- `host_asyncqueue_timeout_ms` (`2`): wait timeout before overflow handling.
- `host_asyncqueue_warn_ms` (`8`): slow-wait warning threshold.
- `host_asyncqueue_metrics` (`0`): extra queue timing metrics.

## Notes

- The async path is opt-in and should be validated with the defaults first.
- Good smoke test: enable `host_async 1; host_async_fs 1; host_async_assets 1; host_async_workers 2` and stress map loads plus repeated sound triggers.
