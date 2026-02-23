# Shadow Debug Modes (Known-Good Output)

These CVars isolate *pipeline wiring* issues from real shadow math issues.

## CVars

- `r_shadow_debug` (default `0`)
  - `0`: normal behavior
  - `1`: force shadow factor to a **screen-space checkerboard** (`gl_FragCoord` based)
  - `2`: force shadow factor to a **shadow-UV checkerboard** (atlas UV based)
  - `3`: force shadow factor to grayscale **reference depth** (`z/w`)
- `r_shadow_debug_dummytex` (default `0`)
  - `0`: bind real shadow depth atlas
  - `1`: bind a known 8x8 dummy depth texture on the **same texture unit** as the real shadow map
- `r_shadow_debug_source` (default `0`)
  - `0`: sample normal shadow depth atlas
  - `1`: use a debug **color atlas source** rendered in the shadow pass

## Quick tests

### TEST A — prove shader path
1. `r_shadow_debug 1`
2. Observe checkerboard where shadow factor modulates light.

Expected:
- No checkerboard => wrong shader/permutation active, or shadow path not used.

### TEST B — validate shadow coordinates
1. `r_shadow_debug 2`
2. Observe checker pattern in shadow UV space moving with light projection.

Expected:
- Garbled/noisy/unstable pattern => bad shadow matrix, wrong divide, or UV remap mismatch.

### TEST C — validate binding/sampler unit
1. `r_shadow_debug 0`
2. `r_shadow_debug_dummytex 1`
3. Observe shadowing influenced by the dummy pattern.

Expected:
- No visible change => wrong texture unit/sampler binding or binding overwritten later.

### TEST D — validate shadow pass FBO / atlas slot mapping
1. `r_shadow_debug_source 1`
2. `r_shadow_debug 0`
3. Observe stable checkerboard sourced from debug atlas.

Expected:
- Broken/incorrect pattern => viewport/scissor/FBO attach/atlas tile mapping issue.

## Return to normal

```
r_shadow_debug 0
r_shadow_debug_dummytex 0
r_shadow_debug_source 0
```
