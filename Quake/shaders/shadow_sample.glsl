#ifndef SHADOW_SAMPLE_GLSL
#define SHADOW_SAMPLE_GLSL

#ifdef SHADOW_SUN
void ShadowCoordFromClip(vec4 clip, out vec2 uv, out float reference, out float in_range)
{
	if (clip.w <= 0.0)
	{
		uv = vec2(0.0);
		reference = 0.0;
		in_range = 0.0;
		return;
	}

	vec3 ndc = clip.xyz / clip.w;
	uv = ndc.xy * 0.5 + 0.5;
	float depth01 =
#if CLIP_Z_ZERO_TO_ONE
		ndc.z;
#else
		ndc.z * 0.5 + 0.5;
#endif

	reference = depth01;

	bool inside = all(greaterThanEqual(uv, vec2(0.0))) &&
		all(lessThanEqual(uv, vec2(1.0))) &&
		reference >= 0.0 && reference <= 1.0;
	in_range = inside ? 1.0 : 0.0;
}

void ShadowCoord(vec3 world_pos, out vec2 uv, out float reference, out float in_range)
{
	vec4 clip = ShadowViewProj * vec4(world_pos, 1.0);
	ShadowCoordFromClip(clip, uv, reference, in_range);
}

float ShadowSampleRaw(vec2 uv, float reference, float bias)
{
	float compare_depth = reference - bias;
	return texture(ShadowMap, vec3(uv, compare_depth));
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
	vec2 uv;
	float reference;
	ShadowCoord(world_pos, uv, reference, in_range);
	// ShadowDebug must never disable shadows.
	// Shadow enable is controlled by ShadowParams or CPU-side logic.
	if (in_range < 0.5)
		return 1.0;

	float ndotl = dot(normal, ShadowSunDir.xyz);
	float bias = ShadowParams.x + ShadowParams.y * max(0.0, 1.0 - ndotl);

	if (ShadowParams.z > 0.5)
		return ShadowSamplePCF(uv, reference, bias, int(ShadowParams.w + 0.5));

	return ShadowSampleRaw(uv, reference, bias);
}

vec3 ShadowDebugColor(vec3 world_pos, float shadow_term)
{
	int mode = int(ShadowControl.y + 0.5);
	vec4 lightClip = ShadowViewProj * vec4(world_pos, 1.0);
	vec2 uv;
	float reference;
	float in_range;
	ShadowCoordFromClip(lightClip, uv, reference, in_range);
	bool uv_outside = (in_range < 0.5);
	if (mode == 6)
	{
		float matrix_sum = 0.0;
		matrix_sum += dot(abs(ShadowViewProj[0]), vec4(1.0));
		matrix_sum += dot(abs(ShadowViewProj[1]), vec4(1.0));
		matrix_sum += dot(abs(ShadowViewProj[2]), vec4(1.0));
		matrix_sum += dot(abs(ShadowViewProj[3]), vec4(1.0));
		if (matrix_sum < 0.001)
			return vec3(1.0, 0.0, 1.0);

		vec2 clamped_uv = clamp(uv, vec2(0.0), vec2(1.0));
		float depth = texture(ShadowMapDepth, clamped_uv).r;
		if (depth <= 0.0001 || depth >= 0.9999)
			return vec3(0.0, 1.0, 1.0);

		return vec3(0.0, 1.0, 0.0);
	}
	if (in_range < 0.5)
		return vec3(1.0, 0.0, 0.0);

	if (mode == 1)
	{
		float depth = texture(ShadowMapDepth, uv).r;
		return vec3(depth);
	}
	if (mode == 2)
		return vec3(uv, 0.0);
	if (mode == 3)
	{
		if (uv_outside)
			return vec3(0.0, 0.0, 1.0);
		return vec3(uv, 0.0);
	}
	if (mode == 4)
	{
		if (uv_outside)
			return vec3(0.0, 0.0, 1.0);
		float sampled_depth = texture(ShadowMapDepth, uv).r;
		float diff = abs(reference - sampled_depth);
		return vec3(reference, sampled_depth, diff);
	}

	return vec3(shadow_term);
}
#endif

#ifdef SHADOW_DLIGHT
float ShadowSampleRawDlight(vec2 uv, float reference, float bias)
{
	float compare_depth = reference - bias;
	return texture(ShadowDlightMap, vec3(uv, compare_depth));
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

	vec4 clip = ShadowDlightViewProj[shadow_index] * vec4(world_pos, 1.0);
	if (clip.w <= 0.0)
	{
		in_range = 0.0;
		return 1.0;
	}

	vec3 ndc = clip.xyz / clip.w;
	vec2 base_uv = ndc.xy * 0.5 + 0.5;
	float depth01 =
#if CLIP_Z_ZERO_TO_ONE
		ndc.z;
#else
		ndc.z * 0.5 + 0.5;
#endif

	float reference = depth01;

	vec4 atlas = ShadowDlightAtlas[shadow_index];
	vec2 uv = base_uv;
	uv = uv * atlas.xy + atlas.zw;

	bool inside = all(greaterThanEqual(base_uv, vec2(0.0))) &&
		all(lessThanEqual(base_uv, vec2(1.0))) &&
		reference >= 0.0 && reference <= 1.0;
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
