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

## System requirements

| | Minimum GPU | Recommended GPU |
|:--|:--|:--|
|NVIDIA|GeForce GT 420 ("Fermi" 2010)|GeForce GT 630 or newer ("Kepler" 2012)|
|AMD|Radeon HD 5450 ("TeraScale 2" 2009)|Radeon HD 7700 series or newer ("GCN" 2012)|
|Intel|HD Graphics 4200 ("Haswell" 2012)|HD Graphics 620 ("Kaby Lake" 2016) or newer|

Notes:
1. Requirements are based on reported OpenGL capabilities; unexpected incompatibilities may still exist.
2. macOS is currently unsupported because Ironwail targets OpenGL 4.3 for compute shaders, while Apple only provides OpenGL up to 4.1.
