#ifndef SHADOW_SAMPLE_GLSL
#define SHADOW_SAMPLE_GLSL

float ShadowCompare(float depth, float reference, float bias)
{
#if REVERSED_Z
	return (reference >= (depth - bias)) ? 1.0 : 0.0;
#else
	return (reference <= (depth + bias)) ? 1.0 : 0.0;
#endif
}

float ShadowSampleRaw(vec2 uv, float reference, float bias)
{
	float depth = texture(ShadowMap, uv).r;
	return ShadowCompare(depth, reference, bias);
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

	vec4 clip = ShadowViewProj * vec4(world_pos, 1.0);
	if (clip.w <= 0.0)
	{
		in_range = 0.0;
		return 1.0;
	}

	vec3 proj = clip.xyz / clip.w;
	vec2 uv = proj.xy * 0.5 + 0.5;
#if REVERSED_Z
	float reference = proj.z;
#else
	float reference = proj.z * 0.5 + 0.5;
#endif

	bool inside = all(greaterThanEqual(vec3(uv, reference), vec3(0.0))) &&
		all(lessThanEqual(vec3(uv, reference), vec3(1.0)));
	in_range = inside ? 1.0 : 0.0;
	if (!inside)
		return 1.0;

	float ndotl = clamp(dot(normal, -ShadowSunDir.xyz), 0.0, 1.0);
	float bias = ShadowParams.x + ShadowParams.y * (1.0 - ndotl);

	if (ShadowParams.z > 0.5)
		return ShadowSamplePCF(uv, reference, bias, int(ShadowParams.w + 0.5));

	return ShadowSampleRaw(uv, reference, bias);
}

#endif
