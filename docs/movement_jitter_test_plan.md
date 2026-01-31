# Movement/Jitter Fix Test Plan

1. **Console input freeze**
   - Hold `W` to walk forward.
   - Open the console.
   - Expected: player stops immediately (no continued forward drift).

2. **Micro-movement smoothing**
   - Tap movement keys rapidly (short presses).
   - Expected: movement looks smooth without 0/1-frame holds or jumps.

3. **Angle wrap/interp**
   - Rotate across a wrap boundary (e.g., 179 → -179 or 359 → 0) while moving.
   - Expected: no snapping; interpolation stays on the shortest arc.
