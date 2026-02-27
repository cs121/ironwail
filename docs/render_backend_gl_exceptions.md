# Render Backend: erlaubte direkte `gl*`-Ausnahmen

Diese Liste dokumentiert bewusst erlaubte direkte OpenGL-Aufrufe außerhalb der RB-Implementierung (`Quake/rb_gl.c`).

## Grundsatz
- Direkte `gl*`-Aufrufe im Renderpfad sind nur erlaubt, wenn es aktuell keine passende `RB_*`-Abstraktion gibt oder der Pfad explizit als Debug-/Capture-Hook arbeitet.
- Jede Ausnahme muss mit `GL-EXCEPTION` im Code markiert und hier referenziert sein.

## Ausnahmen

### `backend_gl_exec.c`
- **Datei/Stellen:** `Quake/backend_gl_exec.c:268-271`.
- **Direkte Calls:** `glGetIntegerv`, `glPixelStorei`, `glReadPixels`, `glPixelStorei`.
- **Begründung:** Debug-Framehash erfasst explizit den Backbuffer inklusive temporärem Pack-Alignment-Save/Restore.
- **Eigentümer:** Render Backend Maintainer.
- **Geplante Ablösung:** Einführung einer RB-Hilfsfunktion (z. B. `RB_ReadPixelsRGB`) mit internem Alignment-Handling und optionalem Hash-Pfad.

### `gl_screen.c`
- **Datei/Stellen:** `Quake/gl_screen.c:1843-1844`.
- **Direkte Calls:** `glPixelStorei`, `glReadPixels`.
- **Begründung:** Screenshot-Pfad liest den finalen Framebuffer direkt aus; derzeit kein RB-Screenshot-API vorhanden.
- **Eigentümer:** UI/Screenshot Maintainer.
- **Geplante Ablösung:** Umstellung auf zentrale RB-Readback-API, die Screenshot und Debug-Framehash gemeinsam nutzen.

### `gl_sky.c`
- **Datei/Stellen:** `Quake/gl_sky.c:782`, `Quake/gl_sky.c:796`.
- **Direkte Calls:** `glEnable(GL_STENCIL_TEST)`, `glDisable(GL_STENCIL_TEST)`.
- **Begründung:** RB bietet aktuell keine dedizierte Stencil-Test Toggle-API; Sky-Stencil-Maskierung benötigt explizites Enable/Disable.
- **Eigentümer:** World/Sky Rendering Maintainer.
- **Geplante Ablösung:** Ergänzung einer RB-API für Stencil-Test Enable/Disable (oder Pass-Konfiguration), danach Migration von `gl_sky.c`.
