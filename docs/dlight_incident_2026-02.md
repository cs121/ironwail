# Dynamic Lights Incident: additive pass unintentionally gated behind `r_dlight_style`

## Root cause

Dynamic lights were still spawned, culled, and uploaded (`R_PushDlights`) when `r_dynamic 1`, but the additive world dlight pass was skipped unless `r_dlight_style > 0`.

In this tree, `r_dlight_style` defaults to `0`, so in typical settings (`r_dynamic 1`, `r_clustered_lighting 0`) dynamic lights existed in GPU frame data but were never drawn. This made muzzle flashes/explosions appear "off" even though the pool and upload path were active.

## Evidence trail (code-level)

1. Light collection path accepts either legacy dynamic toggle or style toggle:
   - `R_PushDlights`: `(r_dynamic.value > 0.f || r_dlight_style.value > 0.f) && r_dlight_enable.value > 0.f`
2. But draw/composite path previously required `r_dlight_style > 0`:
   - `R_DrawDLightPass` early-return gate.
   - `GL_NeedsSceneEffects` gate for dlight buffered composite.
   - `r_framedata.dlight_params[0]` set only when `r_dlight_style > 0`.

This mismatch created a silent logic split: lights in buffers, no additive draw pass.

## Fix summary

Introduced a single helper (`R_DlightsAdditivePassEnabled`) that expresses intended additive-pass eligibility:

- disabled when clustered path is active.
- enabled when either `r_dlight_style > 0` OR `r_dynamic > 0`.

Then reused it in all three gate points above to keep CPU/GPU/pass scheduling aligned.

## Regression test scene/cmdline

Use an easy dark-scene repro where explosions or muzzle flashes are obvious.

Example startup:

```bash
./ironwail -window -width 1280 -height 720 +map e1m1 +r_dynamic 1 +r_dlight_enable 1 +r_clustered_lighting 0 +r_dlight_style 0 +r_dlight_mode 1 +r_dlight_buffer 1 +notarget 1
```

Then fire repeatedly in a dark interior and verify dynamic light response.

## Test matrix (manual)

- Baseline:
  - `r_dynamic 1; r_clustered_lighting 0; r_dlight_style 0`
- Additive style explicit:
  - `r_dlight_style 1`
- Clustered path:
  - `r_clustered_lighting 1` (ensure additive pass remains off)
- Post/AA interaction:
  - `r_msaa 0/4`
  - `r_tonemap 0/2`
  - `r_bloom 0/1`

Expected: dynamic lights visible in non-clustered path with default style=0, no duplication when clustered path is active.
