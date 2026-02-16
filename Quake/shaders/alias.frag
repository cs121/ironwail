#include "frame_uniforms.glsl"

struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	LightColor; // xyz=LightColor w=Alpha
	vec4	DLightColor; // xyz=DLightColor
	vec4	AmbientColor; // xyz=AmbientColor
	vec4	EnvMapParams; // x=enable y=glossMask z=indoorHint w=intensity
	int		Pose1;
	int		Pose2;
	float	Blend;
	int		Flags;
};

layout(std430, binding=1) restrict readonly buffer InstanceBuffer
{
	mat4	AliasViewProj;
	mat4	AliasPrevViewProj;
	vec3	AliasEyePos;
	float	_AliasPad0;
	vec4	AliasFog;
	float	AliasScreenDither;
	float	AliasOverbright;
	float	ModelHalfLambert;
	float	RimViewmodelScale;
	vec4	RimParams0;
	vec4	RimParams1;
	vec4	RimParams2;
	mat4	AliasShadowViewProj;
	vec4	AliasShadowParams;
	vec4	AliasShadowDebug;
	vec4	AliasShadowSunDir;
	InstanceData instances[];
};
// ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));	// 0  0  0  0 | x3 x2 x1 x0 |  0  0  0  0 | y3 y2 y1 y0
	v = (v ^ (v << 2)) & 0x3333;				// 0  0 x3 x2 |  0  0 x1 x0 |  0  0 y3 y2 |  0  0 y1 y0
	v = (v ^ (v << 1)) & 0x5555;				// 0 x3  0 x2 |  0 x1  0 x0 |  0 y3  0 y2 |  0 y1  0 y0
	v |= v >> 7;								// 0 x3  0 x2 |  0 x1  0 x0 | x3 y3 x2 y2 | x1 y1 x0 y0
	v = bitfieldReverse(v) >> 24;				// 0  0  0  0 |  0  0  0  0 | y0 x0 y1 x1 | y2 x2 y3 x3
	return float(v) * (1.0/256.0);
}

float bayer(ivec2 coord)
{
        return bayer01(coord) - 0.5;
}

vec3 ApplyFog(vec3 clr, vec3 p)
{
        float fog = exp2(-abs(AliasFog.w) * dot(p, p));
        fog = clamp(fog, 0.0, 1.0);
        return mix(AliasFog.rgb, clr, fog);
}

// Hash without Sine
// https://www.shadertoy.com/view/4djSRW 
float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * .1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float whitenoise(vec2 p)
{
	return whitenoise01(p) - 0.5;
}

// Convert uniform distribution to triangle-shaped distribution
// Input in [0..1], output in [-1..1]
// Based on https://www.shadertoy.com/view/4t2SDh 
float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

vec2 ComputeVelocity(vec4 curr_clip, vec4 prev_clip)
{
	const float EPS = 1e-6;
	float inv_curr_w = abs(curr_clip.w) > EPS ? 1.0 / curr_clip.w : 0.0;
	float inv_prev_w = abs(prev_clip.w) > EPS ? 1.0 / prev_clip.w : 0.0;
	vec2 curr_ndc = curr_clip.xy * inv_curr_w;
	vec2 prev_ndc = prev_clip.xy * inv_prev_w;
	return (curr_ndc - prev_ndc) * 0.5;
}

const int ALIAS_FLAG_NO_MOTION_BLUR = 1;
const int ALIAS_FLAG_VIEWMODEL = 2;
const int ALIAS_FLAG_LIGHTNING = 4;

layout(binding=0) uniform sampler2D Tex;
layout(binding=1) uniform sampler2D FullbrightTex;
layout(binding=2) uniform sampler2D EmissiveTex;
layout(binding=5) uniform sampler2D ShadowMap;
layout(binding=6) uniform samplerCube ReflectionTex;

#include "envlight.glsl"
#define SHADOW_SUN 1
#include "shadow_sample.glsl"

#if MODE == 2
	layout(location=0) noperspective in vec2 in_texcoord;
#else
	layout(location=0) in vec2 in_texcoord;
