# Renderer Boundary

Ziel: GL-/SDL_GL-Symbole bleiben im Renderer-Plugin-Scope.

## Erlaubte Schichten

1. **Renderer Plugin Scope**
   - `Quake/src/render/ref_gl_*`
   - `Quake/src/render/gl_backend*`
   - `Quake/src/render/r_resources_gl.*`
2. **Platform GL bootstrap seam**
   - `Quake/src/platform/gl_vidsdl.c`

Alle übrigen Host-Pfade (`core`, `client`, `server`, `network`, `ui`, `audio`, `physics`, `gamecode`, `bot`, `platform/*`) dürfen keine `GL_` oder `SDL_GL_` Symbole einführen.

## Include-Regeln

Erlaubt (Plugin-Scope):
- `#include "glquake.h"`
- `#include "gl_*.h"`

Verboten (Host-Pfade):
- `#include "glquake.h"`
- `#include "gl_*"`
- Direkte Nutzung von `GL_*` / `SDL_GL_*`

## Beispiele

Erlaubt:
```c
// Quake/src/render/ref_gl_plugin.c
#include "glquake.h"
GLenum status = GL_FRAMEBUFFER_COMPLETE;
```

Verboten:
```c
// Quake/src/core/host_cmd.c
#include "glquake.h"            // verboten
if (flags & SDL_GL_CONTEXT_DEBUG_FLAG) { ... } // verboten
```

## CI / PR-Blocker

Diese Regeln werden als Fail-Conditions ausgeführt durch:
- `python/check_no_raw_gl_calls.py`
- `python/check_gl_symbol_boundaries.py`
- `python/check_renderer_topology.py`

Jeder PR mit `GL_` oder `SDL_GL_` in Host-Pfaden wird durch `check_gl_symbol_boundaries.py` geblockt.
