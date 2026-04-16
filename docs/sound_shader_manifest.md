# Sound Shader Manifest

This file is the manifest-style reference for Ironwail sound shader syntax.
It documents the supported `sound` and `layer` contract, not runtime loading.

## Scope
- Source format: `.sndshd`
- Primary top-level block: `sound <name> { ... }`
- Nested block: `layer { ... }`

## Tokens
- Comments: `//` and `/* ... */`
- Boolean literals: `true`, `false`, `yes`, `no`, `on`, `off`, `1`, `0`

## `sound` block fields
- `priority` int `0..255`
- `max_instances` int `0..MAX_CHANNELS`
- `spatialize` bool
- `doppler` bool
- `lowpass_by_distance` bool
- `reverb_send` float `0..1`

## `layer` block fields
- `sample` path, repeatable, 1..`SOUNDDEF_MAX_SAMPLES_PER_LAYER`
- `bus` enum: `sfx`, `ui`, `ambient`, `music`
- `volume` float `0..4`
- `volume_random` float range `min max`
- `pitch` float `>0..8`
- `pitch_random` float range `min max`
- `loop` bool
- `chance` float `0..1`
- `delay_ms` int `>=0`
- `start_offset_ms` int `>=0`

## Rules
- A sound definition must contain 1..`SOUNDDEF_MAX_LAYERS` layers.
- A layer must contain at least one sample.
- Top-level layer keys and explicit `layer {}` blocks should not be mixed in one sound.

## Example
```text
sound debug/ui_cycle {
  priority 240
  max_instances 4
  spatialize false
  reverb_send 0.05
  sample misc/menu1.wav
  sample misc/menu2.wav
  bus ui
  volume 0.80
  pitch 1.00
}
```
