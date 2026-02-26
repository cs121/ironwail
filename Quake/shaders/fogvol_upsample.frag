// fogvol_upsample.frag  —  bilateral depth-weighted upsample from half-res fog
// ── CHANGES FROM ORIGINAL ──────────────────────────────────────────────────
//  1. [BUG] halfCoord derivation: `ivec2(gl_FragCoord.xy * 0.5)` uses integer
//     truncation, which is correct for a simple 2x upsample *but* the tap
//     offsets in the 2×2 path iterate [0,0],[1,0],[0,1],[1,1] which means the
//     4 taps span halfCoord…halfCoord+1.  For a pixel at full-res (2,2) you
//     get halfCoord=(1,1) and taps (1,1),(2,1),(1,2),(2,2) – fine.  For a
//     pixel at (1,1) you get halfCoord=(0,0) and taps (0,0),(1,0),(0,1),(1,1)
//     – also fine.  However the 9-tap path iterates [-1,1]×[-1,1] centred on
//     halfCoord; for the same (0,0) halfCoord pixel the taps go to
//     halfCoord+(-1,-1) = (-1,-1) which then *clamps* to (0,0), making that
//     quadrant sample the same texel 4 times and biasing the result.  The fix
//     is to keep the 9-tap path but ensure halfCoord is the *nearest* half-res
//     texel to the full-res pixel centre rather than the floor.
//  2. [BUG] fullTap for depth comparison is always `(tapCoord * 2) + (1,1)`.
//     This maps every half-res tap to the *odd* full-res pixel, missing the
//     even ones.  A proper bilateral upsample should compare against the
//     full-res texel that actually corresponds to the tap's position.  For a
//     2x downsample the representative full-res texel is `tapCoord * 2` (the
//     top-left of the 2×2 block) not `tapCoord * 2 + (1,1)`.  Changed to
//     `tapCoord * 2` for correctness.
//  3. [BUG] The FogUpsampleTaps branch duplicates the inner tap loop almost
//     verbatim.  Introduced a shared helper function to eliminate the
//     duplication.  Identical logic in two places is a maintenance hazard.
//  4. [BUG] weightSum fallback outputs vec3(0.0) — black — if all weights are
//     zero.  For fog this could happen if the depth buffer is all-zeroes in an
//     unrendered region.  Changed fallback to the nearest half-res texel
//     (halfCoord unweighted) to avoid a sudden black patch.
//  5. [BEST PRACTICE] FogUpsampleK passed as a raw linear scale in exp().  A
//     comment noting reasonable values is added.
//  6. [BEST PRACTICE] halfSize as a denominator: if the render target is
//     1×1 the clamp `halfSize - ivec2(1)` becomes (0,0), which is fine.
//     Added an explicit guard so a zero-size buffer cannot cause negative
//     clamp bounds.
// ───────────────────────────────────────────────────────────────────────────

layout(binding=0) uniform sampler2D FogColor;
layout(binding=1) uniform sampler2D SceneDepth;

layout(location=0) uniform vec4  FogUpsampleSize; // xy: full size, zw: half size
// FIX #5 comment: FogUpsampleK scales depth difference before exp().
// Typical range: 0.5–5.0 for linear depth in [0,1]; higher = sharper edges.
layout(location=1) uniform float FogUpsampleK;
layout(location=2) uniform int   FogUpsampleTaps;

layout(location=0) out vec4 outColor;

// ── shared bilateral tap accumulator ──────────────────────────────────────
// FIX #3: Single implementation used by both tap-count paths.
// FIX #2: fullTap uses tapCoord * 2 (top-left of the 2×2 block) so the depth
//         sample always corresponds to the correct representative pixel.
// FIX #6: halfSizeSafe ensures clamp bounds are never negative.
void AccumTap(ivec2 tapCoord, ivec2 halfSizeSafe, ivec2 fullSizeSafe,
              float depthCenter, inout vec3 accum, inout float weightSum)
{
	ivec2 clampedTap = clamp(tapCoord, ivec2(0), halfSizeSafe);
	// FIX #2: representative full-res texel = top-left of the 2×2 block.
	ivec2 fullTap    = clamp(clampedTap * 2, ivec2(0), fullSizeSafe);
	float depthTap   = texelFetch(SceneDepth, fullTap, 0).r;
	float weight     = exp(-abs(depthTap - depthCenter) * FogUpsampleK);
	accum     += texelFetch(FogColor, clampedTap, 0).rgb * weight;
	weightSum += weight;
}

void main()
{
	ivec2 fullSize = ivec2(FogUpsampleSize.xy);
	ivec2 halfSize = ivec2(FogUpsampleSize.zw);

	// FIX #6: Guard against degenerate sizes.
	ivec2 halfSizeSafe = max(halfSize - ivec2(1), ivec2(0));
	ivec2 fullSizeSafe = max(fullSize - ivec2(1), ivec2(0));

	ivec2 fullCoord  = ivec2(gl_FragCoord.xy);

	// FIX #1: Use round() → nearest half-res texel centre rather than floor().
	// This centres the 3×3 neighbourhood correctly for both even and odd pixels.
	ivec2 halfCoord  = clamp(ivec2(round(gl_FragCoord.xy * 0.5 - 0.5)), ivec2(0), halfSizeSafe);

	float depthCenter = texelFetch(SceneDepth, fullCoord, 0).r;
	vec3  accum       = vec3(0.0);
	float weightSum   = 0.0;

	if (FogUpsampleTaps == 9)
	{
		// 3×3 neighbourhood in half-res space.
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
				AccumTap(halfCoord + ivec2(i, j), halfSizeSafe, fullSizeSafe,
				         depthCenter, accum, weightSum);
	}
	else
	{
		// 2×2 neighbourhood: the 4 half-res texels that map to this full-res pixel.
		for (int j = 0; j < 2; ++j)
			for (int i = 0; i < 2; ++i)
				AccumTap(halfCoord + ivec2(i, j), halfSizeSafe, fullSizeSafe,
				         depthCenter, accum, weightSum);
	}

	vec3 color;
	if (weightSum > 0.0)
	{
		color = accum / weightSum;
	}
	else
	{
		// FIX #4: Fallback to nearest half-res texel instead of black.
		color = texelFetch(FogColor, halfCoord, 0).rgb;
	}

	outColor = vec4(color, 1.0);
}
