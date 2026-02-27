# Render Backend: erlaubte direkte `gl*`-Ausnahmen

Diese Liste dokumentiert bewusst erlaubte direkte OpenGL-Aufrufe außerhalb der RB-Implementierung (`Quake/rb_gl.c`).

## Grundsatz
- Direkte `gl*`-Aufrufe im Renderpfad sind nur erlaubt, wenn es aktuell keine passende `RB_*`-Abstraktion gibt oder der Pfad explizit als Debug-/Capture-/Initialisierungshook arbeitet.
- Jede A2-relevante Ausnahme muss mit `GL-EXCEPTION` im Code markiert und hier referenziert sein.

## A2-relevanter Prüfumfang (Marker-Pflicht)
Die automatische Prüfung (`scripts/check_gl_exceptions.py`) behandelt aktuell folgende Dateien als A2-relevant:
- `Quake/backend_gl_exec.c`
- `Quake/gl_screen.c`
- `Quake/gl_rmain.c`
- `Quake/r_fogvol.c`
- `Quake/r_postfx.c`

## Ausnahmen (vollständige Bestandsaufnahme außerhalb `rb_gl.c`)

### `backend_gl_exec.c` {#backend_gl_execc}
- **Datei/Stellen:** `Quake/backend_gl_exec.c:268-271`.
- **Direkte Calls:** `glGetIntegerv`, `glPixelStorei`, `glReadPixels`.
- **Begründung:** Debug-Framehash erfasst explizit den Backbuffer inklusive temporärem Pack-Alignment-Save/Restore.
- **Ablöseplan:** `RB_ReadPixelsRGB` als zentrale Readback-Hilfe (inkl. Alignment-Handling) einführen und diesen Pfad darauf umstellen.

### `gl_screen.c` {#gl_screenc}
- **Datei/Stellen:** `Quake/gl_screen.c:1843-1844`.
- **Direkte Calls:** `glPixelStorei`, `glReadPixels`.
- **Begründung:** Screenshot-Pfad liest den finalen Framebuffer direkt aus; derzeit kein RB-Screenshot-API vorhanden.
- **Ablöseplan:** Screenshot und Framehash auf gemeinsame RB-Readback-API konsolidieren.

### `gl_rmain.c` {#gl_rmainc}
- **Datei/Stellen:** `Quake/gl_rmain.c:199-5101` (siehe direkte `gl*`-Aufrufe mit Marker in Datei).
- **Direkte Calls (Cluster):** State-Introspection (`glGet*`, `glIsEnabled`), Readback/Debug (`glReadPixels`, `glPixelStorei`, `glGetError`), Fixed-Function Debug/Overlay (`glMatrixMode`, `glOrtho`, `glDrawPixels`, etc.), Raster-/Depth-/Polygon-State (`glDepthRange`, `glPolygonMode`, `glPolygonOffset`, `glEnable`/`glDisable`, `glFinish`).
- **Begründung:** Übergangsdatei mit Legacy- und Backend-Dispatch-Pfaden; enthält Debug-Validierung, Readback-Hooks und Legacy-Stateblöcke, für die noch keine vollständigen RB-APIs existieren.
- **Ablöseplan:**
  1. Debug/Validate-Readback in dedizierte RB-Debug-API auslagern.
  2. Fixed-Function-Overlaypfade auf shaderbasierten RB-2D-Path migrieren.
  3. Verbleibende State-Queries in Backend-State-Tracker kapseln.

### `r_fogvol.c` {#r_fogvolc}
- **Datei/Stellen:** `Quake/r_fogvol.c:213-497`.
- **Direkte Calls:** überwiegend `glGet*`/`glIsEnabled` zur Sicherung/Wiederherstellung von FogVol-spezifischem GL-Zustand.
- **Begründung:** FogVol benötigt aktuell feingranulare State-Snapshots, die RB noch nicht vollständig als strukturierte Save/Restore-API bereitstellt.
- **Ablöseplan:** RB-State-Snapshot-Objekt für FogVol einführen (Capture/Restore), anschließend direkte `glGet*`-Abfragen entfernen.

### `r_postfx.c` {#r_postfxc}
- **Datei/Stellen:** `Quake/r_postfx.c:151-156`.
- **Direkte Calls:** `glGenTextures`, `glTexParameteri`.
- **Begründung:** LUT-Texturinitialisierung nutzt bisher direkten GL-Setup-Pfad ohne RB-Ressourcen-Factory.
- **Ablöseplan:** Texture-Create/Param in RB-Ressourcenlayer (`RB_CreateTexture*`, `RB_SetTextureParams`) konsolidieren.

### `gl_vidsdl.c`
- **Datei/Stellen:** `Quake/gl_vidsdl.c:534-1329`.
- **Direkte Calls:** Context-/Capabilities-/Initialisierungs-Calls (`glGetString`, `glGetIntegerv`, `glEnable`, `glBlendFunc`, etc.).
- **Begründung:** Plattform- und Kontextinitialisierung liegt bewusst außerhalb des Render-Backend-Hotpaths.
- **Ablöseplan:** Optional spätere Trennung in eigenes GL-Device-Layer; aktuell **nicht** A2-Blocker.

### `gl_ktx2.c`
- **Datei/Stellen:** `Quake/gl_ktx2.c:339-356`.
- **Direkte Calls:** Textur-Upload/Parameter (`glBindTexture`, `glTexParameteri`, `glTexImage2D`, `glGetError`).
- **Begründung:** Asset-Decode/Upload-Pfad nutzt Legacy-Texmgr-Schnittstellen ohne RB-Ressourcenabstraktion.
- **Ablöseplan:** KTX2-Uploads auf denselben RB-Texture-Factory-Pfad wie PostFX migrieren.

### `gl_texmgr.c`
- **Datei/Stellen:** `Quake/gl_texmgr.c:198-2963`.
- **Direkte Calls:** vollständiger Legacy-Texture-Manager (`glGenTextures`, `glTexImage2D`, `glTexSubImage2D`, `glGetTexImage`, ...).
- **Begründung:** zentraler Legacy-Subsystem-Baustein; große, derzeit noch nicht in RB abstrahierte Altlast.
- **Ablöseplan:** schrittweise Backend-Ressourcenmigration (create/update/readback/delete), danach Reduktion auf Wrapper oder Entfernung.

### `gl_rmisc.c`
- **Datei/Stellen:** `Quake/gl_rmisc.c:437-1451`.
- **Direkte Calls:** `glClearColor`, `glFinish`, `glGetError`.
- **Begründung:** Misc-/Lifecycle-/Synchronisationspfade außerhalb des eigentlichen A2-Draw-Hotpaths.
- **Ablöseplan:** Synchronisations-/Lifecycle-Hooks in RB-Frame-API verlagern; Fehlerabfragen zentralisieren.