#endif
layout(location=1) in vec4 in_color;
layout(location=2) in vec3 in_pos;
layout(location=3) noperspective in vec4 in_curr_clip;
layout(location=4) noperspective in vec4 in_prev_clip;
layout(location=5) flat in int in_flags;
layout(location=6) in vec3 in_normal;
// Per-instance lighting inputs are linear RGB intensities.
// Do not apply per-material gamma here; final transfer is handled globally
// by postprocess / framebuffer-sRGB selection.
layout(location=7) in vec3 in_static_light;
layout(location=8) in vec3 in_dyn_light;
layout(location=9) in vec3 in_amb_light;
layout(location=10) flat in vec4 in_env_params;

#define OUT_COLOR out_fragcolor
#if OIT
	vec4 OUT_COLOR;
	layout(location=0) out vec4 out_accum;
	layout(location=1) out float out_reveal;

	vec3 GammaToLinear(vec3 v)
	{
#if 0
		return v*v;
#else
		return v;
#endif
	}

	void main_body();

	void main()
	{
		main_body();
		OUT_COLOR = clamp(OUT_COLOR, 0.0, 1.0);
		vec4 color = vec4(GammaToLinear(OUT_COLOR.rgb), OUT_COLOR.a);
		float z = 1./gl_FragCoord.w;
#if 0
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/2e5, 2.0)), 1e-2, 3e3);
#else
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);
#endif
		out_accum = vec4(color.rgb, color.a * weight);
		out_accum.rgb *= out_accum.a;
		out_reveal = color.a;
	}

	#define main main_body
#else
        layout(location=0) out vec4 OUT_COLOR;
        layout(location=1) out vec4 out_velocity;
#endif // OIT


float saturate(float x)
{
	return clamp(x, 0.0, 1.0);
}

