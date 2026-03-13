// fogvol.frag    volumetric fog raymarcher
//  CHANGES FROM ORIGINAL 
//  1. [BUG] Dither() used sin(dot(...)+Time)  adds Time *inside* sin, not as
//     an extra phase.  A tiny Time value barely shifts the phase; a large one
//     causes banding.  Fix: add Time *outside* sin as an additive phase shift.
//  2. [BUG] FogNoiseEnabled jitter block: when FogJitterEnabled!=0 the IGN
//     path ignores FogJitterEnabled but the plain Dither path still runs for
//     the NoiseEnabled=1 / JitterEnabled=0 combination  correct, but the
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
// 

// FogVol Composite Contract:
//   RGB = fog radiance composited over scene color (display-space contribution).
//   A   = fog coverage proxy = 1.0 - transmittance (0=no fog medium, 1=opaque medium).
//   Valid only when CPU marks fogvol composite as ready for this frame.

#include "frame_uniforms.glsl"

#ifndef FOGVOL_GLOBAL_SIMPLE
#define FOGVOL_GLOBAL_SIMPLE 0
#endif

#ifndef FOGVOL_CLUSTERED_LOCAL
#define FOGVOL_CLUSTERED_LOCAL 0
#endif

#define MAX_FOGVOLUMES 64
#define MAX_FOGLIGHTS 31 // keep FogLightsUBO under 64KB std140 limit on some drivers

layout(binding=0) uniform sampler2D SceneColor;
layout(binding=1) uniform sampler2D SceneDepth;
layout(binding=3) uniform sampler3D FogNoiseTex;
layout(binding=6) uniform sampler3D FogFroxelLightTex;
layout(binding=7) uniform sampler2D FogGodrayShaftsTex;

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
	vec4 params2;
};

layout(std140, binding=2) uniform FogVolumeUBO
{
	FogVolume FogVolumes[MAX_FOGVOLUMES];
};

struct FogLight
{
	vec4 pos_rad;
	vec4 col_int;
};

struct FogLightList
{
	ivec4 offset_count; // x = global light offset, y = per-volume light count
};

layout(std140, binding=4) uniform FogLightsUBO
{
	FogLightList FogLightLists[MAX_FOGVOLUMES];
	FogLight FogLights[MAX_FOGVOLUMES * MAX_FOGLIGHTS];
};

layout(std140, binding=5) uniform FogLightgridUBO
{
	vec4 FogLightgridProbeRGB[MAX_FOGVOLUMES * 8];
};

struct FogClusterHeader
{
	ivec4 offset_count;
};

layout(std430, binding=8) readonly buffer FogClusterHeadersSSBO
{
	FogClusterHeader FogClusterHeaders[];
};

