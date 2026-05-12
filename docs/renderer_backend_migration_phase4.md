# Renderer Backend Migration Phase 4

## 1. Goal

Phase 4 adds a second small, isolated renderer-neutral texture handle test path using `render_texture_handle_t`. Phase 3 remains scoped to the PostFX LUT and continues to use `r_renderer_texture_handle_test`; Phase 4 uses a separate CVar so each path can be enabled, disabled, and fallback-tested independently.

## 2. Non-goals

Phase 4 does not migrate world textures, alias skins, shadow maps, FBO attachments, lightmaps, sky textures, or the global particle texture path. It does not replace `gltexture_t`, clean up `quakedef.h`, split `gl_rmain.c`, introduce Vulkan/DX12 activation, descriptor sets, or a new pipeline system, and it must not change visible rendering in the default configuration.

## 3. Selected candidate

Selected candidate: **WinQuake menu background 2D texture** (`winquakemenubg`).

## 4. Why this candidate

The WinQuake menu background texture is a tiny UI/2D texture created from static CPU data, uploaded once during draw initialization, and bound through the existing GUI draw flush path only when the WinQuake menu background style is active. It has no FBO lifetime, no shadow/depth semantics, no world or alias material binding, and no texture-manager ownership changes are required. The new handle is created beside the legacy `gltexture_t *` and only used when `r_renderer_texture_handle_test2` is enabled.

## 5. Why excluded candidates were not chosen

- Console charset: isolated and UI-only, but it is used by most text rendering and therefore has a higher visible-regression surface than one menu-background texture.
- Scrap atlas / cached pics / HUD pics: these are part of broader 2D batching and reload paths, so they are not as isolated.
- Decal texture: decal rendering is gameplay-world facing and tied to decal batching/material choices, so it is riskier than a UI-only texture.
- World textures, alias skins, shadows, FBO attachments, lightmaps, sky textures, and particle globals are explicitly out of scope for Phase 4.

## 6. New CVar `r_renderer_texture_handle_test2`

```text
r_renderer_texture_handle_test2 "0"
```

- `0`: legacy path for the Phase-4 candidate.
- `1`: attempt the neutral `render_texture_handle_t` binding path for the WinQuake menu background texture.

The existing `r_renderer_texture_handle_test` remains dedicated to the Phase-3 PostFX LUT path.

## 7. Legacy fallback

The default path remains legacy. When `r_renderer_texture_handle_test2 1` is active, the GUI draw flush attempts to bind the WinQuake menu background through the GL resource/backend texture-handle helper. If the handle is invalid or the helper cannot resolve/bind it, debug-only `R_TextureHandle:` or `R_Migration:` messages are emitted and the code immediately falls back to the existing legacy `GL_Bind` path.

Debug output is gated by either:

```text
r_renderer_migration_debug 1
r_backend_debug 1
```

## 8. Changed files

- `Quake/src/render/gl_draw.c`: stores the WinQuake menu background handle and uses it in the opt-in Phase-4 bind path.
- `Quake/src/render/gl_rmain.c`: defines the new CVar.
- `Quake/src/render/r_backend.h`: exposes the new CVar declaration.
- `Quake/src/render/r_backend.c`: registers the CVar and extends `r_backend_migration_audit`.
- `docs/renderer_backend_migration_phase4.md`: documents candidate choice, fallback, test matrix, and Phase-5 gate.

## 9. GL leaks intentionally left in place

The known migration blockers remain intentionally unchanged: `quakedef.h` still leaks GL-facing declarations into broad code, `gltexture_t` remains the public legacy bridge, `gl_rmain.c` still orchestrates legacy GL rendering, `glprogs_t` still stores `GLuint` shader program IDs, and framegraph pass execution still calls legacy/GL callbacks. Native GL texture IDs remain owned by GL resource/backend helpers for the new test path.

## 10. Test instructions

Default:

```text
r_renderer_texture_handle_test 0
r_renderer_texture_handle_test2 0
```

Expectation: exact legacy behavior.

Phase 3 only:

```text
r_renderer_texture_handle_test 1
r_renderer_texture_handle_test2 0
```

Expectation: PostFX LUT handle path only; all other texture paths remain legacy.

Phase 4 only:

```text
r_renderer_texture_handle_test 0
r_renderer_texture_handle_test2 1
```

Expectation: WinQuake menu background handle path only; all other texture paths remain legacy.

Both:

```text
r_renderer_texture_handle_test 1
r_renderer_texture_handle_test2 1
```

Expectation: both isolated handle paths are active and their legacy fallbacks operate independently.

Debug:

```text
r_renderer_migration_debug 1
r_backend_debug 1
```

Build gate:

```bash
"/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" C:/projects/ironwail/Windows/VisualStudio/ironwail.sln /t:Build /p:Configuration=Release;Platform=x64 /m
```

Smoke from `C:\Quake\rerelease` with `-condebug -nosteamapi` when a Windows runtime is available.

## 11. Go/No-Go for Phase 5

GO only if:

1. The project compiles.
2. The default path remains unchanged.
3. The Phase-3 PostFX LUT path does not regress.
4. The Phase-4 path is disabled by default through `r_renderer_texture_handle_test2 0`.
5. Legacy fallback works when the Phase-4 handle path cannot bind.
6. Exactly one second isolated texture path was migrated.
7. No world, alias, shadow, FBO, lightmap, sky, or particle-global texture path was migrated.
8. No new GL leaks were added to core/neutral structures.
9. Vulkan/DX12 remain blocked.
10. `r_backend_migration_audit` shows both Phase-3 and Phase-4 status.

Phase 5 recommendation: after Phase 3 and Phase 4 are stable, reduce public `gltexture_t` use by converting safe frontend references first, document a central legacy `gltexture_t` to `render_texture_handle_t` mapping policy, and avoid world, alias, shadow, and FBO paths until the small-handle paths have proven reliable.
