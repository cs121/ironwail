# Sun System

Ironwail now uses a single runtime `SunState` (`r_sun`) as the source of truth for sun lighting inputs used by shadows, godrays, and fog/froxel.

## Source precedence

Sun source is resolved in this order:

1. **Map-authored sun** (`worldspawn` / sun entities):
   - `_sunlight` (or `sunlight`) for intensity
   - `_sunlight_color` (or `sunlight_color`) for RGB color
   - `_sun_mangle` (or `sun_mangle`) for direction angles
2. **User cvar source** (`r_shadow_sun_*`) when map does not define sun.
3. **Fallback** when neither map nor cvar source defines sun (and fallback is enabled).

Map-authored sun is never overridden by cvars.

## Fallback defaults

When fallback is active:

- virtual origin base: `(0 0 8192)`
- direction: `normalize(-0.3 -0.6 -0.7)`
- color: `(1 1 1)`
- intensity: `1.0`

On activation, the engine logs:

`Sun: no map sun found, using fallback origin (0 0 8192), dir (-0.3 -0.6 -0.7)`

## Camera-relative virtual origin

Each frame, a virtual origin is updated from camera position for stable downstream sampling:

`sun_virtual_origin = vieworg + sun_direction * (-r_sun_distance)`

This does not change the sun direction; it only stabilizes sun-referenced effects.

## Behavior knobs

- `r_sun_fallback` (default `1`): allows fallback source.
- `r_sun_force_fallback` (default `0`): forces fallback source when fallback is enabled.
- `r_sun_allow_no_sun` (default `0`): allows scenes with no sun contribution.
- `r_sun_distance` (default `10000`): camera-relative virtual sun distance.
- `r_shadow_sun_dir`, `r_shadow_sun_color`, `r_shadow_sun_intensity`: cvar sun source.

## Debug cvars

- `r_sun_debug`: prints resolved sun source/state.
- `r_shadow_csm_debug`: prints shadow projection stabilization snapping information.
- `r_godrays_debug`: shows godray diagnostics including sun marker behavior.
- `r_fog_debug_sun`: prints fog sun coupling inputs (dir/color/intensity and phase controls).
