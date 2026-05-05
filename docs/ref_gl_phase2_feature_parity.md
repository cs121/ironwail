# ref_gl Phase 2 Feature Parity Audit

Stand: 2026-05-05

## Scope
- `ref_gl` plugin path (`ref_gl_plugin.c` + `render_dispatch.c` + `gl_*` and coupled `r_*` render units)
- Ziel in Phase 2: Feature-Härtung und Sichtbarkeit der Lücken, ohne Legacy-GL-Pfad-Entfernung.

## EntryPoints registered by ref_gl

Source: `REFGL_FillEntryPoints()` in `Quake/src/render/ref_gl_plugin.c`.

| EntryPoint | Vorhanden | Status | Implementierung | Engine-global abhängig | Risiko |
|---|---|---|---|---|---|
| `R_Init` | ja | vollständig | direkt (gl_rmisc) | mittel | mittel |
| `R_RenderView` | ja | vollständig mit bridge-copy | direkt + bridge wrapper | hoch (`r_refdef`, bridge data) | mittel |
| `R_NewMap` | ja | vollständig | direkt | mittel | mittel |
| `R_ClearEfrags`, `R_CheckEfrags`, `R_AddEfrags` | ja | vollständig | legacy render core | mittel | mittel |
| Particle legacy FX (`R_ParseParticleEffect`, `R_RunParticleEffect`, `R_RocketTrail`, `R_EntityParticles`, `R_BlobExplosion`, `R_ParticleExplosion*`, `R_LavaSplash`, `R_TeleportSplash`) | ja | vollständig | `r_part.c` (+ optional Q3P bridge) | mittel | mittel |
| Decal hooks (`R_SpawnImpactDecal*`, `R_ClearDecals`, `R_ReloadDecals`, `R_InitDecals`) | ja | vollständig | legacy bridge + renderer | mittel | mittel |
| Player skin hooks (`R_Translate*`) | ja | vollständig | `gl_rmisc.c` | mittel | niedrig |
| `R_ClearBoundingBoxes` | ja | vollständig | `gl_rmain.c` | niedrig | niedrig |
| `R_StorePrevFrameState` | ja | vollständig | `gl_rmain.c` | mittel | niedrig |
| `R_GetParticleDebugStats` | ja | vollständig | `r_part.c` | niedrig | niedrig |
| Alpha mode (`R_Set/Get/GetEffectiveAlphaMode`) | ja | vollständig | `gl_rmain.c` | mittel | mittel |
| `R_AddStaticModels`, `R_PushDlights`, `R_ParseDlightEntities`, `R_GetLightgridSample` | ja | vollständig | world/light pipeline | mittel | mittel |
| `R_DrawPolyblendOverlay` | ja | vollständig | post/view overlay path | mittel | niedrig |
| Canvas/sample/capability queries | ja | vollständig | `gl_rmain.c` / backend caps | niedrig | niedrig |
| `R_NewGame` | ja | vollständig | renderer state reset path | mittel | niedrig |
| `R_CreateFrameBuffers`, `R_DeleteFrameBuffers` | ja | vollständig | `gl_rmain.c` + shadow submodule | mittel | mittel |
| `R_ResetDRSState`, `R_ResetGodraysStabilization` | ja | vollständig | `gl_rmain.c` | niedrig | niedrig |
| `SCR_UpdateScreen`, `SCR_Init`, `SCR_CenterPrint`, loading/modal hooks | ja | vollständig | `gl_screen.c` | mittel | mittel |
| `CL_RunParticles` | ja | vollständig | `r_part.c` | mittel | niedrig |
| 2D draw block (`Draw_*`, `GL_Set2D`, `GL_SetCanvas*`) | ja | vollständig | `gl_draw.c` | mittel | mittel |
| `Draw_Init`, `Draw_Flush` (bridge stubs) | ja | legacy bridge | `ref_gl_bridge_stubs.c` | hoch (bridge) | mittel |

## render_dispatch usage

- `render_dispatch.c` bleibt breiter **LEGACY_COMPAT_ENTRYPOINT**-Adapter.
- `RenderDispatch_SetEntryPoints` nutzt struct-size gating je Feld und übernimmt nur vorhandene Funktionszeiger.
- Fehlende kritische EntryPoints führen weiterhin zu `Sys_Error` im Host-Frontend-Adapter (bewusst konservativ).

## Feature parity by render domain

