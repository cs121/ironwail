#include "frame_uniforms.glsl"

#define MAX_FOGVOLUMES 64

#define FOGVOL_SHAPE_BOX 0
#define FOGVOL_SHAPE_SPHERE 1

#define NOISE_SCALE_MIN 0.001
#define NOISE_SCALE_MAX 1.0
#define NOISE_PERIOD 64

layout(binding=0) uniform sampler2D SceneColor;
layout(binding=1) uniform sampler2D SceneDepth;
layout(binding=3) uniform sampler3D FogNoiseTex;
layout(binding=6) uniform sampler3D FogFroxelLightTex;
layout(binding=7) uniform sampler2D FogGodrayShaftsTex;
layout(binding=8) uniform sampler2D FogSunShadowTex;

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

layout(std430, binding=2) readonly buffer FogVolumeBuffer
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
layout(location=9)  uniform vec4  FogViewportParams;
layout(location=10) uniform vec2  FogDepthScale;
layout(location=11) uniform vec4  FogViewParams;
layout(location=12) uniform vec4  FogDepthParams;
layout(location=13) uniform vec4  FogDensityParams; // x densityScale, y sigmaMax, z globalNoiseScale, w globalNoiseBias
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
layout(location=24) uniform float FogNoiseSubsample;
layout(location=25) uniform float FogNoiseLodSwitchDist;
layout(location=26) uniform float FogDomainWarpMaxDist;
layout(location=27) uniform float FogNoiseDetailStrength; // mapped to global noise amount
layout(location=28) uniform float FogDLightScale;
layout(location=29) uniform vec4  FogLightScissor;
layout(location=34) uniform int   FogFroxelEnabled;
layout(location=35) uniform vec4  FogFroxelParams0; // x near, y far, z tanHalfFovX, w tanHalfFovY
layout(location=36) uniform vec4  FogFroxelParams1; // x log(far/near), yzw dims
layout(location=37) uniform int   FogFroxelDebug;
layout(location=38) uniform int   FogFroxelParityMode;
layout(location=39) uniform vec4  FogLightSourceScales;
layout(location=40) uniform int   FogLightingMode;
layout(location=41) uniform int   FogGodrayCoupling;
layout(location=42) uniform int   FogLocalOcclusionMode;
layout(location=43) uniform vec4  FogFroxelTemporalParams;
layout(location=47) uniform int   FogCheckerboard;
layout(location=49) uniform vec4  FogClusterParams;
layout(location=50) uniform mat4  FogSunShadowViewProj;
layout(location=54) uniform vec4  FogSunShadowParams; // x enabled, y depthBias, z pcfUv, w reserved

layout(location=0) out vec4 FragColor;

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