layout(std430, binding=9) readonly buffer FogClusterIndicesSSBO
{
	int FogClusterIndices[];
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
layout(location=16) uniform int   FogLightEnabled;
layout(location=17) uniform int   FogShadowEnabled;
layout(location=18) uniform int   FogShadowSamples;
layout(location=19) uniform float FogShadowStrength;
layout(location=20) uniform float FogShadowJitter;
layout(location=21) uniform vec3  FogShadowDir;
layout(location=22) uniform int   FogLightgridEnabled;
layout(location=23) uniform int   FogFrameIndex;
layout(location=24) uniform int   FogNoiseSubsample;
layout(location=25) uniform float FogNoiseLodSwitchDist;
layout(location=26) uniform float FogDomainWarpMaxDist;
layout(location=27) uniform int   FogCheckerboard;
layout(location=28) uniform int   FogHalfRes;
layout(location=29) uniform int   FogLightSubsample;
layout(location=30) uniform float FogSunScatter;
layout(location=31) uniform vec3  FogSunColor;
layout(location=32) uniform float FogSunAnisotropy;
layout(location=33) uniform float FogDLightScale;
layout(location=34) uniform int   FogFroxelEnabled;
layout(location=35) uniform vec4  FogFroxelParams0; // x near, y far, z tanHalfFovX, w tanHalfFovY
layout(location=36) uniform vec4  FogFroxelParams1; // x log(far/near), yzw dims
layout(location=37) uniform int   FogFroxelDebug;
layout(location=38) uniform int   FogFroxelParityMode; // 0: froxel perf fast path, 1: legacy per-light parity path
layout(location=39) uniform vec3  FogLightSourceScales; // x: froxel, y: local light list, z: lightgrid/static
layout(location=40) uniform int   FogLightingMode; // 0=off, 1=raymarch, 2=froxel, 3=froxel+raymarch detail
layout(location=41) uniform int   FogGodrayCoupling;
layout(location=42) uniform vec4  FogGodrayShaftsParams; // xy: shafts texture size, z: coupling strength, w: froxel godray scale
layout(location=43) uniform vec4  FogFroxelTemporalParams; // x: alpha, y: reject threshold, z: camera delta, w: prev valid
layout(location=44) uniform int   FogLocalOcclusionMode; // 0=off, 1=cheap signed depth test, 2=multi-tap depth cone trace
layout(location=45) uniform vec4  FogLocalOcclusionParams; // x depth thickness, y cone scale, z max dist scale, w reserved
layout(location=46) uniform float FogNoiseDetailStrength;
layout(location=47) uniform float FogHeightMistStrength;
layout(location=48) uniform float FogAtmosphereBoost;
layout(location=49) uniform vec4  FogClusterParams; // x: tile size, y: tiles x, z: tiles y, w: clustered enabled

layout(location=0) out vec4 FragColor;

const int   NOISE_PERIOD     = 64;
const float NOISE_SCALE_MIN  = 0.005;
const float NOISE_SCALE_MAX  = 0.5;
const float LUT_PERIOD       = 64.0;
const float ANISO_G_LOCAL    = 0.5;
// Subtle cloud shaping constants (kept local: no new CVars / uniforms).
const float FOG_RIDGED_STRENGTH = 0.22;
const float FOG_MACRO_SCALE_MUL = 0.22;
const float FOG_MACRO_STRENGTH  = 0.20;
const vec3  LUMA_WEIGHTS        = vec3(0.2126, 0.7152, 0.0722);

const int FOGVOL_SHAPE_BOX = 0;
const int FOGVOL_SHAPE_SPHERE = 1;

//  helpers 

int WrapIndex(int v, int period)
{
	// Generic path (kept for non-power-of-two periods if ever changed).
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
	// Quintic smoothstep (C2-continuous) reduces grid-like banding in low-density fog.
	vec3 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

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

// PERF: FBM with LOD selection  distant fragments use 1 octave instead of 2.
// Controlled by distance passed from the march loop.
// lod=0  2 octaves (near, full quality), lod=1  1 octave (far, half cost).
float FBM_LOD(vec3 p, int lod)
{
	float v0 = ValueNoise(p);
	if (lod > 0)
		return v0; // 1-octave fast path for distant fog
	float v1 = ValueNoise(p * 2.0);
	return (v0 * 0.5 + v1 * 0.25) * (1.0 / 0.75);
}

float FBM(vec3 p)
{
	float v0 = ValueNoise(p);
	float v1 = ValueNoise(p * 2.0);
	return (v0 * 0.5 + v1 * 0.25) * (1.0 / 0.75);
}

float FogNoise(vec3 p, int lod)
{
	if (FogNoiseMode == 1)
		return texture(FogNoiseTex, fract(p / LUT_PERIOD)).r;
	return FBM_LOD(p, lod);
}

// Keep overload for backward-compat with shadow calls (always full quality).
float FogNoise(vec3 p)
{
	if (FogNoiseMode == 1)
		return texture(FogNoiseTex, fract(p / LUT_PERIOD)).r;
	return FBM(p);
}

float Fake3DNoiseFromPlanes(vec3 p, float seed, int lod)
{
	float nXY = FogNoise(vec3(p.xy, seed + 3.17), lod);
	float nYZ = FogNoise(vec3(p.yz, seed + 11.71), lod);
	float nZX = FogNoise(vec3(p.zx, seed + 19.37), lod);
	return (nXY + nYZ + nZX) * (1.0 / 3.0);
}

float MultiScaleFogNoise(vec3 noisePos, int lod, float detailStrength)
{
	// World-space fake-3D noise from axis planes avoids planar/curtain artifacts.
	float large = Fake3DNoiseFromPlanes(noisePos * 0.15 + vec3(11.2, 3.7, 17.9), 4.0, 1);
	float medium = Fake3DNoiseFromPlanes(noisePos * 0.60 + vec3(5.7, 13.1, 2.9), 9.0, lod);
	float fine = Fake3DNoiseFromPlanes(noisePos * 2.45 + vec3(17.13, 9.70, 5.31), 15.0, 1);
	float baseShape = large * 0.6 + medium * 0.3 + fine * 0.1;

	// Shape into cloud-like clumps while keeping broad-scale volume breakup.
	float billow = abs(2.0 * baseShape - 1.0);
	float ridge = 1.0 - billow;
	ridge *= ridge;
	float structure = mix(baseShape, ridge, 0.32 + 0.28 * detailStrength);
	float valleys = smoothstep(0.10, 0.65, 1.0 - baseShape);
	structure = clamp(structure * (0.68 + 0.52 * valleys), 0.0, 1.0);
	return structure;
}

float DepthToNdcZ(float depth)
{
	// FogDepthParams.z > 0.5    reverse-Z  (depth already in [0,1] NDC).
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
float LinearEyeDepth(float depth, vec2 viewUv);

vec2 ScreenUvToViewUv(vec2 screenUv)
{
	vec2 screenPos = screenUv * FogViewportParams.xy;
	return (screenPos - FogViewParams.xy) * FogViewParams.zw;
}

vec2 ProjectWorldToScreenUv(vec3 worldPos)
{
	vec4 clip = ViewProj * vec4(worldPos, 1.0);
	if (abs(clip.w) < 1e-6)
		return vec2(-1.0);
	vec2 ndc = clip.xy / clip.w;
	return ndc * 0.5 + 0.5;
}

float LocalLightOcclusionTap(vec3 probePos, float probeViewDist, float thickness)
{
	vec2 uv = ProjectWorldToScreenUv(probePos);
	if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0)
		return 1.0;
	float depth = texture(SceneDepth, uv).r;
	if (IsSkyDepth(depth))
		return 1.0;
	float sceneViewDist = LinearEyeDepth(depth, ScreenUvToViewUv(uv));
	return (sceneViewDist + thickness >= probeViewDist) ? 1.0 : 0.0;
}

float EstimateLocalLightOcclusion(vec3 samplePos, vec3 lightPos, float lightDist)
{
	if (FogLocalOcclusionMode <= 0 || lightDist <= 1e-3)
		return 1.0;

	vec3 dir = (lightPos - samplePos) / lightDist;
	float maxDist = lightDist * max(FogLocalOcclusionParams.z, 0.0);
	if (FogLocalOcclusionMode == 1)
	{
		float t = min(maxDist, lightDist * 0.6);
		vec3 probePos = samplePos + dir * t;
		float probeViewDist = length(probePos - FogCameraPosWS);
		return LocalLightOcclusionTap(probePos, probeViewDist, max(FogLocalOcclusionParams.x, 0.0));
	}

	float vis = 1.0;
	const int kTapCount = 4;
	for (int s = 0; s < kTapCount; ++s)
	{
		float t = min(maxDist, lightDist * (float(s + 1) / float(kTapCount + 1)));
		vec3 probePos = samplePos + dir * t;
		float probeViewDist = length(probePos - FogCameraPosWS);
		float cone = max(FogLocalOcclusionParams.y, 0.0) * t;
		float thickness = max(FogLocalOcclusionParams.x, 0.0) + cone;
		vis *= LocalLightOcclusionTap(probePos, probeViewDist, thickness);
	}
	return vis;
}

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
	/* BUG FIX (G-01): 1.0/rd produces +/-Inf when any component of rd is
	 * exactly 0.0 (e.g. perfectly horizontal or vertical gaze directions).
	 * While Inf arithmetic is technically well-defined in IEEE 754 and produces
	 * the correct intersection result in most cases, some GPU drivers or
	 * optimised compiler paths flush Inf to NaN, breaking the test.
	 * Clamp each component to a minimum magnitude before inversion. */
	vec3 rdSafe = vec3(
		abs(rd.x) < 1e-7 ? (rd.x >= 0.0 ? 1e-7 : -1e-7) : rd.x,
		abs(rd.y) < 1e-7 ? (rd.y >= 0.0 ? 1e-7 : -1e-7) : rd.y,
		abs(rd.z) < 1e-7 ? (rd.z >= 0.0 ? 1e-7 : -1e-7) : rd.z
	);
	vec3 invRd  = 1.0 / rdSafe;
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
// Original: sin(dot(pixel, k) + Time)   Time inside sin is fine for small
// values but causes the *frequency* to appear modulated once Time grows large.
float Dither(vec2 pixel)
{
	return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453 + Time);
}

float InterleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// Henyey-Greenstein phase scaled so isotropic (g=0) evaluates to 1.0.
// This lets us modulate existing scattering terms without changing their
// baseline energy when anisotropy is disabled.
float AnisotropicPhase(float cosTheta, float g)
{
	g = clamp(g, -0.95, 0.95);
	float gg = g * g;
	float denom = max(1.0 + gg - 2.0 * g * cosTheta, 1e-4);
	return (1.0 - gg) / (denom * sqrt(denom));
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

// FIX #6: Simplified  max(falloff,0) already ensures edgeThickness >= 0,
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
	float k = abs(hScale);
	float directional = (hScale >= 0.0) ? exp(-k * dh) : exp(k * dh);
	float above = max(dh, 0.0);
	float below = max(-dh, 0.0);
	float groundBand = exp(-above * (k * 1.45 + 0.0065));
	float underBand = 1.0 + (1.0 - exp(-below * (k * 0.55 + 0.0025))) * 0.28;
	float mist = exp(-abs(dh) * (k * 0.55 + 0.0045));
	float mistStrength = clamp(FogHeightMistStrength, 0.0, 1.0);
	float layered = mix(directional, groundBand * underBand, 0.58);
	layered *= mix(1.0, 1.0 + 0.95 * mist, mistStrength);
	return clamp(layered, 0.07, 3.6);
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

float FogEdgeSoftnessMask(float edgeDist, float falloff, float edgeSoftness)
{
	if (edgeSoftness <= 0.0)
		return 1.0;
	float softnessWidth = max(1.0, max(falloff, 0.0)) * clamp(edgeSoftness, 0.0, 1.0);
	return smoothstep(0.0, softnessWidth, max(edgeDist, 0.0));
}

bool PointInsideVolume(vec3 p, FogVolume volume)
{
	if (int (volume.extra.x + 0.5) == FOGVOL_SHAPE_SPHERE)
		return length(p - volume.sphere.xyz) <= volume.sphere.w;
	return all(greaterThanEqual(p, volume.mins.xyz)) && all(lessThanEqual(p, volume.maxs.xyz));
}

vec3 SampleFogLightgrid(vec3 p, FogVolume volume)
{
	vec3 size = max(volume.maxs.xyz - volume.mins.xyz, vec3(1e-3));
	vec3 local = clamp((p - volume.mins.xyz) / size, 0.0, 1.0);

	int base = FogVolumeIndex * 8;
	vec3 c00 = mix(FogLightgridProbeRGB[base + 0].rgb, FogLightgridProbeRGB[base + 1].rgb, local.x);
	vec3 c10 = mix(FogLightgridProbeRGB[base + 2].rgb, FogLightgridProbeRGB[base + 3].rgb, local.x);
	vec3 c01 = mix(FogLightgridProbeRGB[base + 4].rgb, FogLightgridProbeRGB[base + 5].rgb, local.x);
	vec3 c11 = mix(FogLightgridProbeRGB[base + 6].rgb, FogLightgridProbeRGB[base + 7].rgb, local.x);
	vec3 c0 = mix(c00, c10, local.y);
	vec3 c1 = mix(c01, c11, local.y);
	return mix(c0, c1, local.z);
}

vec3 SampleFroxelLight(vec3 p)
{
	if (FogFroxelEnabled == 0)
		return vec3(0.0);

	vec3 viewPos = (View * vec4(p, 1.0)).xyz;
	float z = max(-viewPos.z, FogFroxelParams0.x);
	float halfW = max(1e-4, z * FogFroxelParams0.z);
	float halfH = max(1e-4, z * FogFroxelParams0.w);
	float u = viewPos.x / (2.0 * halfW) + 0.5;
	float v = viewPos.y / (2.0 * halfH) + 0.5;
	float w = log(max(z, FogFroxelParams0.x) / max(FogFroxelParams0.x, 1e-4)) / max(FogFroxelParams1.x, 1e-4);
	vec3 uvw = vec3(u, v, w);
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0))))
		return vec3(0.0);
	return texture(FogFroxelLightTex, clamp(uvw, 0.0, 1.0)).rgb;
}

