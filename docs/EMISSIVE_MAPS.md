# Emissive Maps

Ironwail supports emissive/fullbright texture data on world and model assets.
The current renderer uses emissive information for visible shading and bloom-style
effects; a separate runtime proxy-light path exists in code but is currently disabled.

## World materials

- `mat_material.c` supports top-level `emissive`, `bloom`, `emissive_scale`, and `bloom_scale`.
- Stage-level `emissive`, `bloom`, `emissiveScale`, and `bloomScale` are also supported.
- World textures are still resolved through the normal replacement search order.

## Model textures

- Alias/MD5 skin loaders preserve emissive/fullbright companion slots.
- External replacement textures are searched under `textures/<map>/<name>` first, then `textures/<name>`.
- Model texture naming still uses the existing `progs/<model>...` pattern with `_glow` for emissive companions where present.

## Runtime note

- `r_realtimelight.c` contains an emissive proxy collector, but the emissive path is compiled disabled today.
- If you only need visible glow, use emissive/bloom material keywords and texture companions; do not expect extra dynamic lights.
