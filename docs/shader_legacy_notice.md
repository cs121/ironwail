# Legacy Shader Retention Notice

During shader cleanup, deprecated and unreferenced GLSL shader files were removed from `Quake/shaders`.

The following legacy-named shader remains intentionally because it is still actively used by the runtime shader loader:

- `Quake/shaders/shadow_depth_alias.vert`

It is compiled from `Quake/opengl/gl_shaders.c` via `GL_CreateProgram(... "shadow_depth_alias.vert", "shadow_depth.frag", ...)` and is required for alias-model shadow depth rendering.
