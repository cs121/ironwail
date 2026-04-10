# Ironwail docs index

This directory tracks engine-facing implementation notes and feature references.

## Current runtime feature docs
- `EMISSIVE_MAPS.md` - emissive/glow map naming and behavior.
- `async_threading.md` - async worker model and controlling CVars.
- `bspx_lumps.md` - BSPX lump handling status.
- `decal_material_manual.md` - Decal workflow, material syntax, keywords, manifest model, and debug usage.
- `how2use-materials.md` - current material workflow/API.
- `model_lighting_guidelines.md` - mapper/modder best practices and legacy-safe model-lighting cvar profiles.
- `shadow_mapping.md` - shadow mapping runtime behavior and tuning CVars.
- `ssao.md` - SSAO conventions, debug modes, and controls.

## Design/debug notes
- `particle_debug_mvp.md`
- `particle_material_contract.md`
- `q3p_phase0_particle_mapping.md`

The design/debug notes are intentionally implementation-oriented snapshots. They
may describe rollout phases, experiments, or diagnostics rather than user-facing
commands.
