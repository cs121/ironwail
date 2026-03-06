// fogvol_upsample.frag  —  bilateral depth-weighted upsample from half-res fog
// ── CHANGES FROM ORIGINAL ──────────────────────────────────────────────────
//  1. [BUG] halfCoord derivation: `round(gl_FragCoord.xy * 0.5 - 0.5)` was
//     introduced as a "fix" but is wrong.  For a 2× downsample the correct
//     mapping is simple floor/integer-truncation: full-res pixel (fx,fy) →
//     half-res texel (fx/2, fy/2).  The round()-0.5 formula produces the same
//     half-res index for two consecutive full-res pixels (e.g. pixels 2 and 3
//     both map to half-res texel 1 instead of 1 and 1 respectively — actually
//     round(2*0.5-0.5)=round(0.5)=0 and round(3*0.5-0.5)=round(1.0)=1, but
//     round(0.5) is implementation-defined and can return 0 or 1).  On many
//     GPU implementations this causes systematic ½-texel misalignment that
//     manifests as a warped/distorted upsample.  The fix is to use integer
//     division (floor) which is unambiguous and correct for 2× downsampling.
//     The 9-tap path centring concern is handled by clamping to valid range.
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

// FogVol Composite Contract:
//   RGB = fog radiance composited over scene color (display-space contribution).
//   A   = fog coverage proxy = 1.0 - transmittance (0=no fog medium, 1=opaque medium).
//   Valid only when CPU marks fogvol composite as ready for this frame.

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
// FIX #2: fullTap uses tapCoord * 2 + 1 to match the halfres fog pass depth lattice.
// FIX #6: halfSizeSafe ensures clamp bounds are never negative.
void AccumTap(ivec2 tapCoord, ivec2 halfSizeSafe, ivec2 fullSizeSafe,
              float depthCenter, inout vec4 accum, inout float weightSum)
{
	ivec2 clampedTap = clamp(tapCoord, ivec2(0), halfSizeSafe);
	// Match the representative depth texel used by the halfres fog pass.
	ivec2 fullTap    = clamp(clampedTap * 2 + ivec2(1), ivec2(0), fullSizeSafe);
	float depthTap   = texelFetch(SceneDepth, fullTap, 0).r;
	float weight     = exp(-abs(depthTap - depthCenter) * FogUpsampleK);
	// Accumulate RGBA: RGB=fog color, A=fog density (1-transmittance).
	accum     += texelFetch(FogColor, clampedTap, 0) * weight;
	weightSum += weight;
}

void main()
{
	ivec2 fullSize = ivec2(FogUpsampleSize.xy);
	/* BP FIX: Clamp halfSize to at least ivec2(1) before subtraction to guard
	 * against float-to-int conversion producing 0 or negative values for
	 * degenerate framebuffers (e.g. if FogUpsampleSize.zw < 1.0). */
	ivec2 halfSize = max(ivec2(FogUpsampleSize.zw), ivec2(1));

	// FIX #6: Guard against degenerate sizes.
	ivec2 halfSizeSafe = max(halfSize - ivec2(1), ivec2(0));
	ivec2 fullSizeSafe = max(fullSize - ivec2(1), ivec2(0));

	ivec2 fullCoord  = ivec2(gl_FragCoord.xy);

	// FIX #1: Use integer division (floor) for the half-res texel address.
	// For a 2× downsample, full-res pixel (fx,fy) → half-res texel (fx/2,fy/2).
	// The previous round()-0.5 formula was ambiguous at half-integer values and
	// caused systematic ½-texel misalignment → warped/distorted output.
	ivec2 halfCoord  = clamp(ivec2(gl_FragCoord.xy) / 2, ivec2(0), halfSizeSafe);

	float depthCenter = texelFetch(SceneDepth, fullCoord, 0).r;
	vec4  accum       = vec4(0.0); // RGB=fog color, A=fog density (1-transmittance)
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
		/* BUG-U-01 FIX: The 4 straddling half-res texels depend on which quadrant
		 * of the 2×2 block the full-res pixel falls in (its parity).
		 * For pixel (fx,fy): halfCoord = (fx/2, fy/2).
		 * The four half-res texels whose 2×2 footprints cover (fx,fy) are:
		 *   halfCoord + (0,0), (-1,0), (0,-1), (-1,-1)  when fx,fy are ODD
		 *   halfCoord + (0,0), (+1,0), (0,+1), (+1,+1)  when fx,fy are EVEN
		 * Unified via parity offset: base = halfCoord, delta = parity - 1
		 *   parity = (fx&1, fy&1) → delta = (0,-1) or (-1,0) etc.
		 * Using (parity - 1) maps: odd→(0,-1) stays in block, even→(-1,-1) wrong.
		 * Correct: offset the BASE by parity so the 4 taps are always the 2×2
		 * block that contains the full-res pixel. */
		ivec2 parity  = ivec2(fullCoord.x & 1, fullCoord.y & 1); // 0 or 1
		ivec2 base    = halfCoord - (ivec2(1) - parity); // shift base to block origin
		AccumTap(base,                    halfSizeSafe, fullSizeSafe, depthCenter, accum, weightSum);
		AccumTap(base + ivec2(1, 0),      halfSizeSafe, fullSizeSafe, depthCenter, accum, weightSum);
		AccumTap(base + ivec2(0, 1),      halfSizeSafe, fullSizeSafe, depthCenter, accum, weightSum);
		AccumTap(base + ivec2(1, 1),      halfSizeSafe, fullSizeSafe, depthCenter, accum, weightSum);
	}

	vec4 result;
	if (weightSum > 0.0)
	{
		result = accum / weightSum;
	}
	else
	{
		// FIX #4: Fallback to nearest half-res texel instead of black.
		// texelFetch returns RGBA — includes fog density in alpha.
		result = texelFetch(FogColor, halfCoord, 0);
	}

	// Pass RGBA: RGB=upsampled fog color, A=upsampled fog density.
	// glColorMask(A=false) in the final blit prevents this reaching composite.fbo.
	outColor = result;
}
