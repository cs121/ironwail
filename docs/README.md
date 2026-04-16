# Ironwail docs index

This directory tracks engine-facing implementation notes, feature references, and standalone manifest docs.

## Current runtime feature docs
- `EMISSIVE_MAPS.md` - emissive/glow map naming and behavior.
- `async_threading.md` - async worker model and controlling CVars.
- `bspx_lumps.md` - BSPX lump handling status.
- `decal_material_manual.md` - Decal workflow, material syntax, keywords, manifest model, and debug usage.
- `decal_shader_manifest.md` - Decal shader manifest reference.
- `how2use-materials.md` - current material workflow/API.
- `material_shader_manifest.md` - Material shader manifest reference.
- `model_lighting_guidelines.md` - mapper/modder best practices and legacy-safe model-lighting cvar profiles.
- `q3_particle_manifest.md` - q3 particle manifest reference.
- `shadow_mapping.md` - shadow mapping runtime behavior and tuning CVars.
- `sound_shader_manifest.md` - Sound shader manifest reference.
- `ssao.md` - SSAO conventions, debug modes, and controls.

## Design/debug notes
- `particle_debug_mvp.md`
- `particle_material_contract.md`
- `q3p_phase0_particle_mapping.md`

The design/debug notes are intentionally implementation-oriented snapshots. They
may describe rollout phases, experiments, or diagnostics rather than user-facing
commands.
