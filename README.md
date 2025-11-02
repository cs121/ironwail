# Ironwail

Ironwail is a high-performance fork of the GLQuake descendant [QuakeSpasm](https://sourceforge.net/projects/quakespasm/). It keeps the familiar QuakeSpasm gameplay while moving heavy lifting to the GPU (culling, lightmap updates, instancing, compute shaders, persistent buffer mapping, indirect multi-draw, bindless textures) so big modern maps stay smooth even on older hardware. The renderer is decoupled from the server using code from [QSS](https://github.com/Shpoike/Quakespasm/) via [vkQuake](https://github.com/Novum/vkQuake) to avoid physics issues at high frame rates.

## Quick start
1. Purchase and install [Quake on Steam](https://store.steampowered.com/app/2310/QUAKE/).
2. Download the [latest Ironwail release](https://github.com/andrei-drexler/ironwail/releases/latest) and unzip it into a folder that is not already a Quake installation.
3. Launch Ironwail to play the original campaign, the 2021 re-release, and any add-ons you've already downloaded. Use the new *Mods* menu to switch between installed content instantly.

## Highlights
- UI upgrades: mouse support, in-game key binding for weapons, extra video and gameplay options that apply instantly.
- Visual polish: alternative HUD styles (including Q64-inspired layouts), real-time palettization with optional dithering, classic underwater warp, lightmapped liquids, colored lightmaps with optional directional deluxemaps for smoother shading, configurable Quake 3-style lightmap overbrightening, lightstyle interpolation, higher color/depth precision, and improved z-fighting workarounds.
- Performance boosts: GPU-driven culling, compute-based lightmap updates, reduced heap usage, faster loading on jumbo maps, and an automatic frame limiter when no map is loaded.
- Quality-of-life: runs from Unicode paths and plays demanding maps like [Shibboleth](https://www.quaddicted.com/reviews/ter_shibboleth_drake_redux.html), [Raven Keep](https://www.quaddicted.com/reviews/ravenkeep.html), and [ad_tears](https://www.moddb.com/mods/arcane-dimensions) at high frame rates without manual `-heapsize` tuning.

## Wren server runtime

Ironwail now embeds the [Wren](https://wren.io) scripting VM to run server-side gameplay logic. The runtime sits next to the existing QuakeC VM; when a Wren hook is present it runs first, and QuakeC acts as a safety net if the hook is missing or explicitly asks for fallback.

### Lifecycle

1. The engine boots the Wren VM during `Host_Init`, unless `-nowren` is passed on the command line.
2. `Game.init()` executes once per server lifetime.
3. Each time a map loads, `WRENVM_ResetForNewServer()` points the runtime at the current entity lump and `Game.spawnEntities()` runs. If it throws `"NotImplemented"` or is absent, `ED_LoadFromFile()` restores QuakeC spawning.
4. Every physics tick calls `Game.startFrame(dt)` before the QuakeC `StartFrame` hook. Returning normally keeps control in Wren; aborting with `"NotImplemented"` or omitting the method falls back to QuakeC.
5. Player session changes trigger `Game.clientConnect(id)` and `Game.clientDisconnect(id)` with identical fallbacks.
6. Save/Load flows call `Game.onSave()` and `Game.onLoad()` so Wren code can persist custom state. If a strict build (`-wrenstrict`) reports an error the save is cancelled.

### Engine API (subset)

| Class | Member | Notes |
|:--|:--|:--|
| `Engine` | `time` | Current server time in seconds. |
| `Engine` | `setSkill(value)`, `getSkill()` | Mirrors the Quake `skill` cvar; changes propagate to both VMs. |
| `Engine` | `spawnFromMap()` | Parses the BSP entity lump through QuakeC for compatibility. |
| `Engine` | `randf()`, `randi(max)`, `crandom()` | Deterministic PRNG identical to QuakeC builtins. |
| `Entity` | `spawn()` | Wraps `ED_Alloc` and returns a live entity handle. |
| `Entity` | `origin`, `angles`, `velocity` | Vector properties read/write Quake entity fields. |
| `Entity` | `model`, `solid`, `movetype`, `health`, `takedamage` | Scalar property accessors. |
| `Entity` | `remove()`, `link()` | Manage entity lifetime and BSP linking. |

Additional engine services (physics tracing, sound, networking, etc.) currently raise `NotImplemented` so QuakeC continues to execute unchanged. Extending the binding surface is straightforward: add the foreign method in `wren_vm/wren_bindings.c` and the matching declaration in your Wren module.

### Fallback rules

- Hooks missing in Wren or aborting with `Fiber.abort("NotImplemented")` immediately defer to the QuakeC VM.
- Runtime errors inside a hook trigger the QuakeC fallback and print a diagnostic. With `-wrenstrict` the error is treated as fatal and the fallback is suppressed.
- Passing `-nowren` disables the Wren VM entirely; `-wrenstrict` enforces that every hook must be implemented successfully.

### Migration notes

- Scripts live under `<mod>/scripts/` and are imported via `import "q/…"`. For example the default stub resides at `game/scripts/game.wren` and is loaded as `import "q/game"`.
- Existing QuakeC mods keep working: any hook not reimplemented in Wren will continue to execute as before.
- Use the stub `Game` implementation as a starting point. Replace `Engine.spawnFromMap()` once individual entities have been ported. During migration, `-wrenstrict` helps catch forgotten fallbacks.

## System requirements

| | Minimum GPU | Recommended GPU |
|:--|:--|:--|
|NVIDIA|GeForce GT 420 ("Fermi" 2010)|GeForce GT 630 or newer ("Kepler" 2012)|
|AMD|Radeon HD 5450 ("TeraScale 2" 2009)|Radeon HD 7700 series or newer ("GCN" 2012)|
|Intel|HD Graphics 4200 ("Haswell" 2012)|HD Graphics 620 ("Kaby Lake" 2016) or newer|

Notes:
1. Requirements are based on reported OpenGL capabilities; unexpected incompatibilities may still exist.
2. macOS is currently unsupported because Ironwail targets OpenGL 4.6 for compute shaders, while Apple only provides OpenGL up to 4.1.
