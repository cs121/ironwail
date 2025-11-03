# Renderer DLL Preparation

This project now exposes a public renderer interface so that the rendering
backend can eventually live inside a dynamically loaded library (DLL on
Windows, `.so` on Linux, `.dylib` on macOS). The current build still links the
renderer statically, but the boundary is in place and guarded by feature flags.

## Key Files

- `src/renderer/rw_renderer.h` – public C API that declares the opaque renderer
  handle, lifecycle helpers, frame management, placeholder resource APIs, and
  facade utilities for the monolithic build.
- `src/renderer/rw_renderer_impl.c` – delegates the new API calls to the legacy
  Ironwail renderer implementation. All code still runs in-process today.
- `src/renderer/rw_renderer_facade.c` – maintains the active renderer handle and
  offers convenience entry points for existing engine code paths.

## Build Flags

| Define | Default | Description |
| --- | --- | --- |
| `RW_RENDERER_DLL_READY` | `1` | Enables the renderer abstraction layer. Disable only when testing legacy-only builds. |
| `RW_RENDERER_LINK_STATIC` | `1` | Builds against the in-tree renderer implementation. |
| `RW_RENDERER_LINK_DYNAMIC` | `0` | Placeholder for future DLL loading support. Guard new dynamic loading code paths with this flag. |

Only one of `RW_RENDERER_LINK_STATIC` or `RW_RENDERER_LINK_DYNAMIC` may be
enabled at a time.

## Current Usage

The engine creates and initializes the renderer through the new API during
`Host_Init` and tears it down from `Host_Shutdown`. Frame rendering code calls
through the facade helpers (`rw_renderer_active_begin_frame`,
`rw_renderer_active_end_frame`, `rw_renderer_active_present`) so call sites can
switch to dynamic dispatch later without structural changes.

Resource creation functions (`rw_renderer_create_texture`,
`rw_renderer_create_shader`, `rw_renderer_create_buffer`, and the matching
`*_destroy` calls) currently return `RW_ERR_GENERIC` and are marked with
`// TODO(dll)` comments. When the renderer moves to a DLL, these entry points
should forward to the exported symbols provided by that module.

## Migration Plan

1. Keep building the monolithic renderer with `RW_RENDERER_LINK_STATIC` while
   validating the API surface.
2. Introduce a dynamically linked renderer library that implements the same
   interface as declared in `rw_renderer.h`.
3. Flip the build to define `RW_RENDERER_LINK_DYNAMIC` and wire up DLL loading
   (e.g. `LoadLibrary`/`dlopen`). All TODO markers in the renderer sources
   indicate where that logic should live.
4. Once stable, remove the static implementation if desired.

## Testing

The existing build targets and test workflows remain unchanged. Running the
regular `make` or CMake build continues to produce a working executable that
exercises the renderer as before.
