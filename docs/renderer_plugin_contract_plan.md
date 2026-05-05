# Renderer Plugin Contract Plan

Stand: 2026-05-05

## Aktueller Contract-Zustand

Vorhanden:
- Plugin ABI (`renderer_plugin.h`) mit Major/Minor und struct-size gating.
- Host services: surface/resource/upload/pipeline.
- Backend registration (`IRenderBackend`) plus entrypoint registration.
- Dynamic plugin load path (`ref_gl.dll`) vorhanden.

Noch nicht sauber getrennt:
- Legacy breite EntryPoints (`Draw_*`, `SCR_*`, `GL_Set2D`, mehrere `R_*`) laufen weiter durch den Plugin-Contract.
- `ref_gl_plugin.c` bindet an viele Engine-Interna und globale Symbole.
- Einige Ressourcenpfade bleiben GL-typed außerhalb klarer ref_gl-Privatsphäre.

## Pflicht-ABI vs optionale Extensions

## Pflicht-ABI (Core)
- ABI Major/Minor Kompatibilitätsprüfung.
- `register_backend`.
- minimale surface/resource/upload Abfragen für Frame-Ausführung.
- strukturell stabile Datenfelder (size-gated).

## Optionale Extensions
- Pipeline metadata services.
- Legacy compatibility entrypoint block.
- zusätzliche host capabilities (swap interval control, advanced readback) nur additiv.

## Legacy Compatibility EntryPoints (separat markieren)

Als kompatibilitätsgebunden markieren:
- `Draw_*`
- `SCR_*`
- `GL_Set2D`, `GL_SetCanvas*`, `GL_Push/PopCanvasColor`
- historische `R_*` Utility/Effects Hooks

Regel:
- Diese EntryPoints bleiben bis Ersatzpfad stabil ist.
- Neue neutralen Features nicht mehr in diesen Block einhängen.

## Contract-Gaps

1. Kein strikt getrennter Core-Render-Contract vs Legacy-Compat-Block.
2. Resource abstraction erlaubt noch implizite native ID-Leaks (`native_id` usage policy unklar).
3. Host/platform responsibilities sind nicht sauber von GL context/state getrennt.
4. Framegraph/pass execution mapping ist nicht vollständig backend-owned.
5. Ownership-Regeln sind nicht als verbindliche Contract-Norm codiert.

## Empfohlene Zielstruktur

## 1) Core Renderer Contract
- Neutral command/pass submission.
- Neutral resource refs/handles.
- Surface info und frame lifecycle callbacks.
- Keine GL-Namen oder GL-Typen.

## 2) Host Services
- Window/surface metrics
- Event-/focus-/resize Signale
- Optional presentation controls als capability flags

## 3) Resource Services
- create/resolve/release via neutral IDs
- upload epochs/lifetime control
- readback requests neutralisiert

## 4) Legacy Compatibility Extension
- Getrennter optionaler Block im ABI (size-gated extension struct).
- Bestehende `Draw_*`/`SCR_*`/`GL_Set2D` dort kapseln.
- Langfristig abschaltbar pro backend/plugin.

## 5) ref_gl Private Implementation
- GL context/proc/state/bootstrap
- GL object ownership/lifetime
- GL-specific optimizations (bindless, clip control, debug groups)
- keine Exposition nativer GL-Details an Engine/Core

## Inkrementeller Rollout

1. Dokumentieren und markieren (dieser Durchgang).
2. Header-Decontamination vorbereiten (`quakedef.h` transitive GL includes reduzieren).
3. `r_backend.c` als neutralen Anker absichern (keine GL-Abhängigkeit erlauben).
4. `gl_vidsdl.c` Verantwortlichkeiten splitten über Host/GL bridge.
5. Legacy-compat EntryPoints in separaten ABI-Extensionblock verschieben.
6. Erst danach schrittweise Entfernen des internen Legacy-GL-Pfads.
