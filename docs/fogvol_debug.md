# Fog volume / froxel black-frame debugging

## Reproduktion

1. `r_fogvol 1`
2. `r_fogvol_debug 2` für tiefes State-Logging.
3. `r_fogvol_validate 1` für harte Validierung (FBO/Viewport/Hazard).
4. `r_fogvol_black_detect 1` um Black- oder NaN-Frames per 8x8-Readback zu erkennen.
5. Optional `r_fogvol_capture_on_black 1` um bei Trigger PPM-Dumps zu schreiben.
6. Optional `r_fogvol_repro 1` für deterministischen Repro-Modus (kein jitter/noise/temporal history).

## Wichtige Debug-Ausgaben

- `FOGVOL_VALIDATE ...` enthält FBO-Bindings, Draw/Read-Buffer, Viewport/Scissor, Pipeline-State und relevante Texture-Units.
- `FOGVOL_HAZARD ...` zeigt Read/Write-Hazards zwischen input textures und draw attachments.
- `FOGVOL_BLACK ...` feuert, wenn Ausgabe komplett schwarz oder non-finite Werte enthält.
- `FOGVOL_CAPTURE wrote=...` zeigt gespeicherte PPM-Dumps.

## Root Cause (behebene Klasse)

Der Hauptauslöser für die schwarzen/glitchy Frames war NaN/Inf-Propagation im Fog-Raymarching bei degenerierten Blickstrahlen
(z. B. `normalize(worldPos - cameraPos)` mit sehr kleiner Länge und AABB-Ray-Test mit nahezu 0-Richtungsanteilen).
Diese non-finite Werte kontaminierten den temporalen Composite-Pfad und konnten frameweise das Endbild kippen.

Zusätzlich fehlte eine systematische Validierung von pass-lokalen GL-States und FBO-Vollständigkeit, wodurch fehlerhafte Zustände
nur indirekt über Symptom-Logs auffielen.

## Abhilfe

- Shader-seitige Guards gegen degenerierte Rays und non-finite Werte in fogvol + temporal shader.
- Optionaler NaN-Mask-Debugview (`FogDebugMode == 12`, pink/gelb).
- Tiefes Validierungslogging + FBO-Statuschecks pro fogvol Pass.
- Black-frame Detection + automatische Dumps der relevanten FBOs.
- Hazard-Guards bleiben aktiv und werden durch zusätzliche Validation ergänzt.