ivec3 WrapIndex(ivec3 v, int period)
{
	ivec3 r = v % period;
	return ivec3((r.x < 0) ? (r.x + period) : r.x,
		(r.y < 0) ? (r.y + period) : r.y,
		(r.z < 0) ? (r.z + period) : r.z);
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

float FBM(vec3 p, int lod)
{
	float v0 = ValueNoise(p);
	if (lod > 0)
		return v0;
	float v1 = ValueNoise(p * 2.0);
	return (v0 * 0.65 + v1 * 0.35);
}

float FogNoiseSample(vec3 p, int lod)
{
	if (FogNoiseMode == 1)
		return texture(FogNoiseTex, fract(p / 64.0)).r;
	return FBM(p, lod);
}

float DepthToNdcZ(float depth)
{
	if (FogDepthParams.z > 0.5)
		return depth;
	return depth * 2.0 - 1.0;
}

bool IsSkyDepth(float depth)
{
	if (FogDepthParams.z > 0.5)
		return depth <= FogDepthParams.w;
	return depth >= FogDepthParams.w;
}

vec2 ScreenUvToViewUv(vec2 screenUv)
{
	vec2 screenPos = screenUv * FogViewportParams.xy;
	return (screenPos - FogViewParams.xy) * FogViewParams.zw;
}

bool ReconstructWorldPos(vec2 viewUv, float depth, out vec3 worldPos)
{
	float ndcDepth = DepthToNdcZ(depth);
	vec4 clip = vec4(viewUv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
		return false;
	worldPos = world.xyz / world.w;
	return true;
}

bool RayAABB(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float tEnter, out float tExit)
{
	vec3 rdSafe = vec3(
		(abs(rd.x) < 1e-6) ? (rd.x >= 0.0 ? 1e-6 : -1e-6) : rd.x,
		(abs(rd.y) < 1e-6) ? (rd.y >= 0.0 ? 1e-6 : -1e-6) : rd.y,
		(abs(rd.z) < 1e-6) ? (rd.z >= 0.0 ? 1e-6 : -1e-6) : rd.z);
	vec3 invRd = 1.0 / rdSafe;
	vec3 t0 = (bmin - ro) * invRd;
	vec3 t1 = (bmax - ro) * invRd;
	vec3 tmin = min(t0, t1);
	vec3 tmax = max(t0, t1);
	tEnter = max(max(tmin.x, tmin.y), tmin.z);
	tExit = min(min(tmax.x, tmax.y), tmax.z);
	return tExit > max(tEnter, 0.0);
}

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
	float hScale = volume.params2.y;
	if (abs(hScale) < 1e-6)
		return 1.0;
	float dh = p.z - volume.extra.w;
	float k = abs(hScale);
	float directional = (hScale >= 0.0) ? exp(-k * dh) : exp(k * dh);
	return clamp(directional, 0.05, 4.0);
}

float EdgeFade(vec3 p, FogVolume volume, float falloff, out float edgeDist)
{
	if (int(volume.misc.y + 0.5) == FOGVOL_SHAPE_SPHERE)
	{
		edgeDist = volume.sphere.w - length(p - volume.sphere.xyz);
		if (falloff <= 0.0)
			return step(0.0, edgeDist);
		return smoothstep(0.0, falloff, edgeDist);
	}

	vec3 d = min(p - volume.mins.xyz, volume.maxs.xyz - p);
	edgeDist = min(d.x, min(d.y, d.z));
	if (falloff <= 0.0)
		return step(0.0, edgeDist);
	return smoothstep(0.0, falloff, edgeDist);
}

float EvaluateFogSigma(vec3 p, FogVolume volume, vec3 flow, int lod)
{
	float density = max(volume.color_density.w * FogDensityParams.x, 0.0);
	float falloff = max(volume.params2.z, 0.0);
	float edgeDist;
	float edge = EdgeFade(p, volume, falloff, edgeDist);
	float edgeSoftness = clamp(volume.params2.x, 0.0, 1.0);
	if (edgeSoftness > 0.0)
	{
		float softWidth = max(1.0, falloff) * edgeSoftness;
		edge *= smoothstep(0.0, softWidth, max(edgeDist, 0.0));
	}
	if (edge <= 1e-6 || density <= 1e-6)
		return 0.0;

	float noiseFactor = 1.0;
	if (FogNoiseEnabled != 0)
	{
		float scale = clamp(max(volume.noise_params.x, 1e-4) * max(FogDensityParams.z, 1e-4), NOISE_SCALE_MIN, NOISE_SCALE_MAX);
		float amount = clamp(volume.noise_params.y * max(FogNoiseDetailStrength, 0.0), 0.0, 2.0);
		float bias = clamp(volume.noise_params.z + FogDensityParams.w, -1.5, 1.5);
		vec3 noisePos = p * scale + flow * Time * scale;
		float n = FogNoiseSample(noisePos, lod);
		float shaped = clamp((n - bias) * 1.4, 0.0, 1.0);
		noiseFactor = mix(1.0, shaped * 2.0, clamp(amount, 0.0, 1.0));
	}

	float sigma = density * edge * HeightFactor(p, volume) * noiseFactor;
	return clamp(sigma, 0.0, max(FogDensityParams.y, 0.001));
}

float AnisotropicPhase(float cosTheta, float g)
{
	g = clamp(g, -0.95, 0.95);
	float gg = g * g;
	float denom = max(1.0 + gg - 2.0 * g * cosTheta, 1e-4);
	return (1.0 - gg) / (denom * sqrt(denom));
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

float SampleSunShadowMapVisibility(vec3 worldPos)
{
	if (FogSunShadowParams.x <= 0.5)
		return 1.0;

	vec4 clip = FogSunShadowViewProj * vec4(worldPos, 1.0);
	if (abs(clip.w) <= 1e-6)
		return 1.0;

	vec3 ndc = clip.xyz / clip.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	float depth = ndc.z * 0.5 + 0.5;
	float bias = max(FogSunShadowParams.y, 0.0) * 2.0;
	float pcf = max(FogSunShadowParams.z, 0.0);

	if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0)
		return 1.0;
	if (depth <= 0.0 || depth >= 1.0)
		return 1.0;

	if (pcf <= 1e-6)
	{
		float closest = texture(FogSunShadowTex, uv).r;
		return (depth - bias <= closest) ? 1.0 : 0.0;
	}

	float sum = 0.0;
	float taps = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			float closest = texture(FogSunShadowTex, uv + vec2(float(x), float(y)) * pcf).r;
			sum += (depth - bias <= closest) ? 1.0 : 0.0;
			taps += 1.0;
		}
	}
	return (taps > 0.0) ? (sum / taps) : 1.0;
}

