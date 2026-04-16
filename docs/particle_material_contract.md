# Particle Shader Contract (MVP)

Reviewed against the current `mat_material.c` / `r_part_q3p.c` implementation.

Diese Spezifikation beschreibt den **MVP-Partikel-Shader-Vertrag** auf Basis von `mat_material.h/.c`.

## 1) Vertragsumfang pro Stage

Eine Stage ist für den Partikelpfad klassifiziert als:

- `MAT_PARTICLE_STAGE_SUPPORTED`
- `MAT_PARTICLE_STAGE_SKIPPED`
- `MAT_PARTICLE_STAGE_HARD_FAIL`

Die Bewertung erfolgt mit `Material_ClassifyParticleStage(stage, policy, reason, reason_size)`.

### Erlaubte `map`-Typen

- `map <path>` (`MAT_MAP_MAP`)
- `clampmap <path>` (`MAT_MAP_CLAMPMAP`)
- `map $whiteimage` (`MAT_MAP_WHITE`)
- `map $blackimage` (`MAT_MAP_BLACK`)

Nicht erlaubt im Partikel-MVP:

- `map $lightmap` (`MAT_MAP_LIGHTMAP`)

### Erlaubte `rgbGen`/`alphaGen`

- `identity`
- `vertex`
- `const`
- `wave`

### Erlaubte `tcMod`

- `scroll`
- `scale`
- `rotate`
- `turb`
- `stretch`

Zusätzlich gilt:

- `tcGen` muss `base` sein.
- Mehr als `countof(stage->tcmods)` wird beim Parsen verworfen, als `tcmod_overflow` markiert
  und als Hard-Fail klassifiziert.

### Erlaubte Blend-Modi

- `replace`
- `alpha` (`blend`)
- `add`
- `mult` (`filter`)
- `premult`
- `custom` nur mit gültigen `blend_src`/`blend_dst` Faktoren.

## 2) MVP-Subset (Quad/Sprite)

Der MVP umfasst die Features, die direkt im Partikel-Rendering sinnvoll sind:

- Quad/Sprite-Stage mit einer unterstützten `map`/`clampmap`.
- `animMap` (FPS + Frame-Liste).
- UV-Modulation über `tcMod`: `scroll`, `scale`, `rotate`, `turb`, `stretch`.
- Farb-/Alpha-Modulation über `rgbGen`/`alphaGen`: `identity`, `vertex`, `const`, `wave`.
- Blending über die oben definierten Modi.

Bewusst außerhalb des MVP (deferred):

- `alphaFunc`
- `tcGen environment/lightmap`
- nicht unterstützte `tcMod`-Typen
- sonstige Stage-Direktiven außerhalb der obigen Liste

## 3) Policy: tolerant vs. strict

`r_particles_material_strict` steuert das Verhalten für **nicht unterstützte, aber syntaktisch valide** Stage-Features:

- `r_particles_material_strict 0` (tolerant):
  - Ergebnis: `MAT_PARTICLE_STAGE_SKIPPED`
  - Fallback: Stage wird verworfen, Renderer nutzt sichere Standard-Partikelpfade.
- `r_particles_material_strict 1` (strict):
  - Ergebnis: `MAT_PARTICLE_STAGE_HARD_FAIL`
  - Fallback: Shader-/Stage-Kandidat gilt als nicht nutzbar für den Partikelpfad.

Unabhängig von strict/tolerant bleiben **inkonsistente Daten** Hard-Fail:

- fehlende Stage (`NULL`)
- `tcMod` overflow (`tcmod_overflow`, inkl. Warnung: `tcMod limit exceeded; ignoring extra modifiers`)
- `blendFunc` custom mit ungültigen Faktoren

## 4) Akzeptanzkriterien und Tests

Für jede Stage muss eindeutig klassifiziert sein:

- supported
- skipped
- hard-fail

### Testmatrix (Soll-Verhalten)

1. `map textures/particles/smoke` + `rgbGen vertex` + `alphaGen wave` + `tcMod scroll` → **supported**
2. `map $whiteimage` + `blendFunc add` → **supported**
3. `map $lightmap`:
   - tolerant → **skipped**
   - strict → **hard-fail**
4. `tcGen environment`:
   - tolerant → **skipped**
   - strict → **hard-fail**
5. `tcMod transform` (nicht MVP):
   - tolerant → **skipped**
   - strict → **hard-fail**
6. `blendFunc GL_ONE ???` mit ungültigem Faktor-Parsing → **hard-fail**
7. `stage == NULL` → **hard-fail**
8. mehr als 4 `tcMod`-Direktiven in einer Stage (Overflow-Flag gesetzt) → **hard-fail**

### Dokumentierter Fallback

Wenn eine Stage nicht unterstützt wird, ist der Fallback deterministisch:

- tolerant: Stage skippen und auf sichere Partikel-Defaults degradieren.
- strict: Stage als Hard-Fail markieren und nicht als MVP-kompatibel akzeptieren.
