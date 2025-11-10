# iwshader Feature Support Overview

This document cross-references the iwshader material grammar with the current implementation in `r_iwshader.c` and the world renderer. It highlights which keys are parsed and which ones influence rendering, helping to spot missing functionality.

## Material keys

| Key | Parser support | Rendering impact | Notes |
| --- | --------------- | ---------------- | ----- |
| `qer_editorimage` | Stored from the shader file.【F:Quake/renderer/r_iwshader.c†L1101-L1111】 | Not used by the renderer; it is only written back out when dumping materials.【F:Quake/renderer/r_iwshader.c†L1708-L1709】 | Placeholder for tooling previews.
| `surfaceparm` | Recognised and OR-ed into `surfaceFlags`.【F:Quake/renderer/r_iwshader.c†L1113-L1127】 | No runtime usage; only emitted by the dump utility.【F:Quake/renderer/r_iwshader.c†L1710-L1713】 | Flags are parsed but not consumed.
| `cull` | Parsed into `material->cull`.【F:Quake/renderer/r_iwshader.c†L1046-L1050】 | Not consulted during rendering; only preserved for dumps.【F:Quake/renderer/r_iwshader.c†L1686-L1689】 | Back/front/none values retained.
| `sort` | Parses numeric and named buckets.【F:Quake/renderer/r_iwshader.c†L1057-L1097】 | Rendering path ignores it; value reappears only in dumps.【F:Quake/renderer/r_iwshader.c†L1691-L1706】 | Potential hook for draw ordering.
| `polygonoffset` | Accepts on/off and numeric forms.【F:Quake/renderer/r_iwshader.c†L1130-L1157】 | Not applied in renderer; dump reprints flag.【F:Quake/renderer/r_iwshader.c†L1715-L1716】 | Could control depth offset.
| `detail` | Boolean stored on the material.【F:Quake/renderer/r_iwshader.c†L1159-L1167】 | Only exported by dump tool.【F:Quake/renderer/r_iwshader.c†L1717-L1718】 | Reserved for classification.
| `strict` | Enables strict parsing when loading subsequent keys.【F:Quake/renderer/r_iwshader.c†L1170-L1179】【F:Quake/renderer/r_iwshader.c†L1255-L1265】 | Parsing-only guard to stop on errors. | Useful for diagnosing broken shaders.

## Stage keys

