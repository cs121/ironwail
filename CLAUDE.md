# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Ironwail is a high-performance Quake engine fork of [QuakeSpasm](https://sourceforge.net/projects/quakespasm/). It prioritizes GPU-accelerated rendering over maximum compatibility, using OpenGL 4.3+ features: compute shaders for frustum culling and lightmap updates, indirect multi-draw, persistent buffer mapping, and bindless textures. macOS is not supported (requires OpenGL 4.3, which Apple deprecated after 4.1).

## Build Commands

**Linux/macOS (GNU Make):**
```sh
make --directory=Quake               # release build
make DEBUG=1 --directory=Quake       # debug build
make --jobs=4 --directory=Quake      # parallel build
```

**Linux/macOS (CMake):**
```sh
cmake -B build && cmake --build build
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

**Windows (MSVC):**
Open `Windows/VisualStudio/ironwail.sln` in Visual Studio and build, or use MSBuild:
```
msbuild Windows\VisualStudio\ironwail.sln /p:Configuration=Release /p:Platform=x64
```

**Windows (MinGW cross-compile from Linux):**
```sh
cd Quake && ./build_cross_win64-sdl2.sh
cd Quake && ./build_cross_win32-sdl2.sh
```

Required dependencies: SDL2, OpenGL. Optional: libcurl (add-on downloads), libmpg123/mad (MP3), libvorbis/tremor (OGG), opusfile, FLAC, libmikmod, libmodplug, libxmp.

There are no automated tests — verification is done via CI builds and manual game testing.

## Architecture

### Subsystem Layout

All engine source lives under `Quake/` with subdirectories for each subsystem:

| Directory | Role |
|---|---|
| `Quake/` (root files) | Core engine: host, console, cmd, cvar, filesystem, math, main loop |
| `Quake/client/` | Client-side logic: prediction, demo playback, input, post-FX |
| `Quake/server/` | Server-side physics, entity movement, networking |
| `Quake/renderer/` | High-level rendering: world, alias models, shadows, particles, decals, sky, environment lighting |
| `Quake/opengl/` | OpenGL backend: shader management, texture manager, buffer objects, screen/video |
| `Quake/sound/` | Audio system with codec plugins (WAVE, MP3, Vorbis, FLAC, Opus, tracker formats) |
| `Quake/net/` | Network layer: datagram sockets, loopback, platform-specific (WinSock vs BSD) |
| `common/` | Shared code used by both engine and tools (e.g., lightgrid) |

### Key Architectural Points

- **Decoupled renderer:** The renderer runs independently from the server tick (sourced from QSS/vkQuake) to avoid physics issues at high framerates.
- **Two-layer rendering:** `renderer/` contains high-level scene logic (what to draw); `opengl/` contains the GL API calls (how to draw). `render.h` and `glquake.h` define the boundary.
- **Entry point:** `Quake/main_sdl.c` → `Host_Init()` in `host.c` → game loop.
- **Platform abstraction:** `sys_sdl_unix.c` / `sys_sdl_win.c` for OS calls; `net/net_udp.c` / `net/net_wins.c` for sockets; `pl_linux.c` / `pl_win.c` for platform-specific helpers.
- **bc7enc.c** is the only C++ file (compiled as C++17); everything else uses C11 (`-std=gnu11`).

### Naming Conventions

- Functions use `snake_case` with a module prefix: `GL_`, `R_`, `Host_`, `Sys_`, `CL_`, `SV_`, `S_`, `NET_`
- Types use `snake_case` with `_t` suffix: `entity_t`, `dlight_t`, `client_t`
- Constants and macros use `UPPER_CASE`
- Header guards: `#ifndef MODULE_H` / `#define MODULE_H` / `#endif`
- Source files follow the same prefix pattern as their functions (e.g., `gl_rmain.c` contains `GL_RenderMain`, `r_world.c` contains `R_DrawWorld`)