| Bereich | Status ref_gl | Pfad-Typ | Hauptabhängigkeiten | Risiko |
|---|---|---|---|---|
| World rendering | weitgehend vollständig | direkt GL (`gl_rmain.c`, `r_world.c`) | globale scene/view/light states | mittel |
| Alias models | vollständig | direkt GL (`r_alias.c`, shared glprogs) | model/skin globals | mittel |
| Sprites | vollständig | direkt GL (`r_sprite.c`) | material/texture globals | niedrig-mittel |
| Particles (classic) | vollständig | direkt GL + backend wrappers (`r_part.c`) | legacy particle globals | mittel |
| Q3 particles | vollständig (optional mode) | GL compute/SSBO (`r_part_q3p.c`) | GPU caps + shader path | mittel-hoch |
| Decals | vollständig | legacy bridge + GL draw path | material/decal globals | mittel |
| Sky | vollständig | GL (`gl_sky.c` + world path) | world state | mittel |
| Water/warp | vollständig | GL (`gl_warp.c`, warp pass in backend) | scene/postfx coupling | mittel |
| Fog / fog volumes | vollständig | GL + frame data | fog globals | mittel |
| Shadows | vollständig | GL (`gl_shadow_runtime.c`) | world/light globals + FB lifecycle | mittel-hoch |
| PostFX | weitgehend vollständig | GL (`r_postfx.c`, `gl_rmain.c`) | LUT upload, scene/depth targets | mittel |
| OIT/transparency | vollständig (alpha mode abhängig) | GL (`gl_oit.c`, blend path) | effective alpha mode + FBO setup | mittel |
| 2D/UI/HUD/console/menu | vollständig | legacy compat entrypoints (`gl_draw.c`,`gl_screen.c`) | host/console/menu globals | mittel |
| Screenshot/readback | vollständig | GL readback (`gl_screen.c`, `gl_rmain.c`) | FB binding/readback format | mittel |
| Resize handling | vollständig | backend + vid hooks (`R_Backend_OnResize`, `GL_Create/DeleteFrameBuffers`) | host window metrics + GL context | mittel |
| Texture reload/upload | vollständig | GL texmgr | transitive `gltexture_t` exposure | mittel-hoch |
| Shader reload | vorhanden | GL hot reload poll | global shader registry | mittel |
| Gamma/contrast/palette | vollständig | GL palette+post path | cvars + palette buffers | mittel |
| Lightmaps | vollständig | GL texmgr + world usage | upload PBO path | mittel |
| Dynamic lights | vollständig | GL light pipeline | pool/framedata coupling | mittel |
| Debug markers/stat counters | vorhanden | GL debug groups + dev stats | debug cvars | niedrig-mittel |

## Vollständigkeit, Legacy-Bridge, Direkt-GL

- **Direkt GL-implementiert**: world, shadows, postfx, OIT, texmgr, draw/screen, particle renderers.
- **Nur Legacy-Bridge/Compat**: weite Teile Draw/SCR forwarding über `render_dispatch` und bridge stubs.
- **Engine-global stark abhängig**: `R_RenderView` bridge sync, many `R_*` globals, menu/console/UI hooks.

## Kritische Lücken (Phase-2 Sicht)

1. Plugin boundary in `ref_gl_plugin.c` ist funktionsreich, aber stark an globale Legacy-Symbole gekoppelt.
2. Compatibility-EntryPoints sind nicht separat als optionales ABI-Extension-Block formalisiert.
3. Ressourcen-Ownership ist praktisch ref_gl-zentriert, aber native IDs (`native_id`) können über Host-Services sichtbar werden.
4. `render_dispatch` ist korrekt als Übergangslayer, bleibt aber breite Fehlerfläche bei fehlenden EntryPoints.

## Phase-2 sichere Verbesserungen umgesetzt

- Defensive ABI checks + Diagnostics in `ref_gl_plugin.c`:
  - host_api size
  - ABI major/minor compatibility
  - missing callbacks diagnostics
  - explicit register/query logging
- Marker gesetzt:
  - `REF_GL_PRIVATE`
  - `LEGACY_COMPAT_ENTRYPOINT`
  - `TODO_RENDER_CONTRACT`
  - `TODO_RESOURCE_BOUNDARY`

## Offene Follow-ups (Phase 3 Kandidaten)

- EntryPoint-Set in `core contract` vs `legacy compat extension` aufteilen.
- Bridge-Abhängigkeit in `R_RenderView` und Draw/SCR Pfaden reduzieren.
- Shader/texture/lightmap APIs schrittweise auf neutralere Handle-Oberfläche vorbereiten.
