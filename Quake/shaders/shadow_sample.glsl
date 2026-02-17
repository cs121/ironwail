#ifndef SHADOW_SAMPLE_GLSL
#define SHADOW_SAMPLE_GLSL

#include "depth_common.glsl"

// ShadowViewProj is uploaded as std140 mat4 and consumed as GLSL column-major.
// Keep receiver projection deterministic; adaptive row/column switching here causes
// view-dependent instability/flicker because neighboring fragments can choose
// different paths.
vec4 ShadowMul(mat4 M, vec4 v)
{
    return M * v;
}

float ShadowReference01(float proj_z)
{
    return ShadowReferenceFromProjZ(proj_z);
}

float ShadowTestManual(float receiverDepth, float shadowDepth, float bias, bool isReverseZ)
{
	if (isReverseZ)
		return (receiverDepth >= (shadowDepth - bias)) ? 1.0 : 0.0;
	return (receiverDepth <= (shadowDepth + bias)) ? 1.0 : 0.0;
}

#ifdef SHADOW_SUN

vec3 g_shadow_debug_coord = vec3(0.0);
float g_shadow_debug_inside = 0.0;
float g_shadow_debug_receiver_depth = 0.0;
float g_shadow_debug_sampled_depth = 0.0;
float g_shadow_debug_depth_delta = 0.0;

float ShadowDebugReverseZ()
{
	int compare_mode = int(ShadowDebug.z + 0.5); // 0=auto, 1=LEQUAL, 2=GEQUAL
	bool reversed_auto =
#if REVERSED_Z
		true;
#else
		false;
#endif
	bool use_gequal = (compare_mode == 2) || (compare_mode == 0 && reversed_auto);
	return use_gequal ? 1.0 : 0.0;
}

vec3 ShadowDebugCoordVisualize()
{
	if (g_shadow_debug_inside > 0.5)
		return vec3(clamp(g_shadow_debug_coord.xy, 0.0, 1.0), 0.0);
	return vec3(1.0, 0.0, 0.0);
}

vec3 ShadowDebugReceiverDepthVisualize()
{
	if (g_shadow_debug_inside < 0.5)
		return vec3(1.0, 0.0, 0.0);
	float d = clamp(g_shadow_debug_receiver_depth, 0.0, 1.0);
	return vec3(d);
}

vec3 ShadowDebugSampleDepthVisualize()
{
	if (g_shadow_debug_inside < 0.5)
		return vec3(1.0, 0.0, 0.0);
	float d = clamp(g_shadow_debug_sampled_depth, 0.0, 1.0);
	return vec3(d);
}

vec3 ShadowDebugDeltaVisualize()
{
	if (g_shadow_debug_inside < 0.5)
		return vec3(1.0, 0.0, 0.0);
	float delta = clamp(g_shadow_debug_depth_delta, -1.0, 1.0);
	if (delta >= 0.0)
		return vec3(delta, 0.0, 0.0);
	return vec3(0.0, 0.0, -delta);
}

vec3 ShadowDebugVisualize(int mode, float visibility)
{
	if (mode == 2)
		return ShadowDebugCoordVisualize();
	if (mode == 3)
		return ShadowDebugReceiverDepthVisualize();
	if (mode == 4)
		return ShadowDebugSampleDepthVisualize();
	if (mode == 5)
		return ShadowDebugDeltaVisualize();
	return vec3(clamp(visibility, 0.0, 1.0));
}

float ShadowSampleRaw(vec2 uv, float reference, float bias)
{
	float depth = texture(ShadowMap, uv).r;
	bool reversez = ShadowDebugReverseZ() > 0.5;
	float lit = ShadowTestManual(reference, depth, bias, reversez);
	g_shadow_debug_sampled_depth = depth;
	g_shadow_debug_depth_delta = reference - depth;
	return lit;
}

float ShadowSamplePCF(vec2 uv, float reference, float bias, int taps)
{
	vec2 texel = 1.0 / vec2(textureSize(ShadowMap, 0));
	float sum = 0.0;
	int count = 0;

	if (taps <= 2)
	{
		vec2 offsets[4] = vec2[4](
			vec2(-0.5, -0.5),
			vec2(0.5, -0.5),
			vec2(-0.5, 0.5),
			vec2(0.5, 0.5)
		);
		for (int i = 0; i < 4; ++i)
		{
			sum += ShadowSampleRaw(uv + offsets[i] * texel, reference, bias);
		}
		return sum * 0.25;
	}

	if (taps <= 4)
	{
		for (int y = -1; y <= 1; ++y)
		{
			for (int x = -1; x <= 1; ++x)
			{
				sum += ShadowSampleRaw(uv + vec2(x, y) * texel, reference, bias);
				++count;
			}
		}
		return sum / float(count);
	}

	for (int y = -2; y <= 2; ++y)
	{
		for (int x = -2; x <= 2; ++x)
		{
			sum += ShadowSampleRaw(uv + vec2(x, y) * texel, reference, bias);
			++count;
		}
	}

	return sum / float(count);
}

