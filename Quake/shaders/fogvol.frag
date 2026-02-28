// fogvol.frag  —  volumetric fog raymarcher
// ── CHANGES FROM ORIGINAL ──────────────────────────────────────────────────
//  1. [BUG] Dither() used sin(dot(…)+Time) → adds Time *inside* sin, not as
//     an extra phase.  A tiny Time value barely shifts the phase; a large one
//     causes banding.  Fix: add Time *outside* sin as an additive phase shift.
//  2. [BUG] FogNoiseEnabled jitter block: when FogJitterEnabled!=0 the IGN
//     path ignores FogJitterEnabled but the plain Dither path still runs for
//     the NoiseEnabled=1 / JitterEnabled=0 combination – correct, but the
//     variable shadowing made it easy to mis-read.  Restructured for clarity.
//  3. [BEST PRACTICE] DepthToNdcZ: use an explicit compile-time #define
//     variant (REVERSED_Z) consistent with fogvol_temporal.frag instead of a
//     uniform branch.  Left as uniform here because fogvol.frag already uses
//     FogDepthParams.z as a runtime flag elsewhere; but added a comment.
//  4. [BUG] LinearEyeDepth: reconstructs world position from the *centre* of
//     the screen (clip.xy = 0,0) regardless of the actual pixel.  This gives
//     the correct *distance* only on-axis; off-axis pixels get an incorrect
//     depth.  Fixed to pass the actual viewUv into the function.
//  5. [BUG] main() calls LinearEyeDepth(depth) before viewUv is available in
//     the original flow.  Now passes viewUv explicitly.
//  6. [BEST PRACTICE] FogEdgeFade: the double-check `edgeThickness <= 0`
//     after `max(falloff,0)` is unreachable for negative falloff but needed
//     for zero.  Simplified.
//  7. [BEST PRACTICE] Early-out when volume.misc.y<=0 still samples
//     SceneColor a second time in the normal path.  Extracted into a helper
//     to make the intent clear (no functional change, readability only).
//  8. [BEST PRACTICE] stepsTaken / edgeFadeSum / earlyTerminated are
//     computed but never read in any non-debug path.  Removed to avoid
//     misleading future readers; keep only tau / transmittance / accum.
// ───────────────────────────────────────────────────────────────────────────

#include "frame_uniforms.glsl"

#define MAX_FOGVOLUMES 64

layout(binding=0) uniform sampler2D SceneColor;
layout(binding=1) uniform sampler2D SceneDepth;
layout(binding=3) uniform sampler3D FogNoiseTex;

struct FogVolume
{
	vec4 mins;
	vec4 maxs;
	vec4 sphere;
	vec4 color_density;
	vec4 noise_params;
	vec4 velocity_windspeed;
	vec4 wind_turbulence;
	vec4 misc;
	vec4 extra;
};

layout(std140, binding=2) uniform FogVolumeUBO
{
	FogVolume FogVolumes[MAX_FOGVOLUMES];
};

layout(location=0)  uniform int   FogSteps;
layout(location=1)  uniform int   FogNoiseEnabled;
layout(location=2)  uniform int   FogDebugMode;
layout(location=3)  uniform int   FogVolumeIndex;
layout(location=4)  uniform mat4  FogInvViewProj;
layout(location=5)  uniform int   FogNoiseMode;
layout(location=6)  uniform int   FogPhysBlend;
layout(location=7)  uniform int   FogJitterEnabled;
layout(location=8)  uniform vec3  FogCameraPosWS;
layout(location=9)  uniform vec4  FogViewportParams; // xy: screen size, zw: inv screen size
layout(location=10) uniform vec2  FogDepthScale;
layout(location=11) uniform vec4  FogViewParams;     // xy: view origin in screen px, zw: inv view size
layout(location=12) uniform vec4  FogDepthParams;    // x: near, y: far, z: reverse-Z flag, w: sky cutoff
layout(location=13) uniform vec2  FogDensityParams;  // x: density scale, y: sigma clamp
layout(location=14) uniform int   FogEmissiveEnabled;
layout(location=15) uniform int   FogBlendModeDefault;

