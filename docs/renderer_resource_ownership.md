# Renderer Resource Ownership

Stand: 2026-05-05

## Zielregel

- Engine/Core sieht nur neutrale Handles und neutrale Resource-Refs.
- `ref_gl.dll` besitzt und verwaltet native GL-Objekte (IDs/targets/programs/FBOs/VAOs).
- Native GL-Handles (`GLuint`, `GLenum`, `GLuint64`, `GLsync`) dürfen nur in ref_gl-spezifischen Dateien sichtbar sein.

## Ownership-Regeln

## CPU-Daten
- Ownership: Engine/Asset-System.
- Beispiele: WAD/BSP/GLTF dekodierte Bild- und Geometriedaten, material params, visibility sets.
- Regel: CPU-Daten bleiben backend-neutral bis zum Upload-Auftrag.

## GPU-Daten
- Ownership: Renderer-Backend (aktuell ref_gl).
- Engine hält nur stabile neutrale IDs/Refs, keine nativen Handles.
- Mapping neutral ID -> native object liegt exklusiv im Backend.

## Uploads
- Upload-Aufträge kommen über neutralen Contract (resource services/upload services).
- Backend entscheidet über staging/PBO/buffer-storage Details.
- Kein direktes GL-Upload-API aus Engine/Core.

## Destroy / Lifetime
- Engine steuert logische Lebensdauer (create/use/release) über neutrale Handles.
- Backend führt reale GL-Deletion aus, ggf. epoch-/frame-safe verzögert.
- Keine direkten `glDelete*` außerhalb ref_gl-only.

## Resize / Recreate
- Host liefert Größen-/Mode-Änderungen neutral.
- Backend recreated swap-target-nahe Ressourcen (FBOs/postfx/shadow atlases) selbst.
- Resize-Policies bleiben backend-intern.

## Readback / Screenshots
- Engine fordert Readback neutral an.
- Backend entscheidet Implementierungsdetails (`glReadPixels`, PBO readback etc.).
- Readback-Formatkonvertierung an neutraler Boundary dokumentieren.

## Wer darf native GL-Handles sehen?

Darf sehen:
- `Quake/src/render/gl_*` Implementierungen
- `Quake/src/render/ref_gl_*` Implementierungen
- explizit GL-spezifische bridge/helper Dateien

Darf nicht sehen (Zielzustand):
- `core/*`, `client/*`, `server/*`, `network/*`, `physics/*`, `ui/*`
- platform host code (außer strikt temporäre Übergangsstellen)
- renderer-neutrale Contract-Header

## Übergangsregeln (Legacy-Pfade)

- Legacy interner GL-Pfad bleibt vorerst bestehen, aber als Compatibility Layer markiert.
- `render_dispatch.c` bleibt breiter Adapter, bis Contract-Split abgeschlossen ist.
- `quakedef.h`-transitive GL-Sichtbarkeit ist vorübergehend erlaubt, wird aber pro Phase reduziert.
- Jede neue API an neutralen Grenzen ohne GL-Typen/Namen.

## Problematische Ressourcenliste

## textures
- Aktuell: `gltexture_t` ist GL-typed und weit sichtbar.
- Ziel: neutrales texture handle in Engine; GL-Texturobjekte nur ref_gl.

## lightmaps
- Aktuell: GL Texturverwaltung über texmgr/brush/world Pfad.
- Ziel: lightmap resource slots + backend resolve.

## shadow maps
- Aktuell: GL depth array/cubemap arrays + FBO in `gl_shadow_runtime.c`.
- Ziel: shadow resources als backend-owned opaque resources.

## FBOs
- Aktuell: global framebuf structs/GL binds.
- Ziel: nur backend-intern; engine sieht nur pass targets/ids.

## buffers
- Aktuell: `GLuint` VBO/IBO/SSBO IDs auch außerhalb strikt privater Units.
- Ziel: neutral buffer refs im Contract.

## shader programs
- Aktuell: program IDs (`GLuint`) und uniforms in GL-zentrischen Modulen.
- Ziel: engine referenziert pipeline/shader metadata nur neutral.

## VAOs
- Aktuell: global VAO bootstrap in `gl_vidsdl.c`.
- Ziel: VAO ownership vollständig ref_gl.

## postfx targets
- Aktuell: framebuffer recreation teils platform-gesteuert.
- Ziel: backend lifecycle, host liefert nur surface events.

## screenshots/readbacks
- Aktuell: GL-readback Pfade implizit.
- Ziel: neutral readback request + backend implementation.
