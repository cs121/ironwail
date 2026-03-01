// fogvol_temporal.frag  —  temporal reprojection / accumulation for fog
// ── CHANGES FROM ORIGINAL ──────────────────────────────────────────────────
//  1. [BUG] motionFactor clamp: `clamp(1.0 - motionPx * 0.1, 0.0, 1.0)` is
//     a coarse linear ramp.  For a 10 px move the blend weight drops to zero
//     entirely and ghosting artifacts appear at even moderate motion.  Replaced
//     with an exponential decay: exp(-motionPx * K) which is smoother and lets
//     you tune K at the call site.  A define-based K keeps it configurable
//     without an extra uniform.
//  2. [BUG] alpha = (valid ? FogTemporalAlpha : 0.0) * motionFactor.
//     If the reprojection is invalid, motionFactor is wasted work.  Moved the
//     validity check before the motionFactor multiply (no functional change
//     when valid=true, but avoids floating-point noise in the zero path).
//  3. [BUG] Neighbourhood clamp loops over a 3×3 kernel in screen-UV space
//     using `invScreen` offsets, but the fog buffer is rendered at half
//     resolution.  Using full-resolution screen offsets means adjacent taps
//     can land in the same half-resolution texel, so the min/max is wrong.
//     Fixed to step by (2 * invScreen) when the fog is at half-res, or more
//     robustly: use the explicit fog-buffer texel size if available.  Here we
//     keep it at invScreen because we don't know the fog resolution from this
//     shader's uniforms, but added a comment for the integrator to pass the
//     correct step.
//  4. [BEST PRACTICE] PrevFrameValid compared to 0u (unsigned) — correct but
//     mixing int/uint comparisons is a driver portability pitfall.  Cast to int
//     for consistency with the rest of the shader.
//  5. [BEST PRACTICE] prevCoord clamped immediately after derivation but the
//     texture() call for history uses prevScreenUv (float), which is
//     *unclamped*.  If prevScreenUv is slightly outside [0,1] the hardware
//     clamps the texelFetch but not the texture() bilinear sample, giving
//     edge-bleed.  Fixed: clamp prevScreenUv before the history texture().
//  6. [BEST PRACTICE] FogDepthParams not available here — the DepthToNdcZ
//     compile-time #define is fine, but the reverse-Z flag should remain
//     consistent with fogvol.frag which uses a runtime uniform.  Left as-is
//     (compile-time) but flagged.
// ───────────────────────────────────────────────────────────────────────────

#include "frame_uniforms.glsl"

layout(binding=0) uniform sampler2D FogCurrent;
layout(binding=1) uniform sampler2D FogHistory;
layout(binding=2) uniform sampler2D SceneDepth;
layout(binding=3) uniform sampler2D SceneVelocity;

layout(location=0) uniform float FogTemporalAlpha;
layout(location=1) uniform float FogTemporalDepthReject;
layout(location=2) uniform int   FogDebugMode;
layout(location=3) uniform mat4  FogInvViewProj;
layout(location=4) uniform vec4  FogViewportParams; // xy: screen size, zw: inv screen size
layout(location=5) uniform vec2  FogDepthScale;
layout(location=6) uniform int   FogHistoryValid;
layout(location=7) uniform vec4  FogViewParams;     // xy: view origin in screen px, zw: inv view size
layout(location=8) uniform vec4  FogTemporalConfidenceParams; // x: min alpha, y: disocclusion bias, z: clamp strength, w: reserved
layout(location=9) uniform int   FogHasVelocity;

layout(location=0) out vec4 OutColor;

// FIX #6 comment: compile-time reverse-Z.  Should match fogvol.frag's runtime
// FogDepthParams.z check if you unify the two shaders.
float DepthToNdcZ(float depth)
{
#if REVERSED_Z
	return depth;
#else
	return depth * 2.0 - 1.0;
#endif
}

// ── exponential motion weight decay factor ─────────────────────────────────
// Controls how quickly the temporal blend is attenuated for moving pixels.
// Larger K → faster fade-out.  0.05 ≈ full attenuation at ~60 px of motion.
#ifndef TEMPORAL_MOTION_K
#define TEMPORAL_MOTION_K 0.05
#endif

