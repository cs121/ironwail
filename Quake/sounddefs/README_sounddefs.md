# SoundDef Syntax Reference

Die Datei `syntax_manifest.json` ist die **Single Source of Truth** für die `.sndshd`-Sprache.

## Inhalt des Manifests

- unterstützte Tokens und Bool-Literale
- Top-Level-`sound`-Block inkl. erlaubter Definition-Properties
- `layer`-Block inkl. Layer-Properties
- Werttypen (`int`, `float`, `bool`, `path`, `enum`, `range`)
- Parser-/Runtime-Constraints (inkl. Grenzwerte und Symbol-Limits)
- gültige Beispieldefinitionen aus `demo_debug.sndshd`

## Kurzregel zur Blockstruktur

Innerhalb eines `sound`-Blocks gilt:

- entweder impliziter Layer über Top-Level-Layer-Keys (`sample`, `bus`, `volume`, …)
- oder explizite `layer { ... }`-Blöcke

Eine Mischung beider Formen in derselben Definition ist ungültig.