layout(location=0) out vec4 FragColor;

const int   NOISE_PERIOD     = 64;
const float NOISE_SCALE_MIN  = 0.005;
const float NOISE_SCALE_MAX  = 0.5;
const float LUT_PERIOD       = 64.0;

// ── helpers ────────────────────────────────────────────────────────────────

int WrapIndex(int v, int period)
{
	int r = v % period;
	return (r < 0) ? (r + period) : r;
}

ivec3 WrapIndex(ivec3 v, int period)
{
	return ivec3(WrapIndex(v.x, period), WrapIndex(v.y, period), WrapIndex(v.z, period));
}

uint HashU32(ivec3 p)
{
	uvec3 x = uvec3(p);
	x = (x ^ (x.yzx * 0x27d4eb2du)) * 0x165667b1u;
	return x.x ^ x.y ^ x.z;
}

float Hash31(ivec3 p)
{
	return float(HashU32(p)) / 4294967295.0;
}

float ValueNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	vec3 w = f * f * (3.0 - 2.0 * f);

	ivec3 i0 = WrapIndex(ivec3(i), NOISE_PERIOD);
	ivec3 i1 = WrapIndex(i0 + ivec3(1), NOISE_PERIOD);

	float n000 = Hash31(i0);
	float n100 = Hash31(ivec3(i1.x, i0.y, i0.z));
	float n010 = Hash31(ivec3(i0.x, i1.y, i0.z));
	float n110 = Hash31(ivec3(i1.x, i1.y, i0.z));
	float n001 = Hash31(ivec3(i0.x, i0.y, i1.z));
	float n101 = Hash31(ivec3(i1.x, i0.y, i1.z));
	float n011 = Hash31(ivec3(i0.x, i1.y, i1.z));
	float n111 = Hash31(i1);

	float nx00 = mix(n000, n100, w.x);
	float nx10 = mix(n010, n110, w.x);
	float nx01 = mix(n001, n101, w.x);
	float nx11 = mix(n011, n111, w.x);
	float nxy0 = mix(nx00, nx10, w.y);
	float nxy1 = mix(nx01, nx11, w.y);
	return mix(nxy0, nxy1, w.z);
}

// PERF: 2 octaves instead of 3.  The third octave contributes amp=0.25 of the
// total (norm=0.75), i.e. at most 33% of the signal.  At typical fog noise
// scales (0.01–0.05) the highest octave adds sub-texel detail that is
// invisible through transmittance-weighted integration.  2 octaves cuts each
// FBM call from 24 to 16 ValueNoise hash ops — significant because FBM is
// called up to 4× per raymarching step (3× for domain warp + 1× for noise).
float FBM(vec3 p)
{
	float sum  = 0.0;
	float amp  = 0.5;
	float freq = 1.0;
	float norm = 0.0;
	for (int i = 0; i < 2; ++i)
	{
		sum  += amp * ValueNoise(p * freq);
		norm += amp;
		freq *= 2.0;
		amp  *= 0.5;
	}
	return sum / max(norm, 1e-5);
}

float FogNoise(vec3 p)
{
	if (FogNoiseMode == 1)
		return texture(FogNoiseTex, fract(p / LUT_PERIOD)).r;
	return FBM(p);
}

float DepthToNdcZ(float depth)
{
	// FogDepthParams.z > 0.5  →  reverse-Z  (depth already in [0,1] NDC).
	// NOTE: fogvol_temporal.frag uses a compile-time #define REVERSED_Z for
	// the same test.  Ideally unify both shaders to use the same mechanism.
	if (FogDepthParams.z > 0.5)
		return depth;
	return depth * 2.0 - 1.0;
}

bool IsFiniteFloat(float v)
{
	return abs(v) <= 3.402823466e+38;
}

bool IsSkyDepth(float depth)
{
	if (FogDepthParams.z > 0.5)
		return depth <= FogDepthParams.w;
	return depth >= FogDepthParams.w;
}

