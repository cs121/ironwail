#include "frame_uniforms.glsl"

#define MAX_FOGVOLUMES 64

layout(binding=0) uniform sampler2D SceneColor;
layout(binding=1) uniform sampler2D SceneDepth;
layout(binding=3) uniform sampler3D FogNoiseTex;

struct FogVolume
{
	vec4 mins;
	vec4 maxs;
	vec4 color_density;
	vec4 noise_params;
	vec4 velocity;
	vec4 misc;
};

layout(std140, binding=2) uniform FogVolumeUBO
{
	FogVolume FogVolumes[MAX_FOGVOLUMES];
};

layout(location=0) uniform int FogSteps;
layout(location=1) uniform int FogNoiseEnabled;
layout(location=2) uniform int FogDebugMode;
layout(location=3) uniform int FogVolumeIndex;
layout(location=4) uniform mat4 FogInvViewProj;
layout(location=5) uniform int FogNoiseMode;
layout(location=6) uniform int FogPhysBlend;
layout(location=7) uniform int FogJitterEnabled;
layout(location=8) uniform vec3 FogCameraPosWS;
layout(location=9) uniform vec4 FogViewportParams; // xy: screen size, zw: inv screen size
layout(location=10) uniform vec2 FogDepthScale;
layout(location=11) uniform vec4 FogViewParams; // xy: view origin in screen px, zw: inv view size

layout(location=0) out vec4 FragColor;

const int NOISE_PERIOD = 64;
const float NOISE_SCALE_MIN = 0.005;
const float NOISE_SCALE_MAX = 0.5;
const float LUT_PERIOD = 64.0;

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

