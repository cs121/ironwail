# Render-Backend Go/No-Go Gates (M1–M4 / A1–A6)

Kurz-Dokument für eine **feste Go/No-Go-Entscheidung** auf Basis reproduzierbarer `rg`-Prüfungen.

## Relevante Dateien (Direktlinks)
- [`Quake/rb_gl.c`](../Quake/rb_gl.c)
- [`Quake/render_backend.h`](../Quake/render_backend.h)
- [`Quake/backend_gl_exec.c`](../Quake/backend_gl_exec.c)
- [`Quake/gl_rmain.c`](../Quake/gl_rmain.c)
- [`Quake/gl_screen.c`](../Quake/gl_screen.c)

## Soll-Zustand (für Go)
1. **Vertragsebene steht:** `IRenderBackend` + Dispatch-/Fallback-Regeln sind zentral in `render_backend.h` definiert.
2. **State-Ownership konsistent:** A2-Hotpath-States laufen über RB-/Backend-API; direkte `gl*`-Calls sind nur als dokumentierte `GL-EXCEPTION` markiert.
3. **Dispatch aktiv:** `gl_rmain.c` und `gl_screen.c` schalten pro Block via `RBackend_DispatchBlock` um und besitzen Legacy-Fallback.
4. **Vergleichbarkeit aktiv:** Framehash-Debug (`backend_gl_exec.c`) ist vorhanden und liefert klare Mismatch-Kontexte.
5. **Go/No-Go formal:** Alle Gates unten sind "PASS" (M1–M4 + A1–A6).

---

## Gates

### M1 – Interface- und Fallback-Vertrag
**Kriterium:** Backend-API + Fallback-Regel sind zentral und explizit.

**Checks (`rg`)**
```bash
rg -n "typedef struct render_backend_vtable_s|typedef render_backend_vtable_t IRenderBackend" Quake/render_backend.h
rg -n "Fallback rule:|r_backend == 1|RBackend_DispatchBlock" Quake/render_backend.h
```

### M2 – RB-Wrapper/Baseline vorhanden
**Kriterium:** Pass-Baseline-Matrix und Pass-Wrapper in `rb_gl.c` existieren.

**Checks (`rg`)**
```bash
rg -n "Render-backend pass baseline matrix|rb_pass_info\[PASS_COUNT\]" Quake/rb_gl.c
rg -n "RB_BeginPass\(|RB_EndPass\(|RB_SetState\(" Quake/rb_gl.c
```

### M3 – Runtime-Dispatch im Renderpfad
**Kriterium:** Welt/Partikel/Alias/Fogvol/Fullscreen/UI/PostFX sind über `RBackend_DispatchBlock` angebunden.

**Checks (`rg`)**
```bash
rg -n "RBackend_DispatchBlock \(\"(world_opaque|world_alpha|particles_opaque|particles_alpha|alias|fogvol|fullscreen)\"" Quake/gl_rmain.c
rg -n "RBackend_DispatchBlock \(\"(ui|postfx)\"" Quake/gl_screen.c
rg -n "RBackend_DispatchRenderView|RBackend_DispatchUpdateScreen" Quake/gl_rmain.c Quake/gl_screen.c
```

### M4 – Framehash-Vergleich für Parität
**Kriterium:** End-of-frame Hash + Mismatch-Log mit Kontext ist vorhanden.

**Checks (`rg`)**
```bash
rg -n "RBackend_DebugCaptureEndFrameHash|backend framehash|backend framehash mismatch" Quake/backend_gl_exec.c Quake/gl_screen.c
rg -n "r_backend_framehash_(debug|scene|epsilon)" Quake/render_backend.h Quake/backend_gl_exec.c
```

### A1 – A2-Regel im Header dokumentiert
**Kriterium:** Verbot direkter `GL_*`/`GL_SetState` Calls im A2-Hotpath ist textlich festgelegt.

**Checks (`rg`)**
```bash
rg -n "A2 render-hotpath state rule|direct GL_\* / GL_SetState calls are forbidden" Quake/render_backend.h
```

### A2 – Direkte GL-Calls in Ziel-Dateien markiert
**Kriterium:** Direkte GL-Calls in `gl_rmain.c`, `gl_screen.c`, `backend_gl_exec.c` tragen `GL-EXCEPTION`-Marker.

**Checks (`rg`)**
```bash
rg -n "GL-EXCEPTION" Quake/gl_rmain.c Quake/gl_screen.c Quake/backend_gl_exec.c
```

### A3 – Dispatch-Toggles vorhanden
**Kriterium:** Backend-Toggles für Blöcke sind zentral deklariert.

**Checks (`rg`)**
```bash
rg -n "extern cvar_t r_backend(_ui|_postfx|_fullscreen|_particles|_alias|_world|_fogvol)?;" Quake/render_backend.h
```

### A4 – Globaler Umschalter erzwingt Backend/Legacy
**Kriterium:** Dispatch verwendet `r_backend` als globales Gate.

**Checks (`rg`)**
```bash
rg -n "\(int\)r_backend.value == 1|backend_globally_enabled" Quake/backend_gl_exec.c
```

### A5 – UI/PostFX Abschluss mit Hash-Capture
**Kriterium:** Nach UI/PostFX-Dispatch erfolgt End-Frame-Hash-Capture.

**Checks (`rg`)**
```bash
rg -n "SCR_PostProcess_Backend|SCR_DrawUI2D_Backend|RBackend_DebugCaptureEndFrameHash" Quake/gl_screen.c
```

### A6 – Renderview-Dispatch am Einstieg
**Kriterium:** Render-Entry nutzt zentralen `RBackend_DispatchRenderView`-Aufruf.

**Checks (`rg`)**
```bash
rg -n "RBackend_DispatchRenderView \(R_RenderView_Legacy\)" Quake/gl_rmain.c
```

---

## Go/No-Go Entscheidungsschema
- **GO**, wenn **alle** 10 Gates (M1–M4 + A1–A6) PASS sind (mind. 1 Treffer je Check, fachlich plausibel).
- **NO-GO**, wenn **mind. ein** Gate FAIL ist.

> Empfehlung: Check-Ausgaben als Build-Artefakt ablegen, damit die Entscheidung revisionssicher bleibt.