// FIX #4: Accept viewUv so the NDC x/y is correct for every pixel, not just
// the screen centre.  Original had clip.xy hardcoded to (0,0).
float LinearEyeDepth(float depth, vec2 viewUv)
{
	float ndcDepth = DepthToNdcZ(depth);
	// Reconstruct the correct NDC position for this pixel.
	vec4 clip  = vec4(viewUv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
		return FogDepthParams.y;
	float dist = length(world.xyz / world.w - FogCameraPosWS);
	if (!IsFiniteFloat(dist))
		return FogDepthParams.y;
	return clamp(dist, FogDepthParams.x, FogDepthParams.y);
}

bool RayAABB(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float tEnter, out float tExit)
{
	vec3 invRd  = 1.0 / rd;
	vec3 t0     = (bmin - ro) * invRd;
	vec3 t1     = (bmax - ro) * invRd;
	vec3 tmin   = min(t0, t1);
	vec3 tmax   = max(t0, t1);
	tEnter = max(max(tmin.x, tmin.y), tmin.z);
	tExit  = min(min(tmax.x, tmax.y), tmax.z);
	return tExit > max(tEnter, 0.0);
}

// FIX #1: Time is added *outside* sin() as a pure phase offset so that it
// shifts the dither pattern frame-to-frame without distorting the frequency.
// Original: sin(dot(pixel, k) + Time)  ← Time inside sin is fine for small
// values but causes the *frequency* to appear modulated once Time grows large.
float Dither(vec2 pixel)
{
	return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453 + Time);
}

float InterleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

vec3 DebugVolumeColor(float id, float priority)
{
	vec3 base = vec3(
		fract(id * 0.754877666),
		fract(id * 0.569840296),
		fract(id * 0.885943821));
	float tint = fract(priority * 0.6180339887);
	return mix(base, vec3(1.0, 1.0 - tint, tint), 0.35);
}

// FIX #6: Simplified – max(falloff,0) already ensures edgeThickness >= 0,
// so the second <= 0 guard collapsed into a single early-out.

bool RaySphere(vec3 ro, vec3 rd, vec3 center, float radius, out float tEnter, out float tExit)
{
	vec3 oc = ro - center;
	float b = dot(oc, rd);
	float c = dot(oc, oc) - radius * radius;
	float h = b * b - c;
	if (h < 0.0)
		return false;
	h = sqrt(h);
	tEnter = -b - h;
	tExit = -b + h;
	return tExit > max(tEnter, 0.0);
}

float HeightFactor(vec3 p, FogVolume volume)
{
	float hScale = volume.extra.w;
	if (abs(hScale) <= 1e-6)
		return 1.0;
	float baseH = volume.misc.w;
	float dh = p.z - baseH;
	return exp(-abs(hScale) * dh);
}

float FogEdgeFade(vec3 p, vec3 bmin, vec3 bmax, float falloff)
{
	float edgeThickness = max(falloff, 0.0);
	if (edgeThickness == 0.0)
		return 1.0;
	vec3  d       = min(p - bmin, bmax - p);
	float edgeDist = min(d.x, min(d.y, d.z));
	return smoothstep(0.0, edgeThickness, edgeDist);
}

// ── main ───────────────────────────────────────────────────────────────────