float FBM(vec3 p)
{
	float sum = 0.0;
	float amp = 0.5;
	float freq = 1.0;
	float norm = 0.0;
	for (int i = 0; i < 3; ++i)
	{
		sum += amp * ValueNoise(p * freq);
		norm += amp;
		freq *= 2.0;
		amp *= 0.5;
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
#if REVERSED_Z
	return depth;
#else
	return depth * 2.0 - 1.0;
#endif
}

bool IsSkyDepth(float depth)
{
#if REVERSED_Z
	return depth <= 0.0001;
#else
	return depth >= 0.9999;
#endif
}

bool RayAABB(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float tEnter, out float tExit)
{
	vec3 invRd = 1.0 / rd;
	vec3 t0 = (bmin - ro) * invRd;
	vec3 t1 = (bmax - ro) * invRd;
	vec3 tmin = min(t0, t1);
	vec3 tmax = max(t0, t1);
	tEnter = max(max(tmin.x, tmin.y), tmin.z);
	tExit = min(min(tmax.x, tmax.y), tmax.z);
	return tExit > max(tEnter, 0.0);
}

float Dither(vec2 pixel)
{
	return fract(sin(dot(pixel, vec2(12.9898, 78.233)) + Time) * 43758.5453);
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

float FogEdgeFade(vec3 p, vec3 bmin, vec3 bmax, float falloff)
{
	float edgeThickness = max(falloff, 0.0);
	if (edgeThickness <= 0.0)
		return 1.0;
	vec3 d = min(p - bmin, bmax - p);
	float edgeDist = min(d.x, min(d.y, d.z));
	return smoothstep(0.0, edgeThickness, edgeDist);
}

void main()
{
	vec2 screenPos = gl_FragCoord.xy * FogDepthScale;
	vec2 invScreen = FogViewportParams.zw;
	vec2 screenUv = screenPos * invScreen;
	vec2 viewUv = (screenPos - FogViewParams.xy) * FogViewParams.zw;

	int _fogIdx = FogVolumeIndex;
	if (_fogIdx < 0 || _fogIdx >= MAX_FOGVOLUMES)
	{
		// Invalid index -> no fog
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}
	FogVolume volume = FogVolumes[_fogIdx];
	if (volume.misc.y <= 0.0)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}

	ivec2 depthCoord = ivec2(screenPos);
	float depth = texelFetch(SceneDepth, depthCoord, 0).r;
	float ndcDepth = DepthToNdcZ(depth);
	vec4 clip = vec4(viewUv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}
	vec3 worldPos = world.xyz / world.w;

	vec3 ro = FogCameraPosWS;
	vec3 toP = worldPos - ro;
	float tScene = length(toP);
	if (tScene < 1e-6)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}
	vec3 rd = toP / tScene;
	if (IsSkyDepth(depth))
		tScene = 1e6;

	float tEnter;
	float tExit;
	if (!RayAABB(ro, rd, volume.mins.xyz, volume.maxs.xyz, tEnter, tExit))
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}

	tEnter = max(tEnter, 0.0);
	tExit = min(tExit, tScene);
	float maxDistance = volume.noise_params.w;
	if (maxDistance > 0.0)
		tExit = min(tExit, maxDistance);
	if (tExit <= tEnter)
	{
		FragColor = vec4(texture(SceneColor, screenUv).rgb, 1.0);
		return;
	}

	float stepCount = max(float(FogSteps), 1.0);
	float len = tExit - tEnter;
	float stepLen = len / stepCount;
	if (FogNoiseEnabled != 0)
	{
		float jitter = Dither(gl_FragCoord.xy);
		if (FogJitterEnabled != 0)
			jitter = InterleavedGradientNoise(gl_FragCoord.xy + vec2(Time * 12.3, Time * 4.7));
		tEnter += jitter * stepLen;
	}

	vec3 scatterColor = volume.color_density.rgb;
	float density = max(volume.color_density.a, 0.0);
	float falloff = volume.misc.z;

	vec3 accum = vec3(0.0);
	float transmittance = 1.0;
	float tau = 0.0;
	float edgeFadeSum = 0.0;
	float edgeFadeSamples = 0.0;
	float stepsTaken = 0.0;
	bool earlyTerminated = false;

	for (int i = 0; i < FogSteps; ++i)
	{
		float t = tEnter + (float(i) + 0.5) * stepLen;
		if (t >= tExit)
			break;
		stepsTaken += 1.0;

		vec3 p = ro + rd * t;
		float edgeFade = FogEdgeFade(p, volume.mins.xyz, volume.maxs.xyz, falloff);
		float noiseFactor = 1.0;
		if (FogNoiseEnabled != 0)
		{
			float noiseScale = clamp(volume.noise_params.x, NOISE_SCALE_MIN, NOISE_SCALE_MAX);
			vec3 noisePos = p * noiseScale + volume.velocity.xyz * Time * noiseScale;
			float n = FogNoise(noisePos);
			float noiseBias = clamp(volume.noise_params.z, 0.0, 1.0);
			if (noiseBias > 0.0)
				n = smoothstep(noiseBias, 1.0, n);
			float amt = clamp(volume.noise_params.y, 0.0, 1.0);
			noiseFactor = mix(1.0, n, amt);
		}

		float sigma = density * noiseFactor * edgeFade;
		float att = exp(-sigma * stepLen);
		vec3 stepScatter = (1.0 - att) * scatterColor;
		accum += transmittance * stepScatter;
		transmittance *= att;
		tau += sigma * stepLen;
		edgeFadeSum += edgeFade;
		edgeFadeSamples += 1.0;
		if (transmittance < 0.01)
		{
			earlyTerminated = true;
			break;
		}
	}

	if (FogDebugMode == 5)
	{
		vec3 debugColor = DebugVolumeColor(float(_fogIdx), volume.misc.x);
		FragColor = vec4(debugColor, 1.0);
		return;
	}
	if (FogDebugMode == 3)
	{
		float tauViz = tau / (1.0 + tau);
		FragColor = vec4(vec3(tauViz), 1.0);
		return;
	}
	if (FogDebugMode == 4)
	{
		float fadeViz = (edgeFadeSamples > 0.0) ? (edgeFadeSum / edgeFadeSamples) : 0.0;
		FragColor = vec4(vec3(fadeViz), 1.0);
		return;
	}
	if (FogDebugMode == 8)
	{
		float stepViz = stepsTaken / max(stepCount, 1.0);
		float earlyViz = earlyTerminated ? 1.0 : 0.0;
		FragColor = vec4(stepViz, earlyViz, 0.0, 1.0);
		return;
	}

	vec3 scene = texture(SceneColor, screenUv).rgb;
	vec3 outColor;
	if (FogPhysBlend != 0)
		outColor = scene * transmittance + accum;
	else
		outColor = mix(scene, scatterColor, clamp(tau, 0.0, 1.0));
	FragColor = vec4(outColor, transmittance);
}
