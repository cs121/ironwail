# Renderer Backend Migration Phase 3

## 1. Ziel

Phase 3 introduces the first small renderer-neutral texture handle path without replacing `gltexture_t` or moving broad texture ownership. The test path is intentionally narrow: a `render_texture_handle_t` can be registered for one isolated texture and resolved by the GL backend/resource layer when the opt-in CVar is enabled.

The default renderer path remains the legacy `gltexture_t`/native GL texture binding path.

## 2. Nicht-Ziele

Phase 3 deliberately does **not** do any of the following:

- migrate world textures;
- migrate alias skins;
- migrate shadows or FBO attachments;
- replace or hide `gltexture_t` globally;
- clean up `quakedef.h`;
- activate Vulkan or DX12;
- change visible render output;
- introduce descriptor sets, pipeline objects, or a full texture registry redesign.

## 3. Gewählter Kandidat

Selected candidate: **PostFX LUT**.

UI/2D texture migration was reviewed as the fallback candidate, but it touches broader draw/HUD/console paths and is less isolated for a first handle test.

## 4. Warum dieser Kandidat

The PostFX LUT is the smallest safe Phase-3 candidate because:

- it is a single texture-array binding used by the postprocess pass;
- it has no shadow/depth semantics;
- it is not an FBO attachment;
- it does not participate in world or alias material binding;
- it already has a clear native GL texture lifetime in the PostFX reload path;
- the legacy native texture ID remains available for immediate fallback;
- invalid handle or resolve failure can fall back before any visible output changes.

## 5. Neuer CVar

```text
r_renderer_texture_handle_test "0"
```

Values:

- `0`: use the legacy PostFX LUT GL texture ID binding path.
- `1`: attempt the renderer-neutral `render_texture_handle_t` PostFX LUT binding path.

The default is `0`, so the default path is unchanged.

## 6. Legacy-Fallback

When `r_renderer_texture_handle_test 1` is active, the postprocess pass attempts to bind the PostFX LUT through the texture handle adapter. If the handle is invalid, cannot be resolved, has the wrong target, or cannot bind, the code logs a debug-only `R_Migration:`/`R_TextureHandle:` message and immediately binds the existing legacy LUT texture ID instead.

Debug output is gated behind either:

```text
r_renderer_migration_debug 1
r_backend_debug 1
```

## 7. Welche Dateien geändert wurden

- `Quake/src/render/texture_handles.h`: added neutral handle validity helpers and legacy GL texture adapter declarations.
- `Quake/src/render/r_resources_gl.h`: declared the GL resource/backend texture-handle registration, resolve, and bind helpers.
- `Quake/src/render/gl_backend_resources.c`: added the small GL-side texture-handle table and resolve/bind implementation for legacy `gltexture_t` and native GL textures.
- `Quake/src/render/r_postfx.c`: records a PostFX LUT `render_texture_handle_t` alongside the existing legacy native texture ID.
- `Quake/src/render/r_postfx.h`: exposes the PostFX LUT handle getter.
- `Quake/src/render/gl_rmain.c`: adds the opt-in PostFX LUT handle binding attempt with legacy fallback.
- `Quake/src/render/r_backend.c`: registers `r_renderer_texture_handle_test` and extends `r_backend_migration_audit` with Phase-3 status.
- `Quake/src/render/r_backend.h`: exposes the CVar for render code.

## 8. Welche GL-Leaks bewusst NICHT entfernt wurden

The following known leaks remain intentionally unchanged for Phase 3:

- `quakedef.h` GL leak;
- public `gltexture_t` bridge;
- `gl_rmain.c` legacy GL orchestration;
- `glprogs_t` stores `GLuint` programs;
- framegraph execution still calls legacy pass paths;
- broad world/alias/sprite/UI texture binding still uses existing legacy paths.

## 9. Testanleitung

Default path:

```text
r_renderer_texture_handle_test 0
```

Expected result: exact old PostFX LUT binding path.

Handle test path:

```text
r_renderer_texture_handle_test 1
```

Expected result: same image output. If handle registration/resolve/bind fails, the legacy PostFX LUT binding path is used automatically.

Debug:

```text
r_renderer_migration_debug 1
r_backend_debug 1
```

Useful audit command:

```text
r_backend_migration_audit
```

## 10. Go/No-Go für Phase 4

GO for Phase 4 only if:

1. Project compiles.
2. Default path remains unchanged.
3. New path is disabled by default via CVar.
4. Legacy fallback works.
5. No visible render change is observed.
6. Only one isolated texture path was migrated.
7. No world, alias, shadow, or FBO texture migration was included.
8. Vulkan/DX12 remain blocked/stubbed.
9. Audit command reports Phase-3 status.

NO-GO if any of the above fails.

Phase 4 should migrate a second small texture-handle path, likely an isolated UI/2D or decal texture path, while still avoiding world, alias, shadow, and FBO texture ownership.