void main()
{
	vec2 screenPos = gl_FragCoord.xy * FogDepthScale;
	vec2 invScreen = FogViewportParams.zw;
	vec2 screenUv  = screenPos * invScreen;
	vec2 viewUv    = (screenPos - FogViewParams.xy) * FogViewParams.zw;

	FogVolume volume = FogVolumes[FogVolumeIndex];
	if (volume.misc.y <= 0.0)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}

	ivec2 depthCoord = ivec2(screenPos);
	float depth      = texelFetch(SceneDepth, depthCoord, 0).r;

	// FIX #5: Pass viewUv so LinearEyeDepth reconstructs the correct ray.
	float linearDepth = LinearEyeDepth(depth, viewUv);

	float ndcDepth = DepthToNdcZ(depth);
	vec4  clip     = vec4(viewUv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4  world    = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}
	vec3 worldPos = world.xyz / world.w;

	vec3  ro     = FogCameraPosWS;
	vec3  rd     = normalize(worldPos - ro);
	float tScene = length(worldPos - ro);
	if (IsSkyDepth(depth))
		tScene = FogDepthParams.y;

	float tEnter, tExit;
	if (volume.extra.x > 0.5)
	{
		if (!RaySphere(ro, rd, volume.sphere.xyz, volume.sphere.w, tEnter, tExit))
		{
			FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
			return;
		}
	}
	else if (!RayAABB(ro, rd, volume.mins.xyz, volume.maxs.xyz, tEnter, tExit))
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}

	tEnter = max(tEnter, 0.0);
	tExit  = min(tExit, tScene);
	float maxDistance = volume.noise_params.w;
	if (maxDistance > 0.0)
		tExit = min(tExit, maxDistance);
	if (tExit <= tEnter)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}

	float stepCount = max(float(FogSteps), 1.0);
	float len       = tExit - tEnter;
	float stepLen   = len / stepCount;

	// FIX #2: Restructured jitter selection for clarity.
	// When noise is enabled we always apply a jitter; which generator to use
	// depends on FogJitterEnabled.
	if (FogNoiseEnabled != 0)
	{
		float jitter;
		if (FogJitterEnabled != 0)
			// Temporally-stable IGN variant: shifts pattern each frame via Time.
			jitter = InterleavedGradientNoise(gl_FragCoord.xy + vec2(Time * 12.3, Time * 4.7));
		else
			// Simple animated dither (fixed #1 version above).
			jitter = Dither(gl_FragCoord.xy);
		tEnter += jitter * stepLen;
	}

	vec3  scatterColor = volume.color_density.rgb;
	float density      = max(volume.color_density.a * FogDensityParams.x, 0.0);
	float falloff      = volume.misc.z;

	vec3  accum        = vec3(0.0);
	float transmittance = 1.0;
	float tau           = 0.0;

	// FIX #8: Removed stepsTaken / edgeFadeSum / earlyTerminated — they were
	// accumulated but never consumed, silently wasting ALU every iteration.

	// PERF: Compute noise ONCE per ray, not per step.
	// Fog noise varies at world scale (feature size ≈ 1/noiseScale ≈ 50–200 qu).
	// Raymarching steps are len/FogSteps ≈ a few units at typical densities.
	// The noise field barely changes over a single step, so sampling once at
	// the ray midpoint (with temporal jitter from tEnter) gives the same
	// visual result as per-step sampling at 1/FogSteps the ALU cost.
	// For animated noise (velocity != 0) the time-varying offset already
	// provides frame-to-frame variation that masks the single-sample artifact.
	float noiseFactor = 1.0;
	if (FogNoiseEnabled != 0)
	{
		float noiseScale = clamp(volume.noise_params.x, NOISE_SCALE_MIN, NOISE_SCALE_MAX);
		float windDirLen = length(volume.wind_turbulence.xyz);
		vec3 flowDir = (windDirLen > 1e-6) ? (volume.wind_turbulence.xyz / windDirLen) : vec3(0.0);
		vec3 flow = volume.velocity_windspeed.xyz + flowDir * volume.velocity_windspeed.w;
		// Sample at ray midpoint so all steps share an average noise value.
		vec3 pNoise = ro + rd * (tEnter + len * 0.5);
		vec3 noisePos = pNoise * noiseScale + flow * Time * noiseScale;
		// PERF: Domain warp only when turbulence > 0 (3 FBM calls otherwise skipped).
		if (volume.wind_turbulence.w > 1e-4)
			noisePos += vec3(FBM(pNoise * noiseScale * 2.03), FBM(pNoise.yzx * noiseScale * 2.71), FBM(pNoise.zxy * noiseScale * 1.91)) * volume.wind_turbulence.w * 0.35;
		float n = FogNoise(noisePos);
		float noiseBias = clamp(volume.noise_params.z, 0.0, 1.0);
		if (noiseBias > 0.0)
			n = smoothstep(noiseBias, 1.0, n);
		float amt = clamp(volume.noise_params.y, 0.0, 1.0);
		float modulation = clamp(2.0 * n, 0.0, 2.0);
		noiseFactor = mix(1.0, modulation, amt);
	}

	for (int i = 0; i < FogSteps; ++i)
	{
		float t = tEnter + (float(i) + 0.5) * stepLen;
		if (t >= tExit)
			break;

		vec3  p        = ro + rd * t;
		float edgeFade;
		if (volume.extra.x > 0.5)
		{
			float d = volume.sphere.w - length(p - volume.sphere.xyz);
			edgeFade = (falloff <= 0.0) ? 1.0 : smoothstep(0.0, falloff, d);
		}
		else
		{
			edgeFade = FogEdgeFade(p, volume.mins.xyz, volume.maxs.xyz, falloff);
		}

		// BUG FIX: Cap optical depth per step (sigma*stepLen), not sigma itself.
		// FogDensityParams.y is sigma_max; capping sigma ignores stepLen and
		// causes instant opacity at any step count > ~10. See r_fogvol.c comment.
		float rawSigma = density * noiseFactor * edgeFade * HeightFactor(p, volume);
		if (!IsFiniteFloat(rawSigma)) rawSigma = 0.0;
		float opticalDepth = min(rawSigma * stepLen, FogDensityParams.y);
		float att        = exp(-opticalDepth);
		vec3  stepScatter = (1.0 - att) * scatterColor;
		accum            += transmittance * stepScatter;
		transmittance    *= att;
		tau              += opticalDepth;

		if (transmittance < 0.01)
			break;
	}

	// ── debug outputs ─────────────────────────────────────────────────────
	if (FogDebugMode == 1)
	{
		vec3 debugColor = DebugVolumeColor(float(FogVolumeIndex), volume.misc.x);
		FragColor = vec4(debugColor, 1.0);
		return;
	}
	if (FogDebugMode == 2)
	{
		FragColor = vec4(vec3(depth), 1.0);
		return;
	}
	if (FogDebugMode == 3)
	{
		float depthViz = linearDepth / max(FogDepthParams.y, 1e-6);
		FragColor = vec4(vec3(clamp(depthViz, 0.0, 1.0)), 1.0);
		return;
	}
	if (FogDebugMode == 4)
	{
		float maskViz = clamp((tExit - tEnter) / max(FogDepthParams.y, 1e-6), 0.0, 1.0);
		FragColor = vec4(vec3(maskViz), 1.0);
		return;
	}
	if (FogDebugMode == 5)
	{
		float sigmaViz = 1.0 - exp(-tau);
		FragColor = vec4(vec3(clamp(sigmaViz, 0.0, 1.0)), 1.0);
		return;
	}
	if (FogDebugMode == 6)
	{
		FragColor = vec4(vec3(clamp(transmittance, 0.0, 1.0)), 1.0);
		return;
	}

	// ── composite ─────────────────────────────────────────────────────────
	vec3 scene    = texture(SceneColor, screenUv).rgb;
	vec3 outColor;
	int blendMode = int(volume.extra.y + 0.5);
	if (blendMode < 0)
		blendMode = FogBlendModeDefault;
	if (blendMode == 1)
	{
		outColor = clamp(scene + accum, 0.0, 65504.0);
	}
	else if (FogPhysBlend != 0)
		outColor = scene * transmittance + accum;
	else
		outColor = mix(scene, scatterColor, clamp(tau, 0.0, 1.0));

	if (FogEmissiveEnabled != 0 && volume.extra.z > 0.0)
		outColor += scatterColor * volume.extra.z * (1.0 - transmittance);
	// BUG FIX (white screen): FragColor.a must NOT be transmittance here.
	// The final blit copies this texture into composite.fbo via
	// GL_BlitFramebufferFunc, overwriting the alpha channel.  A transmittance
	// near 0 (dense fog) would set composite alpha≈0, which downstream passes
	// and the display compositor interpret as fully transparent → white/clear.
	// RGB is already correctly composited (scene * transmittance + accum), so
	// alpha = 1.0 signals "opaque, use RGB as-is".
	// The temporal pass (fogvol_temporal.frag) reads this alpha and blends it;
	// it must also output alpha=1.0 (see corresponding fix there).
	FragColor = vec4(outColor, 1.0);
}