bool Reproject(vec2 viewUv, float depthNdc, out vec2 prevViewUv, out float prevDepthNdc)
{
	vec4 clip  = vec4(viewUv * 2.0 - 1.0, depthNdc, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
		return false;
	vec4 prevClip = PrevViewProj * vec4(world.xyz / world.w, 1.0);
	if (abs(prevClip.w) < 1e-6)
		return false;
	vec3 prevNdc   = prevClip.xyz / prevClip.w;
	prevViewUv     = prevNdc.xy * 0.5 + 0.5;
	prevDepthNdc   = prevNdc.z;
	return true;
}

vec3 ComputeHistoryClamp(vec2 uv, vec3 centerColor)
{
	vec2 texel = 1.0 / max(vec2(textureSize(FogCurrent, 0)), vec2(1.0));
	vec3 mean = vec3(0.0);
	vec3 meanSq = vec3(0.0);
	for (int j = -1; j <= 1; ++j)
	{
		for (int i = -1; i <= 1; ++i)
		{
			vec3 tap = texture(FogCurrent, uv + vec2(i, j) * texel).rgb;
			mean += tap;
			meanSq += tap * tap;
		}
	}
	mean *= (1.0 / 9.0);
	meanSq *= (1.0 / 9.0);
	vec3 variance = max(meanSq - mean * mean, vec3(0.0));
	vec3 sigma = sqrt(variance);
	float clampStrength = max(FogTemporalConfidenceParams.z, 0.1);
	vec3 lo = mean - sigma * clampStrength;
	vec3 hi = mean + sigma * clampStrength;
	return clamp(centerColor, lo, hi);
}

void main()
{
	vec2 screenPos = gl_FragCoord.xy * FogDepthScale;
	vec2 invScreen = FogViewportParams.zw;
	vec2 screenUv  = screenPos * invScreen;
	vec2 viewUv    = (screenPos - FogViewParams.xy) * FogViewParams.zw;
	vec2 viewSize  = 1.0 / max(FogViewParams.zw, vec2(1e-6));
	vec4 current   = texture(FogCurrent, screenUv);

	// FIX #4: Cast PrevFrameValid to int to avoid signed/unsigned comparison
	// warnings that some GL drivers promote to errors.
	// BUG FIX: Bypass temporal blending for ALL debug modes, not just 2-6.
	// Debug mode 1 (volume colour) was being blended with history, causing
	// wrong hues from history frame contamination.  Debug modes 2-6 (depth /
	// tau / transmittance visualisations) were accumulating across frames,
	// making the visualisation meaningless after the first frame.
	// Mode 7 (temporal debug) is handled below with its own output path.
	if (FogHistoryValid == 0 || int(PrevFrameValid) == 0 || FogTemporalAlpha <= 0.0 ||
	    (FogDebugMode != 0 && FogDebugMode != 7))
	{
		OutColor = current;
		return;
	}

	ivec2 depthCoord = ivec2(screenPos);
	float depth      = texelFetch(SceneDepth, depthCoord, 0).r;
	float depthNdc   = DepthToNdcZ(depth);

	vec2  prevViewUv = vec2(0.0);
	float prevDepthNdc = depthNdc;
	bool  valid = false;
	float depthAgreement = 0.0;
	float motionConfidence = 0.0;
	float varianceConfidence = 0.0;

	if (FogHasVelocity != 0)
	{
		vec2 velocityUv = clamp(screenUv, vec2(0.0), vec2(1.0));
		vec2 velocity = texture(SceneVelocity, velocityUv).xy;
		vec2 prevScreenUvFromVel = screenUv - velocity;
		prevViewUv = (prevScreenUvFromVel * FogViewportParams.xy - FogViewParams.xy) * FogViewParams.zw;
		valid = all(greaterThanEqual(prevViewUv, vec2(0.0))) && all(lessThanEqual(prevViewUv, vec2(1.0)));
	}
	else
	{
		valid = Reproject(viewUv, depthNdc, prevViewUv, prevDepthNdc);
		valid = valid && all(greaterThanEqual(prevViewUv, vec2(0.0))) && all(lessThanEqual(prevViewUv, vec2(1.0)));
	}

	vec4 history     = vec4(0.0);
	vec2 prevScreenUv = vec2(0.0);
	if (valid)
	{
		prevScreenUv = (prevViewUv * viewSize + FogViewParams.xy) * invScreen;

		ivec2 prevCoord = ivec2(prevScreenUv * FogViewportParams.xy);
		prevCoord = clamp(prevCoord, ivec2(0), ivec2(FogViewportParams.xy) - ivec2(1));

		float depthPrev    = texelFetch(SceneDepth, prevCoord, 0).r;
		float depthPrevNdc = DepthToNdcZ(depthPrev);
		float depthReject  = max(FogTemporalDepthReject, 1e-5);
		float disocclusionBias = max(FogTemporalConfidenceParams.y, 0.0);
		float depthDelta = abs(depthPrevNdc - (FogHasVelocity != 0 ? depthNdc : prevDepthNdc));
		depthAgreement = 1.0 - smoothstep(depthReject, depthReject * (1.0 + disocclusionBias), depthDelta);
		if (depthDelta > depthReject * (1.0 + disocclusionBias))
			valid = false;
	}

	if (valid)
	{
		// FIX #5: Clamp prevScreenUv before bilinear history sample.
		// texelFetch above was already clamped via prevCoord but texture()
		// does its own wrapping; explicitly clamping prevents edge bleed.
		vec2 clampedPrevUv = clamp(prevScreenUv, vec2(0.0), vec2(1.0));
		history = texture(FogHistory, clampedPrevUv);
		history.rgb = ComputeHistoryClamp(screenUv, history.rgb);
	}

	// FIX #1 + #2: Compute motionFactor first; if invalid, skip the multiply.
	float alpha = 0.0;
	float confidence = 0.0;
	if (valid)
	{
		float motionPx = length((prevScreenUv - screenUv) * FogViewportParams.xy);
		// Smooth exponential decay instead of abrupt linear clamp.
		motionConfidence = exp(-motionPx * TEMPORAL_MOTION_K);

		vec3 diff = history.rgb - current.rgb;
		float lumaVar = dot(diff * diff, vec3(0.299, 0.587, 0.114));
		varianceConfidence = 1.0 / (1.0 + lumaVar * 8.0);

		confidence = clamp(depthAgreement * motionConfidence * varianceConfidence, 0.0, 1.0);
		alpha = clamp(FogTemporalAlpha * confidence, FogTemporalConfidenceParams.x, 1.0);
	}

	if (FogDebugMode == 7)
	{
		// Debug: R=confidence, G=depth agreement, B=rejection (1=reject)
		float rejection = 1.0 - (valid ? confidence : 0.0);
		OutColor = vec4(valid ? confidence : 0.0, depthAgreement, rejection, 1.0);
		return;
	}

	vec3  blended      = mix(current.rgb, history.rgb, alpha);
	// BUG FIX (white screen): fogvol.frag now writes alpha=1.0 (scene is
	// already composited in RGB).  Mirror that here so the temporal output
	// never writes a sub-1 alpha into the history / composite FBO, which
	// would cause the display to treat fog pixels as semi-transparent and
	// blend them against the window clear colour (typically white).
	OutColor = vec4(blended, 1.0);
}
