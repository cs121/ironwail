#ifndef SHADOW_SAMPLE_GLSL
#define SHADOW_SAMPLE_GLSL

#ifndef SHADOW_VIEWPROJ
#define SHADOW_VIEWPROJ ShadowViewProj
#endif

#ifndef SHADOW_PARAMS
#define SHADOW_PARAMS ShadowParams
#endif

#ifndef SHADOW_DEBUG
#define SHADOW_DEBUG ShadowDebug
#endif

#ifndef SHADOW_SUN_DIR
#define SHADOW_SUN_DIR ShadowSunDir
#endif

// -----------------------------------------------------------------------------
// Matrix multiplication robustness
//
// If the engine uploads row-major matrices to GLSL with transpose=GL_FALSE,
// then (mat4 * vec4) will behave like a transposed matrix and shadow
// projection turns into "screen-space" diagonal banding.
//
// To make shader-side testing robust (without touching engine code), we compute
// BOTH variants and pick the one that looks more plausible.
// This is cheap compared to the ray/pcf work and dramatically reduces the odds
// that a row/column-major mismatch breaks shadows.
// -----------------------------------------------------------------------------

vec4 ShadowMul(mat4 M, vec4 v)
{
    vec4 a = M * v; // GLSL default (column-major expectation)
    vec4 b = v * M; // "row-major" style

    // Prefer a result with a sane perspective divide and reasonable NDC range.
    // We don't require it to be inside [-1,1] (cascades/atlases can overscan),
    // but we reject extreme values that typically come from a bad transpose.
    bool a_ok = (abs(a.w) > 1e-6);
    bool b_ok = (abs(b.w) > 1e-6);
    if (a_ok)
    {
        vec3 pa = a.xyz / a.w;
        a_ok = all(lessThanEqual(abs(pa), vec3(20.0)));
    }
    if (b_ok)
    {
        vec3 pb = b.xyz / b.w;
        b_ok = all(lessThanEqual(abs(pb), vec3(20.0)));
    }

    if (a_ok && !b_ok) return a;
    if (b_ok && !a_ok) return b;

    // If both look OK (or both look bad), prefer the one with a larger |w|
    // (more numerically stable divide).
    return (abs(a.w) >= abs(b.w)) ? a : b;
}

float ShadowReference01(float proj_z)
{
    // Robustly convert proj.z into [0..1].
    // If the engine already uses clip-control 0..1, proj_z will already be in-range.
    // If it's OpenGL NDC (-1..1), this remaps.
    float ref = proj_z;
    if (ref < 0.0 || ref > 1.0)
        ref = ref * 0.5 + 0.5;
    return clamp(ref, 0.0, 1.0);
}

#ifdef SHADOW_SUN
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
	if (SHADOW_DEBUG.x < 0.5)
	{
		in_range = 1.0;
		return 1.0;
	}

	vec4 clip = ShadowMul(SHADOW_VIEWPROJ, vec4(world_pos, 1.0));
	if (clip.w <= 0.0)
	{
		in_range = 0.0;
		return 1.0;
	}

	vec3 proj = clip.xyz / clip.w;
	vec2 uv = proj.xy * 0.5 + 0.5;
	float reference = ShadowReference01(proj.z);

	bool inside = all(greaterThanEqual(vec3(uv, reference), vec3(0.0))) &&
		all(lessThanEqual(vec3(uv, reference), vec3(1.0)));
	in_range = inside ? 1.0 : 0.0;
	if (!inside)
		return 1.0;

	float ndotl = clamp(dot(normal, -SHADOW_SUN_DIR.xyz), 0.0, 1.0);
	float bias = SHADOW_PARAMS.x + SHADOW_PARAMS.y * (1.0 - ndotl);

	if (SHADOW_PARAMS.z > 0.5)
		return ShadowSamplePCF(uv, reference, bias, int(SHADOW_PARAMS.w + 0.5));

	return ShadowSampleRaw(uv, reference, bias);
}
#endif

#ifdef SHADOW_DLIGHT
float ShadowCompare(float depth, float reference, float bias)
{
#if REVERSED_Z
	return (reference >= (depth - bias)) ? 1.0 : 0.0;
#else
	return (reference <= (depth + bias)) ? 1.0 : 0.0;
#endif
}

float ShadowSampleRawDlight(vec2 uv, float reference, float bias)
{
	float depth = texture(ShadowDlightMap, uv).r;
	return ShadowCompare(depth, reference, bias);
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
