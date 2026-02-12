# Material Shader System Feature-Parity Plan (Ironwail vs Quake III)

## Scope and intent

This plan reviews Ironwail's current material shader system and outlines the work needed to reach practical feature parity with Quake III-style shader behavior for runtime rendering.

Primary comparison baseline:
- Current Ironwail parser/runtime support metadata (`mat_shader_keyword_table`) and generated report expectations.
- Canonical Quake III shader feature families (surface-level directives, stage directives, texture coordinate generation/modification, blending/depth/alpha controls, and geometry deformation).

## Current state snapshot

Based on current implementation status tracking:

### Implemented (or mostly complete)
- Core shader discovery/parsing pipeline.
- Top-level basics: `qer_editorimage`, `surfaceparm`, `polygonOffset`, and Ironwail-specific post effects toggles/scales.
- Stage basics: `map`, `clampmap`, `depthWrite`, plus foundational support for `blendFunc`, `rgbGen`, `alphaGen`, `tcGen`, `tcMod`, and `animMap`.

### Partial support (high parity risk)
- `cull`
- `sort`
- `animMap`
- `rgbGen`
- `alphaGen`
- `blendFunc`
- `depthFunc`
- `tcGen`
- `tcMod`

### Known-unimplemented (explicitly tracked)
- `skyParms`
- `fogParms`
- `deformVertexes`
- `alphaFunc`
- `q3map_*` directives (compile-time family; usually no runtime effect but parser compatibility matters)

## Gap analysis vs Quake III behavior

## 1) Stage color/alpha generation parity

### Missing or limited behaviors
- `rgbGen` modes beyond currently supported subset (`identityLighting`, `entity`, `oneMinusEntity`, `exactVertex`, `lightingDiffuse`).
- `alphaGen` modes beyond current subset (`entity`, `oneMinusEntity`, `portal`, complete waveform parity).

### Why this matters
- These modes are heavily used by legacy Q3 content for dynamic modulation and visual authenticity.
- Missing modes often produce washed-out or static materials.

## 2) Alpha testing and depth semantics

### Missing or limited behaviors
- `alphaFunc` is not implemented.
- `depthFunc` only partial versus full Q3 usage patterns.

### Why this matters
- Grates/fences/foliage rely on alpha test semantics for correct silhouette and overdraw behavior.
- Incorrect depth semantics cause sorting artifacts and halo issues.

## 3) Texture coordinate generation/modification parity

### Missing or limited behaviors
- `tcGen vector` behavior.
- `tcMod transform` in addition to existing scroll/scale/rotate/stretch/turb support.
- Full ordering/stacking parity and edge-case handling.

### Why this matters
- Animated signage, liquids, and detail effects often depend on exact tc pipeline behavior.

## 4) Sky and fog feature parity

### Missing or limited behaviors
- `skyParms` (including skybox/farbox behavior and cloud layers as applicable).
- `fogParms` runtime use.

### Why this matters
- Map atmosphere and visibility tuning in many Q3-era maps depend on these directives.

## 5) Vertex deformation parity

### Missing or limited behaviors
- `deformVertexes` modes beyond current placeholders/tracking.

### Why this matters
- Waving banners, water-like geometry undulation, and sprite/autosprite effects are core visual signatures in Q3 content.

## 6) Parser compatibility and robustness

### Missing or limited behaviors
- Broad compatibility handling for Q3-era token variants and alias forms.
- Better diagnostics + unknown token telemetry feedback loop for mod packs.

### Why this matters
- Even when runtime ignores certain compile-time directives, parser acceptance is needed to avoid cascading failures.

## Proposed implementation roadmap

## Phase 0 - Baseline and safety net (short)
1. Add a conformance corpus from representative Q3 shaders (baseq3 + common mod styles).
2. Add parser golden tests: directive recognition, AST fields, warning counts, and stage sequencing.
3. Add render-behavior smoke scenes for alpha-tested geometry, animated tcmods, and fog/sky previews.

Deliverable: repeatable parity benchmark before feature changes.

## Phase 1 - High-impact runtime parity
1. Implement `alphaFunc` end-to-end in parser + runtime state application.
2. Complete `depthFunc` variants used by Q3 content.
3. Finish `blendFunc` factor mapping parity and validation.

Success criteria:
- Typical grates/fences render with correct cutouts.
- Fewer depth-sorting regressions in translucent/multipass materials.

## Phase 2 - Color/alpha generator completion
1. Expand `rgbGen` modes to full Q3-relevant set.
2. Expand `alphaGen` modes to full Q3-relevant set.
3. Validate wave math/ranges against Q3 reference behavior.

Success criteria:
- Dynamic/entity-driven shaders visually match expected behavior in reference scenes.

## Phase 3 - Texcoord pipeline completion
1. Implement `tcGen vector`.
2. Implement `tcMod transform`.
3. Verify ordered composition of multiple tcMods with regression tests.

Success criteria:
- Complex animated UV shaders match expected phase and motion.

## Phase 4 - World-atmosphere features
1. Implement `skyParms` runtime path (minimum viable skybox compatibility first).
2. Implement `fogParms` with map/material interaction consistency.

Success criteria:
- Sky/fog-heavy maps present correct large-scale atmosphere.

## Phase 5 - Geometry deformation
1. Complete `deformVertexes` mode coverage incrementally by usage frequency.
2. Add performance guardrails (feature toggles, budget caps, CPU/GPU path checks).

Success criteria:
- Common deform-driven assets animate correctly without major perf regressions.

## Phase 6 - Compatibility polish
1. Ensure parser accepts common `q3map_*` directives as non-fatal metadata/no-ops at runtime.
2. Improve unknown-token reporting UX and top offenders ranking in reports.
3. Update developer docs with supported/partial/unimplemented matrix and migration notes.

Success criteria:
- Large third-party shader packs load with minimal warnings and no hard parse failures.

## Priority order (if only a subset is feasible)
1. `alphaFunc` + depth/blend parity
2. `rgbGen`/`alphaGen` completion
3. `tcGen`/`tcMod` completion
4. `skyParms`/`fogParms`
5. `deformVertexes`
6. Parser no-op compatibility for compile-time directives

## Validation strategy

For each implemented directive family:
- Parser-level tests: token parsing, defaults, malformed input behavior.
- Runtime-level tests: state translation (blend/depth/alpha) and draw-path assertions.
- Visual regression captures: fixed camera screenshots for before/after comparisons in curated shader scenes.
- Unknown-token telemetry trend: verify reduction across representative shader packs.

## Risks and mitigations

- Risk: Feature work introduces regressions in existing Ironwail-specific material extensions.
  - Mitigation: Keep Q3 feature implementation isolated from extension toggles/scales; add focused regression tests.

- Risk: Full Q3 parity can increase render-path complexity.
  - Mitigation: Phase rollout by directive family; gate expensive paths with cvars where necessary.

- Risk: Some Q3 semantics are renderer-backend specific.
  - Mitigation: Match visible behavior first; document any intentional approximation.

## Definition of done for "feature parity" milestone

A practical parity milestone is reached when:
1. All currently tracked partial/known-unimplemented runtime directives needed for common Q3 content are implemented.
2. Parser accepts mainstream Q3 shader syntax without hard failures.
3. Curated visual parity scenes pass review with no high-severity deviations.
4. Material shader report shows no critical runtime directive gaps remaining in Partial/Known-unimplemented categories for target scope.
