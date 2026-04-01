# Ironwail Lightweight Interactive Physics

## 1. Overview
This system adds a lightweight, engine-side interactive physics path for opted-in entities.

It is designed for gameplay feel, not rigid-body realism:
- push/shove from player contact
- falling/sliding against world
- simple rest/sleep behavior on floors
- basic AABB body-vs-body response and small stacks
- optional visual tilt/tipping impression

It does **not** implement a full physics engine:
- no full rigid-body rotational solver
- no OBB/SAT collision
- no joints/constraints/ragdolls
- no gravity gun/carry system

## 2. Feature Scope
- `push`: player walking into light props can move them
- `shove`: stronger player contact injects impulse into props
- `slide`: props slide along world planes and walls
- `fall`: gravity-driven falling from ledges
- `stack`: simple 2-3 body AABB stacking with iterative correction
- `tilt`: optional visual pitch/roll approximation for tipping feel

## 3. Global Enable
The feature is disabled by default.

Enable globally:
```cfg
sv_phys_interact 1
```

Disable globally:
```cfg
sv_phys_interact 0
```

When disabled, legacy server physics paths are used as before.

## 4. Supported CVars
Core:
- `sv_phys_interact` (default `0`): global on/off switch
- `sv_phys_gravity_scale` (default `1`)
- `sv_phys_friction` (default `0.8`)
- `sv_phys_restitution` (default `0.05`)
- `sv_phys_sleep_epsilon` (default `5`)
- `sv_phys_debug` (default `0`)
- `sv_phys_debug_spawn` (default `0`): one-shot debug spawn request (`1` near player, `2` in front of player), auto-resets to `0` after spawn

Player push:
- `sv_phys_player_push` (default `1`)
- `sv_phys_player_push_max` (default `140`)
- `sv_phys_player_mass_virtual` (default `90`)
- `sv_phys_player_push_vertical` (default `0`)
- `sv_phys_player_push_debug` (default `0`): logs walk-block contacts and push decisions

Body-vs-body solver:
- `sv_phys_solver_iterations` (default `4`)
- `sv_phys_penetration_slop` (default `0.05`)
- `sv_phys_pos_correct` (default `0.6`)
- `sv_phys_stack_damping` (default `0.98`)

Tilt approximation:
- `sv_phys_tilt` (default `1`)
- `sv_phys_tilt_scale` (default `10`)
- `sv_phys_angular_damping` (default `6`)
- `sv_phys_tip_threshold` (default `0.35`)

Debug smoke helper:
- `sv_phys_autospawn_test` (default `0`):
  - `1`: autospawn one test prop near the player
  - `2`: autospawn one test prop directly in front of the player

## 5. Entity Activation Paths
At least one of these can opt an entity into interactive physics:

- explicit key: `physics 1`
- classname allowlist:
  - `physics_prop`
  - `physics_box`
- high spawnflag bit: `16777216` (`1 << 24`)

Optional keys:
- `mass` (clamped to `1..500`)
- `friction` (clamped to `0..4`)
- `restitution` (clamped to `0..0.5`)
- `phys_type` preset:
  - `crate`
  - `barrel`
  - `metal`
  - `debris`

If keys are missing/invalid, safe defaults are applied and clamped.

## 6. Example Entity Definitions
Simple crate:
```map
{
"classname" "physics_prop"
"model" "progs/crate.mdl"
"solid" "2"
"movetype" "6"
"physics" "1"
"mass" "25"
"friction" "0.8"
"restitution" "0.04"
"phys_type" "crate"
}
```

Heavier metal prop:
```map
{
"classname" "physics_box"
"physics" "1"
"mass" "60"
"friction" "0.55"
"restitution" "0.02"
"phys_type" "metal"
}
```

## 7. Typical Tuning Values
- Light crate feel:
  - `mass 15..30`
  - `friction 0.7..1.0`
  - `restitution 0.02..0.08`
- Heavy prop feel:
  - `mass 45..90`
  - `friction 0.5..0.9`
  - `restitution 0.0..0.04`
- Stable stacks:
  - `sv_phys_solver_iterations 4..6`
  - `sv_phys_stack_damping 0.95..0.99`

## 8. Debugging Options
Commands:
- `phys_spawn_test [mass]`
- `phys_spawn_stack [count] [mass] [spacing]`
- `phys_spawn_front [distance] [mass]`
- `phys_list`

Useful runtime flags:
- `developer 1`
- `sv_phys_debug 1`
- `-condebug` (writes `qconsole.log`)

## 9. Known Limitations
- Collision is AABB-only; no oriented collision volumes.
- Rotation is visual approximation; collision remains axis-aligned.
- Fast objects can tunnel (no continuous collision detection).
- Player push is conservative and contact-hook based, not full momentum transfer.
- Stack stability is best for small stacks; large towers can jitter.
- Save/load persists entity state through normal edict fields; runtime sleep/tilt internals are reconstructed rather than fully serialized.

## 10. Regression Smoke Test Checklist
1. Build Release x64.
2. Copy fresh binary to `C:\Quake\rerelease\ironwail.exe`.
3. Run with:
   - `-nosteamapi -condebug +developer 1 +sv_phys_interact 1 +sv_phys_debug 1 +map start`
4. Verify no-entity case:
   - `sv_phys_interact 0` and run a normal map startup.
5. Verify simple fall/rest:
   - `sv_phys_autospawn_test 1` and confirm floor contact + sleep logs.
6. Verify basic stacks:
   - `phys_spawn_stack 3 25 36`
   - confirm plausible settle with no explosion.
7. Verify body interactions:
   - spawn multiple props and observe push/stack response.
8. Verify disable path:
   - set `sv_phys_interact 0`, restart map, ensure legacy behavior.