vec3 SampleSunScatter(vec3 p, vec3 viewDir)
{
	if (FogShadowEnabled == 0 || FogShadowSamples <= 0)
		return vec3(0.0);

	float sunLenSq = dot(FogShadowDir, FogShadowDir);
	if (sunLenSq <= 1e-6)
		return vec3(0.0);

	vec3 sunDir = FogShadowDir * inversesqrt(sunLenSq);
	float vis = SampleSunShadowMapVisibility(p);
	float phase = AnisotropicPhase(clamp(dot(viewDir, sunDir), -1.0, 1.0), 0.45);
	vec3 sunColor = max(SunColorIntensity.rgb * SunColorIntensity.w, vec3(0.0)) * 0.03;
	return sunColor * (phase * vis * max(FogShadowStrength, 0.0));
}

float InterleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

void main()
{
	ivec2 screenPixel = ivec2(floor(gl_FragCoord.xy * FogDepthScale));
	vec2 screenPos = vec2(screenPixel) + vec2(0.5);
	vec2 screenUv = screenPos * FogViewportParams.zw;
	vec2 viewUv = (screenPos - FogViewParams.xy) * FogViewParams.zw;
	vec3 scene = texture(SceneColor, screenUv).rgb;

	if (FogCheckerboard != 0)
	{
		ivec2 pix = ivec2(gl_FragCoord.xy);
		if (((pix.x + pix.y + FogFrameIndex) & 1) != 0)
		{
			FragColor = vec4(scene, 0.0);
			return;
		}
	}

	FogVolume volume = FogVolumes[clamp(FogVolumeIndex, 0, MAX_FOGVOLUMES - 1)];
	if (volume.extra.z <= 0.0)
	{
		FragColor = vec4(scene, 0.0);
		return;
	}

	float depth = texelFetch(SceneDepth, screenPixel, 0).r;
	vec3 worldPos;
	if (!ReconstructWorldPos(viewUv, depth, worldPos))
	{
		FragColor = vec4(scene, 0.0);
		return;
	}

	vec3 ro = FogCameraPosWS;
	vec3 rd = normalize(worldPos - ro);
	vec3 viewDir = -rd;
	float tScene = IsSkyDepth(depth) ? FogDepthParams.y : length(worldPos - ro);

	float tEnter;
	float tExit;
	if (int(volume.misc.y + 0.5) == FOGVOL_SHAPE_SPHERE)
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
	tExit = min(tExit, tScene);
	if (volume.extra.x > 0.0)
		tExit = min(tExit, volume.extra.x);
	if (tExit <= tEnter)
	{
		FragColor = vec4(scene, 0.0);
		return;
	}

	float lengthInVolume = tExit - tEnter;
	int stepCount = max(FogSteps, 1);
	float stepLen = lengthInVolume / float(stepCount);

	if (FogJitterEnabled != 0)
	{
		float jitter = InterleavedGradientNoise(gl_FragCoord.xy + vec2(float(FogFrameIndex) * 0.7548777)) - 0.5;
		tEnter += jitter * stepLen;
	}

	vec3 flow = volume.velocity_windspeed.xyz;
	float windDirLen = length(volume.wind_turbulence.xyz);
	if (windDirLen > 1e-6)
		flow += (volume.wind_turbulence.xyz / windDirLen) * volume.velocity_windspeed.w;

	vec3 accum = vec3(0.0);
	vec3 godrayEnergy = (FogGodrayCoupling != 0) ? texture(FogGodrayShaftsTex, clamp(screenUv, 0.0, 1.0)).rgb : vec3(0.0);
	float transmittance = 1.0;
	float tau = 0.0;

	for (int i = 0; i < FogSteps; ++i)
	{
		if (i >= stepCount)
			break;

		float t = tEnter + (float(i) + 0.5) * stepLen;
		if (t >= tExit)
			break;

		vec3 p = ro + rd * t;
		int noiseLod = (t > FogNoiseLodSwitchDist || transmittance < 0.3) ? 1 : 0;
		float sigma = EvaluateFogSigma(p, volume, flow, noiseLod);
		if (sigma <= 1e-6)
			continue;

		float opticalDepth = min(sigma * stepLen, max(FogDensityParams.y, 0.001));
		float att = exp(-opticalDepth);
		float mediumWeight = 1.0 - att;

		vec3 scattering = vec3(0.0);
		// World-stable baseline in-scattering (no SceneColor coupling).
		scattering += volume.color_density.rgb * 0.16;
		scattering += SampleFroxelLight(p) * max(FogLightSourceScales.x, 0.0);
		scattering += SampleSunScatter(p, viewDir);
		if (FogGodrayCoupling != 0)
			scattering += godrayEnergy * volume.color_density.rgb * 0.25;
		if (FogEmissiveEnabled != 0 && volume.misc.w > 0.0)
			scattering += volume.color_density.rgb * volume.misc.w;

		accum += transmittance * scattering * mediumWeight;
		transmittance *= att;
		tau += opticalDepth;
		if (transmittance < 0.01)
			break;
	}

	if (FogDebugMode == 1)
	{
		FragColor = vec4(volume.color_density.rgb, 1.0);
		return;
	}
	if (FogDebugMode == 2)
	{
		FragColor = vec4(vec3(depth), 1.0);
		return;
	}
	if (FogDebugMode == 3)
	{
		float linearDepth = length(worldPos - FogCameraPosWS) / max(FogDepthParams.y, 1e-3);
		FragColor = vec4(vec3(clamp(linearDepth, 0.0, 1.0)), 1.0);
		return;
	}
	if (FogDebugMode == 5)
	{
		FragColor = vec4(vec3(1.0 - exp(-tau)), 1.0);
		return;
	}
	if (FogDebugMode == 6)
	{
		FragColor = vec4(vec3(clamp(transmittance, 0.0, 1.0)), 1.0);
		return;
	}
	if (FogFroxelDebug != 0)
	{
		vec3 froxelViz = SampleFroxelLight(ro + rd * max(tEnter, 0.0));
		FragColor = vec4(clamp(froxelViz * 0.5, 0.0, 1.0), 1.0);
		return;
	}

	int blendMode = int(floor(volume.misc.z + 0.5));
	if (volume.misc.z < 0.0)
		blendMode = FogBlendModeDefault;

	vec3 outColor;
	if (blendMode == 1)
		outColor = scene + accum;
	else if (FogPhysBlend != 0)
		outColor = scene * transmittance + accum;
	else
		outColor = mix(scene, scene + accum, 1.0 - transmittance);

	FragColor = vec4(outColor, 1.0 - transmittance);
}