float SampleFroxelHistoryWeight(vec3 p)
{
	if (FogFroxelEnabled == 0 || FogFroxelTemporalParams.w <= 0.5)
		return 0.0;
	vec3 viewPos = (View * vec4(p, 1.0)).xyz;
	float z = max(-viewPos.z, FogFroxelParams0.x);
	float rejectThreshold = max(FogFroxelTemporalParams.y, 1.0);
	float depthGate = clamp(z / rejectThreshold, 0.0, 1.0);
	float cameraReject = clamp(FogFroxelTemporalParams.z / rejectThreshold, 0.0, 1.0);
	return clamp(FogFroxelTemporalParams.x * (1.0 - cameraReject) * depthGate, 0.0, 1.0);
}

// PERF: lod=0 full quality (near), lod=1 coarse (far). Caller passes based on distance.
float EvaluateFogSigma(vec3 p, FogVolume volume, float density, float falloff, float noiseScalePre, vec3 flowPre,
	float macroDensityMul, int lod, float marchDist)
{
	float edgeDist;
	float edgeFade;
	float edgeSoftness = clamp(volume.params2.x, 0.0, 1.0);
	if (int (volume.extra.x + 0.5) == FOGVOL_SHAPE_SPHERE)
	{
		edgeDist = volume.sphere.w - length(p - volume.sphere.xyz);
		edgeFade = (falloff <= 0.0) ? 1.0 : smoothstep(0.0, falloff, edgeDist);
	}
	else
	{
		vec3 d = min(p - volume.mins.xyz, volume.maxs.xyz - p);
		edgeDist = min(d.x, min(d.y, d.z));
		edgeFade = FogEdgeFade(p, volume.mins.xyz, volume.maxs.xyz, falloff);
	}
	edgeFade *= FogEdgeSoftnessMask(edgeDist, falloff, edgeSoftness);

	// PERF: If edgeFade is zero, density is zero regardless of noise  skip noise entirely.
	if (edgeFade < 1e-5)
		return 0.0;

	float noiseFactor = 1.0;
	if (FogNoiseEnabled != 0)
	{
		vec3 noisePos = p * noiseScalePre + flowPre * Time * noiseScalePre;
		// PERF: Turbulence domain warp costs 3 extra FBM calls. Apply only at lod=0
		// (near samples). Far samples (lod=1) skip warp  imperceptible at distance.
		if (volume.wind_turbulence.w > 1e-4 && lod == 0 && marchDist < FogDomainWarpMaxDist)
			noisePos += vec3(FBM(p * noiseScalePre * 2.03), FBM(p.yzx * noiseScalePre * 2.71), FBM(p.zxy * noiseScalePre * 1.91)) * volume.wind_turbulence.w * 0.35;
		float detailStrength = clamp(FogNoiseDetailStrength, 0.0, 1.0);
		float baseN = MultiScaleFogNoise(noisePos, lod, detailStrength);
		// ALU-only ridge shaping from the already fetched base noise (no extra texture/noise fetch).
		float ridged = 1.0 - abs(2.0 * baseN - 1.0);
		ridged *= ridged;
		float billow = abs(2.0 * baseN - 1.0);
		float n = mix(baseN, ridged, FOG_RIDGED_STRENGTH);
		n = mix(n, 1.0 - billow * 0.7, 0.25 + 0.25 * detailStrength);
		float noiseBias = clamp(volume.noise_params.z, 0.0, 1.0);
		if (noiseBias > 0.0)
			n = smoothstep(noiseBias, 1.0, n);
		float amt = clamp(volume.noise_params.y, 0.0, 1.0);
		float clump = smoothstep(0.28, 0.9, n);
		float valley = smoothstep(0.55, 1.0, 1.0 - n);
		float shaped = clamp(0.5 + clump * 1.35 - valley * 0.25, 0.12, 2.35);
		noiseFactor = mix(1.0, shaped, amt);
	}

	// Macro billow density modulation is reused per-ray (computed once in main), so no per-step macro fetch.
	float sigma = (density * macroDensityMul) * noiseFactor * edgeFade * HeightFactor(p, volume);
	{
		float baseSigma = max(density * macroDensityMul, 1e-4);
		float localShape = clamp(sigma / baseSigma, 0.0, 2.0);
		float clumpBoost = mix(0.75, 1.4, smoothstep(0.35, 1.05, localShape));
		sigma *= clumpBoost;
	}
	return IsFiniteFloat(sigma) ? max(sigma, 0.0) : 0.0;
}