float ShadowVisibility(vec3 world_pos, vec3 normal, out float in_range)
{
	if (ShadowDebug.x < 0.5)
	{
		in_range = 1.0;
		return 1.0;
	}

	vec4 clip = ShadowMul(ShadowViewProj, vec4(world_pos, 1.0));
	if (clip.w <= 0.0)
	{
		in_range = 0.0;
		return 1.0;
	}

	vec3 proj = clip.xyz / clip.w;
	vec2 uv = proj.xy * 0.5 + 0.5;
	float reference = ShadowReference01(proj.z);

	g_shadow_debug_coord = vec3(uv, reference);
	g_shadow_debug_receiver_depth = reference;
	g_shadow_debug_sampled_depth = 0.0;
	g_shadow_debug_depth_delta = 0.0;

	bool inside = all(greaterThanEqual(vec3(uv, reference), vec3(0.0))) &&
		all(lessThanEqual(vec3(uv, reference), vec3(1.0)));
	g_shadow_debug_inside = inside ? 1.0 : 0.0;
	in_range = g_shadow_debug_inside;
	if (!inside)
		return 1.0;

	float ndotl = clamp(dot(normal, -ShadowSunDir.xyz), 0.0, 1.0);
	float bias = ShadowParams.x + ShadowParams.y * (1.0 - ndotl);

	if (ShadowParams.z > 0.5)
		return ShadowSamplePCF(uv, reference, bias, int(ShadowParams.w + 0.5));

	return ShadowSampleRaw(uv, reference, bias);
}
#endif

#ifdef SHADOW_DLIGHT
float ShadowSampleRawDlight(vec2 uv, float reference, float bias)
{
	float depth = texture(ShadowDlightMap, uv).r;
	bool reversez =
#if REVERSED_Z
		true;
#else
		false;
#endif
	return ShadowTestManual(reference, depth, bias, reversez);
}

float ShadowSamplePCFDlight(vec2 uv, float reference, float bias, int taps)
{
	vec2 texel = 1.0 / vec2(textureSize(ShadowDlightMap, 0));
	float sum = 0.0;
	int count = 0;

	if (taps <= 2)
	{
		vec2 offsets[4] = vec2[4](
			vec2(-0.5, -0.5),
			vec2(0.5, -0.5),
			vec2(-0.5, 0.5),
			vec2(0.5, 0.5)
		);
		for (int i = 0; i < 4; ++i)
		{
			sum += ShadowSampleRawDlight(uv + offsets[i] * texel, reference, bias);
		}
		return sum * 0.25;
	}

	if (taps <= 4)
	{
		for (int y = -1; y <= 1; ++y)
		{
			for (int x = -1; x <= 1; ++x)
			{
				sum += ShadowSampleRawDlight(uv + vec2(x, y) * texel, reference, bias);
				++count;
			}
		}
		return sum / float(count);
	}

	for (int y = -2; y <= 2; ++y)
	{
		for (int x = -2; x <= 2; ++x)
		{
			sum += ShadowSampleRawDlight(uv + vec2(x, y) * texel, reference, bias);
			++count;
		}
	}

	return sum / float(count);
}

float ShadowVisibilityDlight(vec3 world_pos, vec3 normal, vec3 light_pos, uint light_index, out float in_range)
{
	if (ShadowDlightParams.z < 0.5)
	{
		in_range = 1.0;
		return 1.0;
	}

	int shadow_index = -1;
	for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
	{
		if (ShadowDlightInfo[i].x < 0.0)
			continue;
		int idx = int(ShadowDlightInfo[i].x + 0.5);
		if (idx == int(light_index))
		{
			shadow_index = i;
			break;
		}
	}

	if (shadow_index < 0)
	{
		in_range = 0.0;
		return 1.0;
	}

	vec4 clip = ShadowMul(ShadowDlightViewProj[shadow_index], vec4(world_pos, 1.0));
	if (clip.w <= 0.0)
	{
		in_range = 0.0;
		return 1.0;
	}

	vec3 proj = clip.xyz / clip.w;
	vec2 uv = proj.xy * 0.5 + 0.5;
	float reference = ShadowReference01(proj.z);

	vec4 atlas = ShadowDlightAtlas[shadow_index];
	uv = uv * atlas.xy + atlas.zw;

	bool inside = all(greaterThanEqual(vec3(uv, reference), vec3(0.0))) &&
		all(lessThanEqual(vec3(uv, reference), vec3(1.0)));
	in_range = inside ? 1.0 : 0.0;
	if (!inside)
		return 1.0;

	vec3 light_dir = normalize(light_pos - world_pos);
	float ndotl = clamp(dot(normal, light_dir), 0.0, 1.0);
	float bias = ShadowDlightParams.x * (1.0 - ndotl);

	int taps = int(ShadowDlightParams.y + 0.5);
	if (taps > 0)
		return ShadowSamplePCFDlight(uv, reference, bias, taps);

	return ShadowSampleRawDlight(uv, reference, bias);
}
#endif

#endif
