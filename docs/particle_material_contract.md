# Particle Shader Contract (MVP)

This specification describes the **MVP particle shader contract** based on
`mat_material.h/.c`.

Related docs:
- Parser + API overview: [`docs/how2use-materials.md`](./how2use-materials.md)

## 1) Contract scope per stage

A stage is classified for the particle path as:

- `MAT_PARTICLE_STAGE_SUPPORTED`
- `MAT_PARTICLE_STAGE_SKIPPED`
- `MAT_PARTICLE_STAGE_HARD_FAIL`

Classification is performed with
`Material_ClassifyParticleStage(stage, policy, reason, reason_size)`.

### Allowed `map` types

- `map <path>` (`MAT_MAP_MAP`)
- `clampmap <path>` (`MAT_MAP_CLAMPMAP`)
- `map $whiteimage` (`MAT_MAP_WHITE`)
- `map $blackimage` (`MAT_MAP_BLACK`)

Not allowed in the particle MVP:

- `map $lightmap` (`MAT_MAP_LIGHTMAP`)

### Allowed `rgbGen`/`alphaGen`

- `identity`
- `vertex`
- `const`
- `wave`

### Allowed `tcMod`

- `scroll`
- `scale`
- `rotate`
- `turb`
- `stretch`

Additionally:

- `tcGen` muss `base` sein.
- Mehr als `countof(stage->tcmods)` wird beim Parsen verworfen, als `tcmod_overflow` markiert
  und als Hard-Fail klassifiziert.

### Allowed blend modes

- `replace`
- `alpha` (`blend`)
- `add`
- `mult` (`filter`)
- `premult`
- `custom` only with valid `blend_src`/`blend_dst` factors.

## 2) MVP-Subset (Quad/Sprite)

The MVP includes features that are directly useful for particle rendering:

- Quad/sprite stage with a supported `map`/`clampmap`.
- `animMap` (FPS + frame list).
- UV modulation via `tcMod`: `scroll`, `scale`, `rotate`, `turb`, `stretch`.
- Color/alpha modulation via `rgbGen`/`alphaGen`: `identity`, `vertex`, `const`,
  `wave`.
- Blending using the modes listed above.

Intentionally outside the MVP (deferred):

- `alphaFunc`
- `tcGen environment/lightmap`
- unsupported `tcMod` types
- other stage directives outside the list above

## 3) Policy: tolerant vs. strict

`r_particles_material_strict` controls behavior for **unsupported but
syntactically valid** stage features:

- `r_particles_material_strict 0` (tolerant):
  - Result: `MAT_PARTICLE_STAGE_SKIPPED`
  - Fallback: the stage is discarded; the renderer uses safe default particle
    paths.
- `r_particles_material_strict 1` (strict):
  - Result: `MAT_PARTICLE_STAGE_HARD_FAIL`
  - Fallback: the shader/stage candidate is treated as unusable for the
    particle path.

Independent of strict/tolerant policy, **inconsistent data** is always hard
fail:

- fehlende Stage (`NULL`)
- `tcMod` overflow (`tcmod_overflow`, inkl. Warnung: `tcMod limit exceeded; ignoring extra modifiers`)
- `blendFunc` custom mit ungültigen Faktoren

## 4) Acceptance criteria and tests

Each stage must be classified unambiguously as:

- supported
- skipped
- hard-fail

### Test matrix (expected behavior)

1. `map textures/particles/smoke` + `rgbGen vertex` + `alphaGen wave` + `tcMod scroll` → **supported**
2. `map $whiteimage` + `blendFunc add` → **supported**
3. `map $lightmap`:
   - tolerant → **skipped**
   - strict → **hard-fail**
4. `tcGen environment`:
   - tolerant → **skipped**
   - strict → **hard-fail**
5. `tcMod transform` (not in MVP):
   - tolerant → **skipped**
   - strict → **hard-fail**
6. `blendFunc GL_ONE ???` with invalid factor parsing → **hard-fail**
7. `stage == NULL` → **hard-fail**
8. mehr als 4 `tcMod`-Direktiven in einer Stage (Overflow-Flag gesetzt) → **hard-fail**

### Documented fallback

If a stage is unsupported, fallback behavior is deterministic:

- tolerant: skip the stage and degrade to safe particle defaults.
- strict: mark the stage as hard fail and do not accept it as MVP-compatible.

## 5) Last updated against parser behavior

This contract was cross-checked against current parser/implementation behavior
in:

- `Quake/mat_material_parse.c`
- `Quake/mat_material.c`

For parser/API usage details, see
[`docs/how2use-materials.md`](./how2use-materials.md).