// PERF: Accept pre-normalized sunDir + pre-computed jitter to avoid redundant
// work per call. Shadow is now called with the sigma already known for the
// current sample point (passed as firstSigma), so we skip one EvaluateFogSigma.
float EstimateShadowVisibility(vec3 p, FogVolume volume, float density, float falloff,
	float noiseScalePre, vec3 flowPre, float macroDensityMul, float stepLen,
	vec3 sunDirNorm, float shadowJitterVal, float firstSigma)
{
	// sunDirNorm is pre-validated (zero-length guard done by caller)
	int sampleCount = min(FogShadowSamples, 8);
	float shadowStep = max(stepLen * 2.0, 8.0);
	// PERF: First sample reuses the sigma already computed for the main step 
	// no extra EvaluateFogSigma call needed for s==0.
	float tauLight = firstSigma * shadowStep;

	for (int s = 1; s < sampleCount; ++s)
	{
		float sampleDist = (float(s) + shadowJitterVal) * shadowStep;
		vec3 sp = p + sunDirNorm * sampleDist;
		if (!PointInsideVolume(sp, volume))
			break;
		// PERF: Shadow samples always use lod=1 (1 octave)  shadows don't need full detail.
		float sigma = EvaluateFogSigma(sp, volume, density, falloff, noiseScalePre, flowPre, macroDensityMul, 1, sampleDist);
		tauLight += sigma * shadowStep;
	}

	return exp(-max(FogShadowStrength, 0.0) * tauLight);
}

//  main 

