# Fog Refactor Test Plan

## Scenarios

1. **Outdoor ground fog**
   - Global fog enabled.
   - Validate height falloff (`r_fog_height_falloff`, `r_fog_height_base`).
2. **Indoor clear / outdoor dense**
   - Place `fog_volume` (box, `blend=override`, low density) over interior.
   - Keep dense global fog outside.
3. **Light-reactive pools/cones**
   - Persistent `dlight` entities + moving dynamic lights.
   - Verify localized scattering response.
4. **Overlapping local volumes**
   - Mix `blend=add` and `blend=override` sphere/box volumes.
   - Confirm boundary behavior and noise breakup.

## Debug CVAR walkthrough

1. `r_fog_debug 1`
2. `r_fog_debug_mode 1` (density)
3. `r_fog_debug_mode 2` (transmittance)
4. `r_fog_debug_mode 3` (lighting)
5. `r_fog_debug_mode 4` (sliceZ)
6. `r_fog_debug_mode 5; r_fog_debug_slice <z>` (single slice)
7. `r_fog_debug_mode 6` (froxel grid)
8. `r_fog_debug_volume_bounds 1` (wireframe volume bounds)
9. `r_fog_validate 1` (state/FBO validation)

## Stability checks

- Move camera quickly: noise should remain world-locked (no swimming).
- Toggle `r_fog_temporal 0/1` and verify no obvious ghosting explosion.
- Validate no GL state leak warnings while switching debug modes.

## Performance checks

- Sweep:
  - `r_fog_froxel_res 0..3`
  - `r_fog_zslices 16..128`
  - `r_fog_quality 0..3`
  - `r_fog_volume_max_active 0..64`
- Record frame-time impact on representative indoor and outdoor maps.
