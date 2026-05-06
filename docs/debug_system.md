# Ironwail Debug System

## Goal

Unified, modular debug output for Ironwail. Channel-based filtering,
level-based severity, zero cost when disabled. Core stays
OpenGL-agnostic; renderer subsystems own GL-specific debug code.

## Files

| File | Location | Purpose |
|------|----------|---------|
| `debug_core.h` | `Quake/src/core/` | API, channels, levels, macros |
| `debug_core.c` | `Quake/src/core/` | Implementation, CVars, init |
| `r_shadow_debug.h` | `Quake/src/render/` | Shadow-specific helper macros |

No monolithic debug file. No new GL dependencies in core.

## Debug Levels

| Level | Value | Meaning |
|-------|-------|---------|
| `DBG_LEVEL_OFF` | 0 | All debug output disabled |
| `DBG_LEVEL_ERROR` | 1 | Errors only |
| `DBG_LEVEL_WARN` | 2 | Errors + warnings |
| `DBG_LEVEL_INFO` | 3 | General info (default) |
| `DBG_LEVEL_VERBOSE` | 4 | Verbose / detailed |
| `DBG_LEVEL_TRACE` | 5 | Trace / per-frame detail |

## Debug Channels (bitmasks)

OR together to enable multiple channels at once.

| Channel | Bit | Scope |
|---------|-----|-------|
| `DBG_CH_CORE` | 0 | Engine core (host, cmd, cvar, fs) |
| `DBG_CH_RENDER` | 1 | Renderer general |
| `DBG_CH_BACKEND` | 2 | Render backend |
| `DBG_CH_GL` | 3 | OpenGL specifics |
| `DBG_CH_FRAMEGRAPH` | 4 | Framegraph |
| `DBG_CH_SHADOW` | 5 | Shadow maps |
| `DBG_CH_FOGVOL` | 6 | Fog volumes |
| `DBG_CH_TEXTURE` | 7 | Texture manager |
| `DBG_CH_SHADER` | 8 | Shader compile/link |
| `DBG_CH_FBO` | 9 | Framebuffer objects |
| `DBG_CH_CVAR` | 10 | CVar changes |
| `DBG_CH_PERF` | 11 | Performance / timing |
| `DBG_CH_AUDIO` | 12 | Audio subsystem |
| `DBG_CH_NET` | 13 | Networking |
| `DBG_CH_FILE` | 14 | File I/O |
| `DBG_CH_PHYSICS` | 15 | Physics |
| `DBG_CH_BOT` | 16 | Bot AI |
| `DBG_CH_ALL` | all | Enable all channels |

## CVars

| CVar | Default | Purpose |
|------|---------|---------|
| `debug_enable` | 0 | Master switch. 0 = all new debug output disabled |
| `dbg_level` | 3 | Minimum level to emit (0-5) |
| `dbg_channels` | 0 | Bitmask of enabled channels. 0 = none, 2147483647 = all |
| `dbg_timestamps` | 0 | Reserved for future timestamp prefixes |
| `dbg_frame_numbers` | 0 | Prepend frame number to debug output |

### Enabling shadow debug via new system:

```
debug_enable 1
dbg_channels 32
```

(32 = `1 << 5` = `DBG_CH_SHADOW`)

### Enabling shadow + framegraph:

```
debug_enable 1
dbg_channels 48
```

(48 = 32 + 16 = `DBG_CH_SHADOW | DBG_CH_FRAMEGRAPH`)

## API Macros

```c
// Severity-specific output (respects level + channel)
DBG_ERROR(channel, fmt, ...)
DBG_WARN(channel, fmt, ...)
DBG_INFO(channel, fmt, ...)
DBG_VERBOSE(channel, fmt, ...)
DBG_TRACE(channel, fmt, ...)

// Emit only once per session (hashes format string)
DBG_ONCE(channel, level, fmt, ...)

// Rate-limited output (minimum seconds between emissions)
DBG_RATE(channel, level, seconds, fmt, ...)

// Assert with message
DBG_ASSERT_MSG(condition, channel, fmt, ...)
```

### Direct function calls

If macros cause issues, use the underlying functions:

```c
if (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_SHADOW))
    DBG_Output(DBG_LEVEL_ERROR, DBG_CH_SHADOW, "shadow FBO failed: 0x%X", status);
```

## Legacy CVar Compatibility

Existing CVars (`r_shadow_log`, `r_framegraph_debug`, `r_refgl_debug`,
etc.) remain fully functional. Migrated code uses OR logic:

```c
if (r_shadow_log.value > 0.f || SHADOW_LOG_ENABLED())
    Con_Printf("shadow info...\n");
```

Both the old CVar path and the new debug channel path produce output.
No existing behavior is changed when `debug_enable` is 0.

## Ownership Rules

### Core/Engine (Quake/src/core/)

**May:**
- Provide debug API (channels, levels, output routing)
- Check channel/level filters
- Route text to Con_Printf/Con_Warning
- Set optional frame/timestamp prefixes

**Must NOT:**
- Include GL headers
- Read GL state (glGetError, glBindTexture, etc.)
- Know renderer DLL internal structures
- Dump FBO/texture/shader details directly

### Renderer/ref_gl (Quake/src/render/)

**May:**
- Use core debug API for output
- Implement GL state dumps
- Debug FBO/texture/shader/program state
- Create subsystem-specific helper headers
- Add renderer-only debug channels

**Must NOT:**
- Add GL headers to core files
- Require core to understand GL enums

## Examples

### Good usage:

```c
// Shadow channel, error level
DBG_ERROR(DBG_CH_SHADOW, "shadow atlas mismatch: expected=%u bound=%u", expected, bound);

// Rate-limited framegraph info (once per 2 seconds)
DBG_RATE(DBG_CH_FRAMEGRAPH, DBG_LEVEL_INFO, 2.0f, "FG compile: %d passes pruned", count);

// Using subsystem helper
if (SHADOW_LOG_VERBOSE_ENABLED())
    Con_Printf("shadow cascade cull: %d visible of %d\n", vis, total);
```

### Bad usage:

```c
// WRONG: Core code includes GL header to read GL state
#include <GL/gl.h>
DBG_ERROR(DBG_CH_CORE, "GL error: %u", glGetError());

// WRONG: Spammable per-frame output without rate limiting
DBG_TRACE(DBG_CH_RENDER, "drawing entity %d", ent->id);

// WRONG: Removing legacy CVar check
// Old: if (r_shadow_log.value > 0.f) Con_Printf(...)
// Don't replace with just: DBG_INFO(DBG_CH_SHADOW, ...)
// Instead, keep both paths active (see Legacy CVar Compatibility)
```

## Migration Rules

1. **Don't migrate everything at once.** Move a few sites, test, repeat.
2. **Keep legacy CVar paths active.** Use OR logic with new channel check.
3. **Use subsystem helpers** (like `r_shadow_debug.h`) when a subsystem
   has many debug sites gated by a single CVar.
4. **Prefer `DBG_RATE` or `DBG_ONCE`** for any output that could fire
   every frame.
5. **Leave `Con_Warning` for warnings** that should always appear. Only
   add `DBG_ERROR` as a secondary channel-tagged output if you want it
   routable through the debug system.
6. **Never remove existing debug CVars.** They remain the primary control
   for their subsystem.

## Future Work

- `dbg_channels` string-list parsing ("shadow,fogvol,gl")
- Per-channel level overrides
- Debug log file output (route to Con_DebugLog)
- ImGui debug overlay integration
- Subsystem helpers for framegraph, FBO, texture, shader
