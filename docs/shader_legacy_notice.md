# Legacy Shader Retention Notice

During shader cleanup, deprecated and unreferenced GLSL shader files were removed from `Quake/shaders`.

Removed in this pass:

- `Quake/shaders/alias_legacy.vert`
- `Quake/shaders/alias_legacy.frag`
- `Quake/shaders/dlight_composite.frag`
- `Quake/shaders/fogvol.frag`
- `Quake/shaders/fogvol_temporal.frag`
- `Quake/shaders/fogvol_upsample.frag`
- `Quake/shaders/godray_volume.vert`
- `Quake/shaders/godray_volume.frag`
- `Quake/shaders/godray_volume_debug.vert`
- `Quake/shaders/godray_volume_debug.frag`
- `Quake/shaders/godrays_source.frag`
- `Quake/shaders/godrays_source_sky.frag`
- `Quake/shaders/world_dlight.vert`
- `Quake/shaders/legacy_debug/world_dlight.frag`
- `Quake/shaders/legacy_debug/world_dlight_hybrid.frag`

The following legacy-named shader remains intentionally because it is still actively used by the runtime shader loader:

- `Quake/shaders/shadow_depth_alias.vert`

It is compiled from `Quake/opengl/gl_shaders.c` via `GL_CreateProgram(... "shadow_depth_alias.vert", "shadow_depth.frag", ...)` and is required for alias-model shadow depth rendering.
