#include "frame_uniforms.glsl"

#define MAX_FOGVOLUMES 64

layout(binding=0) uniform sampler2D SceneColor;
layout(binding=1) uniform sampler2D SceneDepth;

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
layout(location=8) uniform vec3 FogCameraPosWS;
layout(location=9) uniform vec4 FogViewportParams; // xy: size, zw: inv size

layout(location=0) out vec4 FragColor;

float Hash3(vec3 p)
{
	return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float ValueNoise(vec3 p, float period)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	vec3 w = f * f * (3.0 - 2.0 * f);

	vec3 i0 = mod(i, period);
	vec3 i1 = mod(i0 + 1.0, period);

	float n000 = Hash3(i0);
	float n100 = Hash3(vec3(i1.x, i0.y, i0.z));
	float n010 = Hash3(vec3(i0.x, i1.y, i0.z));
	float n110 = Hash3(vec3(i1.x, i1.y, i0.z));
	float n001 = Hash3(vec3(i0.x, i0.y, i1.z));
	float n101 = Hash3(vec3(i1.x, i0.y, i1.z));
	float n011 = Hash3(vec3(i0.x, i1.y, i1.z));
	float n111 = Hash3(i1);

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
	const float period = 16.0;
	float sum = 0.0;
	float amp = 0.5;
	float freq = 1.0;
	float norm = 0.0;
	for (int i = 0; i < 3; ++i)
	{
		sum += amp * ValueNoise(p * freq, period * freq);
		norm += amp;
		freq *= 2.0;
		amp *= 0.5;
	}
	return sum / max(norm, 1e-5);
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

void main()
{
	vec2 invViewport = FogViewportParams.zw;
	vec2 uv = gl_FragCoord.xy * invViewport;

	FogVolume volume = FogVolumes[FogVolumeIndex];
	if (volume.misc.y <= 0.0)
	{
		FragColor = vec4(texture(SceneColor, uv).rgb, 1.0);
		return;
	}

	float depth = texelFetch(SceneDepth, ivec2(gl_FragCoord.xy), 0).r;
	float ndcDepth = DepthToNdcZ(depth);
	vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
	{
		FragColor = vec4(texture(SceneColor, uv).rgb, 1.0);
		return;
	}
	vec3 worldPos = world.xyz / world.w;

	vec3 ro = FogCameraPosWS;
	vec3 rd = normalize(worldPos - ro);
	float tScene = length(worldPos - ro);
	if (IsSkyDepth(depth))
		tScene = 1e6;

	float tEnter;
	float tExit;
	if (!RayAABB(ro, rd, volume.mins.xyz, volume.maxs.xyz, tEnter, tExit))
	{
		FragColor = vec4(texture(SceneColor, uv).rgb, 1.0);
		return;
	}

	tEnter = max(tEnter, 0.0);
	tExit = min(tExit, tScene);
	float maxDistance = volume.noise_params.w;
	if (maxDistance > 0.0)
		tExit = min(tExit, maxDistance);
	if (tExit <= tEnter)
	{
		FragColor = vec4(texture(SceneColor, uv).rgb, 1.0);
		return;
	}

	float stepCount = max(float(FogSteps), 1.0);
	float len = tExit - tEnter;
	float stepLen = len / stepCount;
	if (FogNoiseEnabled != 0)
		tEnter += (Dither(gl_FragCoord.xy) - 0.5) * stepLen;

	vec3 scatterColor = volume.color_density.rgb;
	float density = max(volume.color_density.a, 0.0);

	vec3 accum = vec3(0.0);
	float transmittance = 1.0;

	for (int i = 0; i < FogSteps; ++i)
	{
		float t = tEnter + (float(i) + 0.5) * stepLen;
		if (t >= tExit)
			break;

		vec3 p = ro + rd * t;
		float noiseFactor = 1.0;
		if (FogNoiseEnabled != 0)
		{
			vec3 noisePos = p * volume.noise_params.x + volume.velocity.xyz * Time;
			float n = FBM(noisePos);
			float biased = max(0.0, n + volume.noise_params.z);
			float amt = clamp(volume.noise_params.y, 0.0, 1.0);
			noiseFactor = mix(1.0, biased, amt);
		}

		float sigma = density * noiseFactor;
		float att = exp(-sigma * stepLen);
		vec3 stepScatter = (1.0 - att) * scatterColor;
		accum += transmittance * stepScatter;
		transmittance *= att;
		if (transmittance < 0.01)
			break;
	}

	if (FogDebugMode == 3)
	{
		FragColor = vec4(vec3(1.0 - transmittance), 1.0);
		return;
	}
	if (FogDebugMode == 4)
	{
		vec3 noisePos = (ro + rd * (tEnter + 0.5 * stepLen)) * volume.noise_params.x + volume.velocity.xyz * Time;
		float n = FBM(noisePos);
		FragColor = vec4(vec3(n), 1.0);
		return;
	}

	vec3 scene = texture(SceneColor, uv).rgb;
	vec3 outColor = scene * transmittance + accum;
	FragColor = vec4(outColor, 1.0);
}