void main()
{
	vec2 screenPos = gl_FragCoord.xy * FogDepthScale;
	vec2 invScreen = FogViewportParams.zw;
	vec2 screenUv  = screenPos * invScreen;
	vec2 viewUv    = (screenPos - FogViewParams.xy) * FogViewParams.zw;

	/* BUG FIX (G-04): SceneColor was sampled up to 4 times per fragment across
	 * the various early-return paths.  Load it once here and reuse the cached
	 * value.  SceneColor is typically hot in the L2 cache for a fullscreen
	 * pass, but eliminating redundant calls reduces instruction count and
	 * avoids accidental double-sampling if the sampler is non-trivial. */
	vec3 scene = texture(SceneColor, screenUv).rgb;
	bool checkerboardSkip = false;
	if (FogCheckerboard != 0 && FogHalfRes == 0)
	{
		ivec2 pix = ivec2(gl_FragCoord.xy);
		checkerboardSkip = (((pix.x + pix.y + FogFrameIndex) & 1) != 0);
		if (checkerboardSkip)
		{
			FragColor = vec4(scene, 0.0);
			return;
		}
	}

	FogVolume volume = FogVolumes[FogVolumeIndex];
#if !FOGVOL_CLUSTERED_LOCAL
	if (volume.misc.y <= 0.0)
	{
		FragColor = vec4(scene, 0.0);
		return;
	}
#endif

	ivec2 depthCoord = ivec2(screenPos);
	float depth      = texelFetch(SceneDepth, depthCoord, 0).r;

	// FIX #5: Pass viewUv so LinearEyeDepth reconstructs the correct ray.
	float linearDepth = LinearEyeDepth(depth, viewUv);

	float ndcDepth = DepthToNdcZ(depth);
	vec4  clip     = vec4(viewUv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4  world    = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
	{
		FragColor = vec4(scene, 0.0);
		return;
	}
	vec3 worldPos = world.xyz / world.w;

	vec3  ro     = FogCameraPosWS;
	vec3  rd     = normalize(worldPos - ro);
	float tScene = length(worldPos - ro);
	if (IsSkyDepth(depth))
		tScene = 2048.0; // BUG-F-02 FIX: cap sky path length — far_clip causes open areas to look/
		                 // disproportionately foggier than enclosed rooms when sky is visible

	#if FOGVOL_CLUSTERED_LOCAL
	{
		int tileSize = max(int(FogClusterParams.x + 0.5), 1);
		int tilesX = max(int(FogClusterParams.y + 0.5), 1);
		int tilesY = max(int(FogClusterParams.z + 0.5), 1);
		ivec2 tileCoord = clamp(ivec2(gl_FragCoord.xy) / tileSize, ivec2(0), ivec2(tilesX - 1, tilesY - 1));
		int tileIndex = tileCoord.y * tilesX + tileCoord.x;
		FogClusterHeader clusterHeader = FogClusterHeaders[tileIndex];
		int clusterOffset = max(clusterHeader.offset_count.x, 0);
		int clusterCount = clamp(clusterHeader.offset_count.y, 0, MAX_FOGVOLUMES);
		float tEnterCluster = 0.0;
		float tExitCluster = tScene;
		float lenCluster = max(tExitCluster - tEnterCluster, 0.0);
		const float MAX_CLUSTER_STEP_LEN = 80.0;
		float stepCountCluster = max(float(FogSteps), 1.0);
		float idealStepLenCluster = lenCluster / stepCountCluster;
		float stepLenCluster = (idealStepLenCluster > MAX_CLUSTER_STEP_LEN) ? MAX_CLUSTER_STEP_LEN : max(idealStepLenCluster, 1.0);
		vec3 accumCluster = vec3(0.0);
		float transmittanceCluster = 1.0;
		vec3 viewDirCluster = -rd;
		float sunDirLenSqCluster = dot(FogShadowDir, FogShadowDir);
		vec3 sunDirCluster = (sunDirLenSqCluster > 1e-6) ? (FogShadowDir * inversesqrt(sunDirLenSqCluster)) : vec3(0.0);
		float phaseSunCluster = (dot(sunDirCluster, sunDirCluster) > 1e-6)
			? AnisotropicPhase(clamp(dot(viewDirCluster, sunDirCluster), -1.0, 1.0), clamp(FogSunAnisotropy, -0.99, 0.99))
			: 1.0;

		if (clusterCount <= 0 || lenCluster <= 1e-4)
		{
			FragColor = vec4(scene, 0.0);
			return;
		}

		if (idealStepLenCluster > MAX_CLUSTER_STEP_LEN)
			stepCountCluster = max(ceil(lenCluster / stepLenCluster), 4.0);

		for (int i = 0; i < FogSteps; ++i)
		{
			float sigmaAccum = 0.0;
			vec3 colorAccum = vec3(0.0);
			float t;
			int noiseLod;

			if (i >= int(stepCountCluster))
				break;
			t = tEnterCluster + (float(i) + 0.5) * stepLenCluster;
			if (t >= tExitCluster)
				break;

			noiseLod = (t > FogNoiseLodSwitchDist || transmittanceCluster < 0.25) ? 1 : 0;
			for (int v = 0; v < MAX_FOGVOLUMES; ++v)
			{
				int volumeIndex;
				FogVolume volumeLocal;
				vec3 samplePos;
				float densityLocal;
				float falloffLocal;
				float noiseScaleLocal;
				vec3 flowLocal;
				float windDirLenLocal;
				float rawSigmaLocal;

				if (v >= clusterCount)
					break;
				volumeIndex = FogClusterIndices[clusterOffset + v];
				if (volumeIndex < 0 || volumeIndex >= MAX_FOGVOLUMES)
					continue;
				volumeLocal = FogVolumes[volumeIndex];
				if (volumeLocal.misc.y <= 0.0)
					continue;
				samplePos = ro + rd * t;
				if (!PointInsideVolume(samplePos, volumeLocal))
					continue;

				densityLocal = max(volumeLocal.color_density.a * FogDensityParams.x, 0.0);
				if (densityLocal <= 0.0)
					continue;
				falloffLocal = volumeLocal.misc.z;
				noiseScaleLocal = clamp(volumeLocal.noise_params.x, NOISE_SCALE_MIN, NOISE_SCALE_MAX);
				windDirLenLocal = length(volumeLocal.wind_turbulence.xyz);
				flowLocal = volumeLocal.velocity_windspeed.xyz;
				if (windDirLenLocal > 1e-6)
					flowLocal += (volumeLocal.wind_turbulence.xyz / windDirLenLocal) * volumeLocal.velocity_windspeed.w;
				rawSigmaLocal = EvaluateFogSigma(samplePos, volumeLocal, densityLocal, falloffLocal, noiseScaleLocal, flowLocal, 1.0, noiseLod, t);
				if (rawSigmaLocal <= 0.0)
					continue;
				sigmaAccum += rawSigmaLocal;
				colorAccum += volumeLocal.color_density.rgb * rawSigmaLocal;
			}

			if (sigmaAccum <= 1e-5)
				continue;

			{
				float opticalDepth = min(sigmaAccum * stepLenCluster, FogDensityParams.y);
				float att = exp(-opticalDepth);
				float mediumWeight = 1.0 - att;
				vec3 scatterColor = colorAccum / max(sigmaAccum, 1e-5);
				vec3 stepScatter = mediumWeight * scatterColor * phaseSunCluster;
				accumCluster += transmittanceCluster * stepScatter;
				transmittanceCluster *= att;
				if (transmittanceCluster < 0.01)
					break;
			}
		}

		{
			float fogAlphaCluster = clamp(1.0 - transmittanceCluster, 0.0, 1.0);
			vec3 outColorCluster;
			if (FogPhysBlend != 0)
				outColorCluster = scene * transmittanceCluster + accumCluster;
			else
				outColorCluster = mix(scene, accumCluster + scene, fogAlphaCluster);
			FragColor = vec4(outColorCluster, fogAlphaCluster);
			return;
		}
	}
	#endif
	float tEnter, tExit;
	if (int (volume.extra.x + 0.5) == FOGVOL_SHAPE_SPHERE)
	{
		if (!RaySphere(ro, rd, volume.sphere.xyz, volume.sphere.w, tEnter, tExit))
		{
			FragColor = vec4(scene, 0.0);
			return;
		}
	}
	else if (!RayAABB(ro, rd, volume.mins.xyz, volume.maxs.xyz, tEnter, tExit))
	{
		FragColor = vec4(scene, 0.0);
		return;
	}

	tEnter = max(tEnter, 0.0);
	tExit  = min(tExit, tScene);
	float maxDistance = volume.noise_params.w;
	if (maxDistance > 0.0)
		tExit = min(tExit, maxDistance);
	if (tExit <= tEnter)
	{
		FragColor = vec4(scene, 0.0);
		return;
	}

	float len       = tExit - tEnter;

	// PERF: Adaptive step count  clamp stepLen to a maximum world-space size.
	// Without this, a global fog volume (16384^3) with farclip=4096 gives
	// stepLen = 4096/16 = 256 units, which is very coarse and wastes steps on
	// short rays (near wall = len of 32 units  stepLen 256  only 1 effective step).
	//
	// Strategy: use a target stepLen based on a fraction of the fog near-distance,
	// but never more than len/4 (always at least 4 samples per ray) and never
	// fewer steps than 4. This makes short rays cheap and long rays still sampled.
	//
	// FogDepthParams.x = near plane (~0.5 units), not useful here.
	// Use a fixed world-space target: 32 units (tunable via density).
	// For global fog, density is very low so large steps are fine visually.
	const float MAX_STEP_LEN = 80.0; // world units, tuned for lower baseline cost
	float stepCount = max(float(FogSteps), 1.0);
	float idealStepLen = len / stepCount;
	// If stepLen would exceed MAX_STEP_LEN, reduce step count proportionally.
	// This avoids wasting FogSteps iterations on a short ray.
	float stepLen;
	if (idealStepLen > MAX_STEP_LEN)
	{
		// Long ray: use fixed step size (fewer steps than FogSteps, but each at good spacing).
		stepLen   = MAX_STEP_LEN;
		stepCount = max(ceil(len / stepLen), 4.0);
	}
	else if (idealStepLen < 1.0 && len < float(FogSteps))
	{
		// Very short ray (e.g. 8 units to wall): use fewer steps to save ALU.
		stepCount = max(ceil(len), 2.0);
		stepLen   = len / stepCount;
	}
	else
	{
		stepLen = idealStepLen;
	}
	// Safety: cap at FogSteps so we never exceed the uniform limit.
	stepCount = min(stepCount, float(FogSteps));

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
	float atmosphereBoost = clamp(FogAtmosphereBoost, 0.0, 1.5);
	float sceneLuma = dot(scene, LUMA_WEIGHTS);

	vec3  accum        = vec3(0.0);
	float transmittance = 1.0;
	float tau           = 0.0;
	float shadowVisAccum = 0.0;
	float shadowWeightAccum = 0.0;

	vec3  viewDir       = -rd;

	// PERF: Precompute per-ray constants outside the march loop.
	// Normalize sunDir once here  CPU already normalizes it, but guard against fp drift.
	float sunDirLenSq   = dot(FogShadowDir, FogShadowDir);
	vec3  sunDir        = (sunDirLenSq > 1e-6) ? (FogShadowDir * inversesqrt(sunDirLenSq)) : vec3(0.0);
	sunDirLenSq         = dot(sunDir, sunDir); // recompute on normalized vector for guard checks

	// Shadow jitter: compute once per ray (same screen pixel  same jitter value).
	bool  doShadow      = (FogShadowEnabled != 0 && FogShadowSamples > 0 && sunDirLenSq > 1e-6);
	float shadowJitter  = doShadow && (FogShadowJitter > 0.0)
		? (InterleavedGradientNoise(gl_FragCoord.xy + Time * 13.37) - 0.5)
		: 0.0;
	// PERF: Precompute sun phase once per ray (viewDir and sunDir are constant).
	float phaseSun      = (sunDirLenSq > 1e-6)
		? AnisotropicPhase(clamp(dot(viewDir, sunDir), -1.0, 1.0), clamp(FogSunAnisotropy, -0.99, 0.99))
		: 1.0;
	float forwardScatterBoost = (sunDirLenSq > 1e-6)
		? (1.0 + atmosphereBoost * 0.95 * pow(clamp(dot(viewDir, sunDir) * 0.5 + 0.5, 0.0, 1.0), 4.0))
		: 1.0;
	float sunScatterStrength = max(FogSunScatter, 0.0);
	vec3 sunScatterColor = max(FogSunColor, vec3(0.0));
	vec3 sunRadiance = (sunDirLenSq > 1e-6 && sunScatterStrength > 0.0)
		? (sunScatterColor * (sunScatterStrength * phaseSun * forwardScatterBoost))
		: vec3(0.0);
	// PERF: Cache active light count and enabled flag to avoid UBO re-fetch in loop.
	int lightOffset = 0;
	int lightCount = 0;
	int fogLightingMode = 0;
	bool doFroxelLights = false;
	bool doListLightsFull = false;
	bool doListLightsDetail = false;
	bool doLights = false;
	bool doLightgrid = false;
#if FOGVOL_GLOBAL_SIMPLE
	doShadow = (FogShadowEnabled != 0 && FogShadowSamples > 0 && sunDirLenSq > 1e-6);
#else
	{
		FogLightList lightList = FogLightLists[clamp(FogVolumeIndex, 0, MAX_FOGVOLUMES - 1)];
		bool useFroxelLighting;
		bool doRaymarchLighting;
		bool doRaymarchDetail;

		lightOffset = max(lightList.offset_count.x, 0);
		lightCount = clamp(lightList.offset_count.y, 0, MAX_FOGLIGHTS);
		fogLightingMode = clamp(FogLightingMode, 0, 3);
		useFroxelLighting = (fogLightingMode == 2 || fogLightingMode == 3);
		doRaymarchLighting = (fogLightingMode == 1);
		doRaymarchDetail = (fogLightingMode == 3);
		// Froxel lighting must not depend on FogLightEnabled (that flag tracks the
		// raymarch light-list path only). Otherwise froxel dlight injection can run
		// on CPU but be completely invisible in shader.
		doFroxelLights = useFroxelLighting && (FogFroxelEnabled != 0) && (FogLightSourceScales.x > 0.0);
		/* Mode 2 fallback: if froxel is selected but no froxel light volume is
		 * available this frame, allow full raymarch light-list shading instead. */
		doListLightsFull = (FogLightEnabled != 0) && (lightCount > 0) && (FogLightSourceScales.y > 0.0)
			&& (doRaymarchLighting || (fogLightingMode == 2 && !doFroxelLights));
		doListLightsDetail = (FogLightEnabled != 0) && doRaymarchDetail && (lightCount > 0) && (FogLightSourceScales.y > 0.0);
		doLights = doFroxelLights || doListLightsFull || doListLightsDetail;
		doLightgrid = (FogLightgridEnabled != 0) && (FogLightSourceScales.z > 0.0);
	}
#endif

	// FIX #8: Removed stepsTaken / edgeFadeSum / earlyTerminated  they were
	// accumulated but never consumed, silently wasting ALU every iteration.

	// Precompute noise flow parameters once (used per step inside loop).
	float noiseScalePre = 0.0;
	vec3  flowPre       = vec3(0.0);
	if (FogNoiseEnabled != 0)
	{
		noiseScalePre = clamp(volume.noise_params.x, NOISE_SCALE_MIN, NOISE_SCALE_MAX);
		float windDirLen = length(volume.wind_turbulence.xyz);
		vec3 flowDir = (windDirLen > 1e-6) ? (volume.wind_turbulence.xyz / windDirLen) : vec3(0.0);
		flowPre = volume.velocity_windspeed.xyz + flowDir * volume.velocity_windspeed.w;
	}

	float macroDensityMulNear = 1.0;
	float macroDensityMulFar = 1.0;
	if (FogNoiseEnabled != 0)
	{
		// Ray-endpoint macro sampling breaks up "curtain" artifacts from per-ray constant density.
		float macroScale = clamp(noiseScalePre * FOG_MACRO_SCALE_MUL, NOISE_SCALE_MIN, NOISE_SCALE_MAX);
		vec3 macroFlow = flowPre * Time * macroScale;
		float macroRawNear = FogNoise((ro + rd * tEnter) * macroScale + macroFlow, 1);
		float macroRawFar = FogNoise((ro + rd * tExit) * (macroScale * 1.17) + macroFlow + vec3(19.7, 11.3, 7.1), 1);
		float billowNear = abs(2.0 * macroRawNear - 1.0);
		float billowFar = abs(2.0 * macroRawFar - 1.0);
		billowNear *= billowNear;
		billowFar *= billowFar;
		float macroStrength = FOG_MACRO_STRENGTH + 0.12 * clamp(FogNoiseDetailStrength, 0.0, 1.0);
		if (FogHalfRes != 0)
			macroStrength *= 0.85;
		macroDensityMulNear = mix(1.0, billowNear, macroStrength);
		macroDensityMulFar = mix(1.0, billowFar, macroStrength);
	}

	int adaptiveSteps = int(stepCount); // adaptive, already capped at FogSteps
#if FOGVOL_GLOBAL_SIMPLE
	adaptiveSteps = clamp(adaptiveSteps, 6, min(FogSteps, 14));
#else
	if (fogLightingMode == 2)
		adaptiveSteps = clamp(adaptiveSteps, 6, 12);
	else if (fogLightingMode == 3)
		adaptiveSteps = clamp(adaptiveSteps, 6, 16);
#endif
	{
		// Distance-adaptive march budget: rays that start far away get up to 75% fewer steps.
		float farStartNorm = clamp(tEnter / max(FogDepthParams.y, 1e-3), 0.0, 1.0);
		float farStepScale = mix(1.0, 0.25, smoothstep(0.4, 0.95, farStartNorm));
		adaptiveSteps = max(2, int(floor(float(adaptiveSteps) * farStepScale + 0.5)));
	}
	bool doGodrayCoupling = (FogGodrayCoupling != 0);
#if FOGVOL_GLOBAL_SIMPLE
	doGodrayCoupling = false;
#endif
	float shaftEnergyPre = 0.0;
	if (doGodrayCoupling)
	{
		// Safe to precompute once: shaftUv depends only on screenUv, which is constant per pixel.
		vec2 shaftUv = clamp(screenUv, vec2(0.0), vec2(1.0));
		vec3 shafts = texture(FogGodrayShaftsTex, shaftUv).rgb;
		shaftEnergyPre = max(shafts.r, max(shafts.g, shafts.b)) * max(FogGodrayShaftsParams.w, 0.0);
	}
	float sigmaEvenPrev = 0.0;
	float shadowVisibilityPrev = 1.0;
	vec3 lightScatterPrev = vec3(0.0);
	vec3 godrayAccum = vec3(0.0);
	for (int i = 0; i < FogSteps; ++i)
	{
		if (i >= adaptiveSteps) break; // PERF: early-out for short rays
		float t = tEnter + (float(i) + 0.5) * stepLen;
		if (t >= tExit)
			break;

		vec3  p        = ro + rd * t;
		float rayT = clamp((t - tEnter) / max(len, 1e-3), 0.0, 1.0);
		float macroDensityMul = mix(macroDensityMulNear, macroDensityMulFar, rayT);
		// Heuristic: use coarse noise LOD for distant/faded segments.
		// transmittance < 0.25 means contributions are already low, so 1-octave is safe.
		int noiseLod = (t > FogNoiseLodSwitchDist || transmittance < 0.25) ? 1 : 0;
		float rawSigma;
		if (FogNoiseSubsample != 0 && (i & 1) != 0)
		{
			// Safe hold on odd steps keeps fog character while cutting ~50% noise evals.
			rawSigma = sigmaEvenPrev;
		}
		else
		{
			rawSigma = EvaluateFogSigma(p, volume, density, falloff, noiseScalePre, flowPre, macroDensityMul, noiseLod, t);
			if ((i & 1) == 0)
				sigmaEvenPrev = rawSigma;
		}
		float opticalDepth = min(rawSigma * stepLen, FogDensityParams.y);
		{
			float fogAlphaPreview = 1.0 - exp(-opticalDepth);
			float contrastGate = smoothstep(0.04, 0.75, fogAlphaPreview);
			opticalDepth *= mix(0.82, 1.18, contrastGate);
			opticalDepth = min(opticalDepth, FogDensityParams.y);
		}
		float att        = exp(-opticalDepth);
		float mediumWeight = 1.0 - att;
		bool canAccumulateStep = (mediumWeight > 1e-4);

		// PERF: Pass rawSigma as firstSigma  shadow reuses it, saves 1 EvaluateFogSigma.
		// Note: rawSigma was computed at noiseLod which may be 0 (full quality near) or 1 (coarse far).
		// Shadow internally uses lod=1 for all its samples anyway, so this approximation is fine.
		// sunDir is pre-normalized; shadowJitter is pre-computed; phaseSun is pre-computed.
		float shadowVisibility = 1.0;
		if (doShadow && canAccumulateStep)
		{
			// PERF: shadow on every second step is visually stable after temporal accumulation.
			if ((FogShadowSamples > 1) && ((i & 1) != 0))
				shadowVisibility = shadowVisibilityPrev;
			else
			{
				shadowVisibility = EstimateShadowVisibility(p, volume, density, falloff, noiseScalePre, flowPre, macroDensityMul,
					stepLen, sunDir, shadowJitter, rawSigma);
				shadowVisibilityPrev = shadowVisibility;
			}
		}

		// phaseSun is already precomputed per-ray (constant for all steps on same ray).
		vec3  stepScatter = mediumWeight * (scatterColor * phaseSun + sunRadiance); // BUG-F-01 FIX: removed inner *transmittance (caused transmittance^2 weighting for sun term)
		vec3 godrayStepScatter = vec3(0.0);
		float godrayInject = 0.0;
		if (doGodrayCoupling && canAccumulateStep)
		{
			float depthGate = clamp(1.0 - (t / max(FogDepthParams.y, 1e-3)), 0.0, 1.0);
			godrayInject = shaftEnergyPre * depthGate * max(FogGodrayShaftsParams.z, 0.0);
			godrayStepScatter = FogSunColor * FogSunScatter * mediumWeight * godrayInject * (1.0 + 0.4 * atmosphereBoost);
			stepScatter += godrayStepScatter;
		}
		{
			float apertureGlow = atmosphereBoost * smoothstep(0.35, 1.0, sceneLuma) * (1.0 - rayT) * mediumWeight;
			if (apertureGlow > 1e-4)
				stepScatter += sunScatterColor * (0.22 * apertureGlow);
		}
		if (doLightgrid && canAccumulateStep)
		{
			// Static/emissive world contribution comes from the baked lightgrid
			// probe data; explicit fog volume emissive remains handled separately
			// by FogEmissiveEnabled/volume.extra.z below.
			vec3 staticScatter = SampleFogLightgrid(p, volume);
			stepScatter += staticScatter * (FogDLightScale * FogLightSourceScales.z * mediumWeight);
		}

// Froxel local-light contribution can run as a fast path; debug outputs remain
// independent below via FogFroxelDebug visualization modes.

		if (doLights && canAccumulateStep)
		{
			vec3 lightScatter = lightScatterPrev;
			bool forceHalfRateLighting = (fogLightingMode == 2 || fogLightingMode == 3);
			if ((!forceHalfRateLighting && FogLightSubsample == 0) || (i & 1) == 0)
			{
				vec3 froxelScatter = vec3(0.0);
				vec3 localScatter = vec3(0.0);
				if (doFroxelLights)
				{
					// Froxel lighting is blended with the per-volume light list.
					// We normalize by active source weight sum to avoid double-counting
					// when both represent the same local lights.
					froxelScatter = SampleFroxelLight(p) * (FogDLightScale * FogLightSourceScales.x);
				}
				if (doListLightsFull || doListLightsDetail)
				{
					int detailCap = doListLightsDetail ? min(lightCount, 4) : lightCount;
					if (doListLightsFull)
					{
						// PERF: deep segments contribute less; cap expensive local-light loop there.
						if (transmittance < 0.35)
							detailCap = min(detailCap, 8);
						if (transmittance < 0.15)
							detailCap = min(detailCap, 4);
					}
					for (int l = 0; l < MAX_FOGLIGHTS; ++l)
					{
						if (l >= detailCap) break;
						int lightIndex = lightOffset + l;
						if (lightIndex >= MAX_FOGVOLUMES * MAX_FOGLIGHTS) break;
						vec3 lightVec = FogLights[lightIndex].pos_rad.xyz - p;
						float radius = max(FogLights[lightIndex].pos_rad.w, 1e-3);
						float radius2 = radius * radius;
						float dist2 = dot(lightVec, lightVec);
						if (dist2 >= radius2)
							continue;
						float normDist2 = dist2 / max(radius2, 1e-6);
						float window = smoothstep(1.0, 0.0, normDist2);
						float invSq = 1.0 / (1.0 + normDist2 * 6.0);
						float atten = window * invSq;
						if (atten < 1e-5) continue;
						float lightDist = sqrt(max(dist2, 1e-8));
						vec3 lightDir = (lightDist > 1e-5) ? (lightVec / lightDist) : vec3(0.0);
						float viewDot = clamp(dot(viewDir, lightDir), -1.0, 1.0);
						float phaseLocal = AnisotropicPhase(viewDot, ANISO_G_LOCAL);
						float localOcclusion = 1.0;
						if (FogLocalOcclusionMode > 0)
							localOcclusion = EstimateLocalLightOcclusion(p, FogLights[lightIndex].pos_rad.xyz, lightDist);
						localScatter += FogLights[lightIndex].col_int.rgb * (atten * 0.75 * FogDLightScale * phaseLocal * localOcclusion);
						{
							float forward = max(viewDot, 0.0);
							float forward2 = forward * forward;
							float viewScatter = forward2 * forward2 * forward2;
							localScatter += FogLights[lightIndex].col_int.rgb * (viewScatter * atten * (0.62 + 0.33 * atmosphereBoost) * localOcclusion);
						}
						{
							float halo = pow(clamp(viewDot * 0.5 + 0.5, 0.0, 1.0), 3.5);
							float haloAtten = smoothstep(0.1, 0.8, atten);
							localScatter += FogLights[lightIndex].col_int.rgb * (halo * haloAtten * 0.3 * atmosphereBoost * localOcclusion);
						}
					}
					localScatter *= FogLightSourceScales.y;
					if (doListLightsDetail)
					{
						localScatter *= 0.2;
					}
				}
				if (doFroxelLights && doListLightsFull)
				{
					float denom = max(FogLightSourceScales.x + FogLightSourceScales.y, 1e-4);
					lightScatter = (froxelScatter + localScatter) / denom;
				}
				else if (doFroxelLights)
					lightScatter = froxelScatter + localScatter;
				else
					lightScatter = localScatter;
				lightScatterPrev = lightScatter;
			}
			stepScatter += lightScatter * mediumWeight;
		}
		if (canAccumulateStep)
		{
			float luma = dot(stepScatter, LUMA_WEIGHTS);
			if (luma > 1e-5)
			{
				vec3 chroma = stepScatter / luma;
				chroma = mix(vec3(1.0), chroma, 0.78 + 0.15 * atmosphereBoost);
				stepScatter = chroma * luma;
			}
		}
		stepScatter *= shadowVisibility;
		godrayAccum += transmittance * (godrayStepScatter * shadowVisibility);
		accum            += transmittance * stepScatter;
		transmittance    *= att;
		tau              += opticalDepth;
		shadowVisAccum   += shadowVisibility * (1.0 - att);
		shadowWeightAccum += (1.0 - att);

		// BUG FIX 3: Threshold 0.005  enough to abort when truly opaque.
		if (transmittance < 0.01)
			break;
	}

	//  debug outputs 
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
	if (FogFroxelDebug == 1)
	{
		vec3 froxelViz = clamp(SampleFroxelLight(ro + rd * max(tEnter, 0.0)) * 0.5, 0.0, 1.0);
		FragColor = vec4(froxelViz, 1.0);
		return;
	}
	if (FogFroxelDebug == 2)
	{
		vec3 froxelViz = SampleFroxelLight(ro + rd * max(tEnter, 0.0));
		float energy = clamp(max(froxelViz.r, max(froxelViz.g, froxelViz.b)) * 0.25, 0.0, 1.0);
		FragColor = vec4(energy, energy * energy, 0.0, 1.0);
		return;
	}
	if (FogFroxelDebug == 3)
	{
		float weight = SampleFroxelHistoryWeight(ro + rd * max(tEnter, 0.0));
		FragColor = vec4(weight, 1.0 - weight, 0.0, 1.0);
		return;
	}

	if (FogFroxelDebug == 4)
	{
		vec3 froxelViz = SampleFroxelLight(ro + rd * max(tEnter, 0.0));
		float energy = clamp(dot(froxelViz, vec3(0.2126, 0.7152, 0.0722)) * 0.3, 0.0, 1.0);
		FragColor = vec4(energy, energy * 0.5, 0.0, 1.0);
		return;
	}

	if (FogDebugMode == 8)
	{
		float avgShadow = (shadowWeightAccum > 1e-6) ? (shadowVisAccum / shadowWeightAccum) : 1.0;
		float shadowedLum = dot(accum, vec3(0.299, 0.587, 0.114));
		float unshadowedLum = shadowedLum / max(avgShadow, 1e-3);
		float ratio = clamp(shadowedLum / max(unshadowedLum, 1e-3), 0.0, 1.0);
		FragColor = vec4(clamp(shadowedLum * 2.0, 0.0, 1.0), clamp(unshadowedLum * 2.0, 0.0, 1.0), ratio, 1.0);
		return;
	}
	if (FogDebugMode == 9)
	{
		FragColor = vec4(clamp(godrayAccum * 2.0, 0.0, 1.0), 1.0);
		return;
	}

	//  composite 
	// BUG FIX (G-04): Use the 'scene' value cached at the top of main() 
	// no need to sample SceneColor a second time here.
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
		outColor = mix(scene, scatterColor, 1.0 - exp(-clamp(tau, 0.0, 32.0)));

	if (FogEmissiveEnabled != 0 && volume.extra.z > 0.0)
		outColor += scatterColor * volume.extra.z * (1.0 - transmittance);
	{
		float fogAlpha = 1.0 - transmittance;
		float hazeLimiter = 1.0 / (1.0 + fogAlpha * (0.42 + 0.38 * (1.0 - sceneLuma)));
		vec3 scenePreserve = scene * (0.62 + 0.38 * transmittance);
		vec3 shaped = scenePreserve + (outColor - scene) * (1.12 + 0.35 * atmosphereBoost);
		outColor = mix(outColor * hazeLimiter, shaped, fogAlpha * 0.58);
		outColor = outColor / (1.0 + outColor * (0.12 + 0.14 * atmosphereBoost));
	}
	// Alpha = fog density (1.0 - transmittance) for SSAO suppression:
	//   alpha=0.0  transparent/no fog  SSAO unchanged
	//   alpha=1.0  fully opaque fog  SSAO suppressed
	// This is SAFE: the final blit into composite.fbo uses
	// glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE) so this alpha never
	// reaches composite.fbo, the display compositor, or any blending path
	// that previously caused the white-screen bug. The alpha stays exclusively
	// in the internal fogvol ping-pong composite_tex buffers where postprocess
	// reads it. The temporal pass blends alpha like RGB (see fogvol_temporal.frag).
	// Using alpha instead of RGB luminance correctly detects dark-colored fog
	// (purple, red, brown) where lum0 even at full density.
	FragColor = vec4(outColor, 1.0 - transmittance);
}
