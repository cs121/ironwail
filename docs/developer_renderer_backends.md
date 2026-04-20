# Developer renderer backend maturity guide

This project currently exposes three `r_backend_api` values:

- `gl` -> `OpenGL` (**implemented**)
- `vulkan` -> `Vulkan` (**experimental** or **stub**, depending on loaded plugin/backend)
- `dx12` -> `DX12` (**experimental** or **stub**, depending on loaded plugin/backend)

## Runtime status model

Runtime status labels are intentionally simple:

- `implemented`: production path expected to work end-to-end.
- `experimental`: non-stub backend is wired in, but activation/readiness can still be blocked.
- `stub`: placeholder backend; selection is blocked and engine falls back to OpenGL.

The console prints a compact help line at backend init:

- `r_backend_api help: gl=..., vulkan=..., dx12=...`

When `r_backend_api` is changed at runtime, it also logs the resolved backend and current runtime status.

## Stub selection policy

Stub backends are rejected early during `R_Backend_Select()` with explicit diagnostics.
When this happens, the engine forces fallback selection to OpenGL so users do not stay on a non-functional renderer path.

## Backend maturity milestones

A backend should only be considered production-ready when these milestones are all `yes`:

1. `init`: backend can initialize successfully.
2. `pass_callbacks`: backend provides required pass callback implementation.
3. `present`: backend can present frames.
4. `resource_translation`: backend resolves/validates framegraph resources.

The status commands (`r_backend_vulkan_status`, `r_backend_dx12_status`) now print milestone values directly.

## Important user-facing expectation

Presence of a backend name (`Vulkan`/`DX12`) in logs or plugin discovery is **not** equivalent to production readiness.
Only the runtime status plus milestone output should be used as readiness signals.
