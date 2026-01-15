# Ironwail AI Coding Guidelines

## Architecture Overview
Ironwail is a high-performance Quake engine fork based on QuakeSpasm, focusing on GPU-accelerated rendering for modern maps. Key components:
- **Client Rendering (gl_*.c)**: OpenGL 4.3 renderer with compute shaders, decoupled from server for performance
- **Game Logic (pr_*.c, host.c)**: Handles QuakeC progs execution, entity management
- **Networking (net_*.c)**: UDP-based multiplayer support
- **Audio (bgmusic.c)**: Background music and sound effects
- **Common Utilities (common/)**: Shared code like lightgrid for lighting calculations

Data flows from BSP/map loading through rendering pipeline, with lightmaps and dynamic lights processed on GPU.

## Build Workflows
- **Cross-platform**: Use CMake (`cmake .. && make`) from build directory
- **Windows**: Run `build.bat` for Visual Studio MSBuild compilation
- **Dependencies**: Requires SDL2, OpenGL 4.3+, optional audio libs (mpg123, flac, etc.)
- **Shaders**: GLSL files in `Quake/shaders/` loaded at runtime; edit and test in-game

## Coding Patterns
- **Includes**: Always `#include "quakedef.h"` first for common definitions
- **Naming**: Functions prefixed by module (e.g., `R_` for render, `CL_` for client, `PR_` for progs)
- **CVars**: Use `cvar_t` for configurable variables; extern in headers, define in .c files
- **Memory**: Use hunk allocator for temporary data; careful with heap for large assets
- **Platform Code**: Separate implementations in `pl_*.c` files (win, linux, osx)
- **Extensions**: Emissive maps use `_emissive` suffix; lightgrid for advanced lighting

## Key Files
- `host.c`: Main game loop and server/client coordination
- `gl_rmain.c`: Core rendering entry point
- `gl_rlight.c`: Dynamic and static lighting calculations
- `pr_exec.c`: QuakeC virtual machine execution
- `CMakeLists.txt`: Build configuration with platform-specific excludes

## Integration Points
- **Mods**: Support via `Mods` menu; add-ons in game directories
- **Steam Content**: Auto-detects Quake 2021 release for zero-setup play
- **External Textures**: Loaded from `textures/<map>/` or `textures/` directories</content>
<parameter name="filePath">c:\Quake\source\ironwail\.github\copilot-instructions.md