float luminance(vec3 rgb)
{
	return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

vec3 normalize_safe(vec3 v)
{
	float len2 = dot(v, v);
	if (len2 < 1e-12)
		return vec3(0.0);
	return v * inversesqrt(len2);
}

void main()
{
        vec2 uv = in_texcoord;
        vec3 emissive = vec3(0.0);
        float shadow_range = 1.0;
        float shadow_term = 1.0;
	vec4 lit_color = in_color;
	vec3 L_static = max(in_static_light, vec3(0.0));
	vec3 L_dyn = max(in_dyn_light, vec3(0.0));
	vec3 L_amb = max(in_amb_light, vec3(0.0));

	if (ShadowDebug.x > 0.5 && (in_flags & ALIAS_FLAG_VIEWMODEL) == 0)
	{
		vec3 world_pos = in_pos + EyePos;
		vec3 shadow_normal = gl_FrontFacing ? in_normal : -in_normal;
		shadow_term = ShadowVisibility(world_pos, shadow_normal, shadow_range);
		if (ShadowDebug.y > 1.5)
		{
			float debug_value = (ShadowDebug.y > 2.5) ? shadow_range : shadow_term;
			out_fragcolor = vec4(vec3(debug_value), 1.0);
#if !OIT
			out_velocity = vec4(0.0);
#endif
			return;
		}
		lit_color.rgb *= shadow_term;
	}
#if MODE == 2
        uv -= 0.5 / vec2(textureSize(Tex, 0).xy);
        vec4 result = textureLod(Tex, uv, 0.);
#else
        vec4 result = texture(Tex, uv);
#endif
#if ALPHATEST
	if (result.a < 0.666)
		discard;
	result.rgb *= lit_color.rgb;
#else
	result.rgb = mix(result.rgb, result.rgb * lit_color.rgb, result.a);
#endif
	result.a = lit_color.a; // FIXME: This will make almost transparent things cut holes though heavy fog
        vec3 fullbright;
#if MODE == 2
        fullbright = textureLod(FullbrightTex, uv, 0.).rgb;
        emissive = textureLod(EmissiveTex, uv, 0.).rgb;
#else
        fullbright = texture(FullbrightTex, uv).rgb;
        emissive = texture(EmissiveTex, uv).rgb;
#endif
        result.rgb += fullbright;
        result.rgb += emissive;

        if ((in_flags & ALIAS_FLAG_LIGHTNING) != 0)
        {
                float d = clamp(length(in_texcoord - 0.5) * 2.0, 0.0, 1.0);
                float ghost = pow(1.0 - d, 3.0) * 0.2;
                result.rgb += ghost * vec3(0.5, 0.7, 1.3);
        }

	vec3 N_env = normalize_safe(gl_FrontFacing ? in_normal : -in_normal);
	vec3 V_env = normalize_safe(-in_pos);
	float ambient_luma = EnvLightLuma(clamp(L_amb / max(AliasOverbright, 1e-4), 0.0, 1.0));
	float indoor_factor = DeriveIndoorFactor(ambient_luma, in_env_params.z, shadow_term);
	float env_mask = max(in_env_params.x, 0.0);
	vec3 env_spec = EvaluateReflectionProbe(
		ReflectionTex,
		1.0,
		in_pos + EyePos,
		N_env,
		V_env,
		in_env_params.y * env_mask,
		indoor_factor,
		in_env_params.w * env_mask);
	result.rgb += env_spec;

	if (RimParams0.x > 0.5)
	{
		vec3 N = normalize_safe(gl_FrontFacing ? in_normal : -in_normal);
		vec3 V = normalize_safe(-in_pos);
		vec3 L = normalize_safe(-ShadowSunDir.xyz);
		// Keep shading normals oriented with the geometric face normal so inconsistent
		// asset normals don't force a full rim contribution on front-facing polygons.
		vec3 Ng = normalize_safe(cross(dFdx(in_pos), dFdy(in_pos)));
		if (!gl_FrontFacing)
			Ng = -Ng;
		if (dot(N, Ng) < 0.0)
			N = -N;
		float ndv = saturate(dot(N, V));
		float rim_raw = pow(saturate(1.0 - ndv), RimParams0.z) * RimParams0.y;
		float ndotl = saturate(dot(N, L));
		float gate = saturate((1.0 - ndotl) * RimParams1.z + RimParams1.w);

		vec3 direct_static = RimParams0.w * L_static * shadow_term;
		vec3 direct_dyn = RimParams1.x * L_dyn;
		vec3 direct_rgb = direct_static + direct_dyn;
		vec3 ambient_rgb = L_amb;
		float rim = rim_raw * gate;
		if ((in_flags & ALIAS_FLAG_VIEWMODEL) != 0)
			rim *= RimViewmodelScale;

		vec3 rim_light_preclamp = rim * (direct_rgb + RimParams1.y * ambient_rgb) * RimParams2.x;
		vec3 local_limit_rgb = (RimParams2.y * direct_rgb) + (RimParams2.z * ambient_rgb);
		vec3 rim_light = min(rim_light_preclamp, local_limit_rgb);

		result.rgb += rim_light;

		if (RimParams2.w > 0.5)
		{
			if (RimParams2.w < 1.5)
				result.rgb = vec3(rim);
			else if (RimParams2.w < 2.5)
				result.rgb = vec3(ndv);
			else
				result.rgb = vec3(ndotl);
			result.a = 1.0;
		}
	}

        result.rgb = clamp(result.rgb, 0.0, 1.0);

        result.rgb = ApplyFog(result.rgb, in_pos);
        out_fragcolor = result;
#if !OIT
        vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
        float viewModelMask = ((in_flags & ALIAS_FLAG_NO_MOTION_BLUR) != 0) ? 1.0 : 0.0;
        vec2 velocityOut = vec2(0.0);
        if (viewModelMask < 0.5 && result.a >= 0.999)
                velocityOut = velocity * result.a;
        out_velocity = vec4(velocityOut, viewModelMask, 1.0);
#endif
#if MODE == 1 || MODE == 2
	// Note: sign bit is used as overbright flag
	if (abs(AliasFog.w) > 0.)
	{
		out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
		out_fragcolor.rgb += SCREEN_SPACE_NOISE() * AliasScreenDither;
		out_fragcolor.rgb *= out_fragcolor.rgb;
	}
#else
	out_fragcolor.rgb += SUPPRESS_BANDING() * AliasScreenDither;
#endif
}