| Key | Parser support | Rendering impact | Notes |
| --- | --------------- | ---------------- | ----- |
| `map` | Reads texture path and normalises slashes.【F:Quake/renderer/r_iwshader.c†L552-L567】 | Used for stage texture lookup (and as a fallback for `animmap`).【F:Quake/r_world.c†L384-L402】 | Forms the basis for texture binding.
| `animmap` | Captures FPS and frame list.【F:Quake/renderer/r_iwshader.c†L1486-L1542】 | Renderer picks animated frames when sampling stage textures, modulating playback speed through `r_iwshader_animrate`.【F:Quake/r_world.c†L915-L933】【F:Quake/renderer/r_iwshader.c†L2587-L2593】 | Animation works for stages actually sampled.
| `blend` | Handles canned modes and explicit factors.【F:Quake/renderer/r_iwshader.c†L569-L633】 | Base stage alpha estimation still derives from constant blends,【F:Quake/r_world.c†L461-L465】 and cubemap environment stages now honour their declared blend mode when composited in the world shader.【F:Quake/shaders/world.frag†L640-L689】 | No general blending state changes yet.
| `rgbgen` | Supports vertex/identity/const/entity/wave.【F:Quake/renderer/r_iwshader.c†L634-L676】 | Const mode controls emissive colour tinting,【F:Quake/r_world.c†L404-L413】 while environment maps respect constant and wave outputs (including the new Fresnel wave helper) when modulating reflections.【F:Quake/shaders/world.frag†L660-L682】 | Other rgb gens remain parsed but unused elsewhere.
| `alphagen` | Supports vertex/const/mask/entity/wave.【F:Quake/renderer/r_iwshader.c†L686-L724】 | Const alpha from stage 0 drives material alpha estimation.【F:Quake/r_world.c†L461-L465】 | Non-const modes are unused so far.
| `mask` | Records the chosen channel.【F:Quake/renderer/r_iwshader.c†L736-L758】 | Applied when sampling a stage: colour channels can be isolated or forced neutral when alpha-only masks are requested.【F:Quake/shaders/world.frag†L170-L190】【F:Quake/shaders/water.frag†L155-L176】 | Dump output remains available when explicitly set.【F:Quake/renderer/r_iwshader.c†L1795-L1797】
| `tcmod` | Parses scroll/scale/rotate/translate/stretch/turb/envmap modifiers.【F:Quake/renderer/r_iwshader.c†L874-L955】 | Scroll/scale/rotate/translate build a matrix that now applies regardless of texture alignment, while stretch/turb/envmap still feed dedicated shader code for the base stage across world and water passes.【F:Quake/renderer/r_iwshader.c†L2498-L2554】【F:Quake/r_world.c†L1100-L1243】【F:Quake/shaders/world.vert†L283-L299】【F:Quake/shaders/water.vert†L232-L247】 | -
| `tcscale` / `tcoffset` | Convenience wrappers for scale/translate tcmods.【F:Quake/renderer/r_iwshader.c†L774-L820】 | Feed into the same texture-matrix pipeline as `tcmod`.【F:Quake/renderer/r_iwshader.c†L1601-L1638】 | Effect limited to supported tcmod ops.
| `tcgen` | Parses base/lightmap/environment/vector modes and stores vector rows when present.【F:Quake/renderer/r_iwshader.c†L1557-L1613】 | Stage calls use the parsed mode to build world, lightmap, environment, or vector-derived coordinates for both world and water passes.【F:Quake/r_world.c†L1100-L1133】【F:Quake/shaders/world.vert†L241-L284】【F:Quake/shaders/water.vert†L190-L233】 | Vector rows become the basis for world/screen alignment transforms.
| `tcalign` | Accepts world/object/screen.【F:Quake/renderer/r_iwshader.c†L822-L846】 | World and screen alignments now feed bespoke coordinate generation before the tcMod matrix is applied in the shaders.【F:Quake/r_world.c†L1100-L1133】【F:Quake/shaders/world.vert†L271-L284】【F:Quake/shaders/water.vert†L220-L233】 | -
| `depthwrite` / `depthtest` | Booleans or on/off tokens parsed.【F:Quake/renderer/r_iwshader.c†L886-L934】 | Stage 0 overrides flip GL depth testing/writing around brush draws.【F:Quake/r_world.c†L392-L443】 | Currently interpreted as material-wide switches via the base stage.【F:Quake/r_world.c†L589-L640】
| `colormask` | Converts string to mask enum.【F:Quake/renderer/r_iwshader.c†L921-L934】 | No runtime usage; dump prints explicit mask value.【F:Quake/renderer/r_iwshader.c†L1801-L1804】 | Colour-write control not wired up.
| `emissive` | Toggles emissive stage flag.【F:Quake/renderer/r_iwshader.c†L848-L858】 | Renderer searches for the first emissive stage, uses its texture, colour, and tex matrix.【F:Quake/r_world.c†L467-L504】 | Primary feature beyond base stage.
| `clamp` | Boolean parsed for each stage.【F:Quake/renderer/r_iwshader.c†L860-L870】 | Not applied to GL sampler state; only preserved during dump.【F:Quake/renderer/r_iwshader.c†L1799-L1800】 | Would require texture parameter tweaks.
| `alpha2coverage` | Stores boolean flag.【F:Quake/renderer/r_iwshader.c†L995-L1006】 | Not used in rendering; dump records the value.【F:Quake/renderer/r_iwshader.c†L1803-L1805】 | Coverage-to-alpha is not toggled yet.

## Implemented runtime behaviour

The renderer currently inspects only a small subset of stage data when drawing brush models:

* Base stage texture coordinates honour `tcgen` modes and `tcalign` world/screen options before running the tcMod matrix produced by `IW_StageTexMatrix`.【F:Quake/renderer/r_iwshader.c†L2498-L2554】【F:Quake/r_world.c†L1100-L1243】【F:Quake/shaders/world.vert†L241-L284】【F:Quake/shaders/water.vert†L190-L233】
* The first stage’s blend/alpha settings are sampled to derive a constant material alpha (used for transparent sorting and draw submission).【F:Quake/r_world.c†L458-L465】
* Emissive stages contribute texture, colour, and tcmods to the emissive pass.【F:Quake/r_world.c†L467-L504】
* Environment-map stages bind cubemap textures, tint them via their rgbgen settings (including Fresnel modulation), and blend them into the lit base colour using the declared blend mode.【F:Quake/r_world.c†L1189-L1246】【F:Quake/shaders/world.vert†L142-L176】【F:Quake/shaders/world.frag†L640-L689】
* Animated texture paths advance according to the parsed `animmap` metadata, scaled by `r_iwshader_animrate`, whenever those stages are sampled.【F:Quake/renderer/r_iwshader.c†L1486-L1542】【F:Quake/r_world.c†L915-L933】【F:Quake/renderer/r_iwshader.c†L2587-L2593】

Other parsed properties are retained so that `r_iwshader_dump` can recreate the original shader text, but they do not currently change rendering behaviour.【F:Quake/renderer/r_iwshader.c†L1680-L1834】
