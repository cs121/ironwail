# ref_gl Phase 3 Resource Boundary Plan

Stand: 2026-05-05

Scope:
- Inkrementelle Resource-Boundary-Bereinigung ohne ABI-Bruch, ohne Legacy-GL-Pfad-Entfernung.
- Fokus auf sichtbare native Handle-Leaks außerhalb strict ref_gl-only Zielbereich.

## 1) textures / gltexture_t

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/gl_texmgr.h` | `gltexture_t.target/texnum/bindless_handle/internal_format` | `GLenum/GLuint/GLuint64` | texmgr + breite Renderunits | ref_gl texture subsystem | ja (comments/accessor candidates) | Marker + Opaque-Kandidaten ergänzt; noch nicht opaque gemacht |
| `Quake/src/render/r_world.c` | `tx->bindless_handle` etc. in bmodel calls | `GLuint64` | world renderer TU | backend texture resolve | teilweise | lokale helper (`R_WorldBindlessHandleOrFallback`) eingeführt |
| `Quake/src/render/r_sprite.c` | `draw_texture->texnum` | `GLuint` | sprite path | backend descriptor resource id | später | Blocker: draw path koppelt direkt an texture native id |
| `Quake/src/render/gl_ktx2.c` | writes `tex->target/internal_format/texnum` | `GLenum/GLuint` | loader + texmgr object | texmgr intern | später | akzeptabel in GL-only Bereich; kein Phase-3 Umbau |

## 2) lightmaps

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/r_world.c` | `lightmap_texture`, `lightmap_dir_texture` usage | gltexture pointer + bind | world renderer | backend resource slots / descriptor bindings | teilweise | weiterhin Legacy; dokumentiert, keine visuelle Änderung |
| `Quake/src/render/gl_texmgr.c` | `TexMgr_LoadLightmap`, PBO upload | direct GL upload | texmgr | texmgr/backend intern | ja (already local) | unverändert, Ownership bereits lokal in ref_gl |

## 3) world buffers

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/r_world.c` | extern `gl_bmodel_*` buffers | `GLuint` | brush/world pipeline | backend resource registry | teilweise | lokale wrappers für SSBO/UBO bind eingeführt |
| `Quake/src/render/r_world.c` | `GL_BindBufferRange` direct calls | `gl* call` | world renderer | backend helper/API | ja | über `R_WorldBindSSBO/R_WorldBindUBO` lokalisiert |
| `Quake/src/render/r_world.c` | vertex attrib setup | `gl* call` + GL enums | world renderer | backend VAO/layout layer | ja | `R_WorldSetupBModelVertexLayout` eingeführt |

## 4) particle buffers

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/r_part.c` | `R_FlushParticleBatch` VBO upload/bind/layout | `GLuint` + `gl* call` | particle renderer | backend draw packet + buffer service | teilweise | Boundary-Marker ergänzt, noch lokaler Legacy-Upload |

## 5) Q3P SSBOs

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/r_part_q3p.c` | `q3p_gpu.sim_buffer/sort_buffer/visible_count_buffer` | `GLuint` | Q3P renderer | backend resource/buffer helper | teilweise | create/bindrange über lokale helper lokalisiert |
| `Quake/src/render/r_part_q3p.c` | `GL_BindBufferRange` in GPU passes | `gl* call` | Q3P renderer | backend descriptor/service | teilweise | `Q3P_GPU_BindSSBORange` eingeführt (erste Stelle) |
| `Quake/src/render/r_part_q3p.c` | map/unmap readback | `GLsizeiptr` + GL map calls | Q3P renderer | backend readback helper | später | Blocker: compute debug/readback stark GL-spezifisch |

## 6) postfx LUT/targets

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/r_postfx.c` | `r_postfx_lut_tex` | `GLuint` | postfx module | backend resource ref | ja | owner/boundary marker ergänzt |
| `Quake/src/render/gl_rmain.c` | `framebufs.*` (scene/composite/bloom/godrays/ssao) | many native ids | ref_gl runtime | ref_gl runtime (private) | ja | private-owner marker ergänzt, keine architekturänderung |

## 7) shadow resources

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/gl_shadow_runtime.c` | `framebufs.shadow.*`, create/delete | `GLuint` + `gl*` | shadow runtime | shadow runtime (private) | ja | owner + boundary comments ergänzt |
| `Quake/src/render/r_world.c` | shadow uniform application path | program/buffer binding | world/shadow interop | backend/shadow service | später | Blocker: current pass wiring is GL-coupled |

## 8) readback/screenshot

| Datei | Symbol/Feld/Funktion | Leak-Art | aktueller Owner | Ziel-Owner | risikoarme Kapselung? | Phase-3 Aktion / Blocker |
|---|---|---|---|---|---|---|
| `Quake/src/render/gl_screen.c` | `SCR_ScreenShot_f` (`glReadPixels`) | direct GL readback | screen/UI module | backend readback request | ja (docs/marker) | boundary marker ergänzt, funktion unverändert |
| `Quake/src/render/gl_rmain.c` | autoexposure/readback helpers | direct GL framebuffer/pbo readback | ref_gl runtime | ref_gl runtime private (later service) | später | Blocker: cross-feature coupling with exposure pipeline |

## gltexture_t Opaque Migration Candidate List

Aktuelle breite Sichtbarkeit (außerhalb `gl_texmgr.c`) zeigt:
- pointer-level usage sehr häufig (`gltexture_t*` in `gl_draw.c`, `r_world.c`, `r_decals.c`, `r_alias.c`, `r_sprite.c`, `r_part_q3p.c`)
- native field usage außerhalb texmgr vor allem:
  - `bindless_handle` in `r_world.c`
  - `texnum` in `r_sprite.c`
  - `target/internal_format/texnum` writes in `gl_ktx2.c`

Kurzfristig opaque-fähige Kandidaten (niedriges Risiko, additiv):
1. `texnum` read access via `TexMgr_GetNativeTextureId(const gltexture_t*)`
2. `bindless_handle` via `TexMgr_GetBindlessHandle(const gltexture_t*, const gltexture_t *fallback)`
3. `target` via `TexMgr_GetTarget(const gltexture_t*)`

Mittelfristige Blocker:
- `r_world.c` bindless batch packing schreibt direkt in GPU call structs.
- `r_sprite.c` descriptor path nutzt native texture id direkt.
- `gl_ktx2.c` initialisiert gltexture intern direkt.

## Phase-3 durchgeführte sichere Kapselungen

- `r_world.c`
  - `R_WorldBindSSBO`, `R_WorldBindUBO` eingeführt.
  - `R_WorldSetupBModelVertexLayout` eingeführt.
  - `R_WorldBindlessHandleOrFallback` eingeführt.
- `r_part_q3p.c`
  - `Q3P_GPU_CreateStorageBuffer` eingeführt.
  - `Q3P_GPU_BindSSBORange` eingeführt.
- Marker/Ownership-Kommentare ergänzt in:
  - `gl_texmgr.h`, `gl_rmain.c`, `gl_shadow_runtime.c`, `r_postfx.c`, `gl_screen.c`, `r_part.c`, `gl_backend_resources.c`.

## Offene Blocker für spätere Phasen

- Vollständige `gltexture_t`-Opacity benötigt mehr als lokale Refactors.
- World/particle draw paths mischen noch GL-API und backend-abstrakte Calls.
- Screenshot/readback bleibt direkt GL-zentriert.
