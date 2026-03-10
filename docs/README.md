# Ironwail docs index

This directory tracks engine-facing implementation notes and feature references.

## Current runtime feature docs
- `EMISSIVE_MAPS.md` - emissive/glow map naming and behavior.
- `async_threading.md` - async worker model and controlling CVars.
- `bspx_lumps.md` - BSPX lump handling status.
- `decal_shader_manual.md` - Decal workflow, shader syntax, keywords, manifest model, and debug usage.
- `how2use-q3-shaders.md` - current material shader workflow/API (legacy filename).
- `shadow_mapping.md` - shadow mapping runtime behavior and tuning CVars.
- `ssao.md` - SSAO conventions, debug modes, and controls.

## Design/debug notes
- `fogvol_debug_notes.md`
- `particle_debug_mvp.md`
- `particle_shader_contract.md`
- `q3p_phase0_particle_mapping.md`

The design/debug notes are intentionally implementation-oriented snapshots. They
may describe rollout phases, experiments, or diagnostics rather than user-facing
commands.
