#if BINDLESS
        #extension GL_ARB_bindless_texture : require
#else
        layout(binding=0) uniform sampler2D Tex;
        layout(binding=1) uniform sampler2D FullbrightTex;
        layout(binding=4) uniform sampler2D EmissiveTex;
#endif

#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p, vec4 fogData)
{
        float fog = exp2(-fogData.w * dot(p, p));
        fog = clamp(fog, 0.0, 1.0);
        return mix(fogData.rgb, clr, fog);
}

const uint
        CF_USE_POLYGON_OFFSET = 1u,
        CF_USE_FULLBRIGHT = 2u,
        CF_NOLIGHTMAP = 4u,
        CF_USE_EMISSIVE = 8u,
        CF_ALPHA_TEST = 16u,
        CF_TC_STRETCH = 32u,
        CF_TC_TURB = 64u,
        CF_TC_ENVMAP = 128u,
        CF_CUSTOM_FOG = 256u
;

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

layout(location=0) flat in uint in_flags;
layout(location=1) flat in float in_alpha;
layout(location=2) in vec2 in_uv;
layout(location=3) in vec3 in_pos;
#if BINDLESS
        layout(location=4) flat in uvec4 in_samplers0;
        layout(location=5) flat in uvec2 in_samplers1;
#endif
layout(location=6) noperspective in vec4 in_curr_clip;
layout(location=7) noperspective in vec4 in_prev_clip;
layout(location=8) in vec2 in_emissive_uv;
layout(location=9) flat in vec3 in_emissive_color;
layout(location=10) flat in vec4 in_stage_params1;
layout(location=11) flat in vec4 in_fog_color;

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

void main()
{
        vec3 fullbright = vec3(0.0);
        vec3 emissive = vec3(0.0);
        vec2 uv = in_uv * 2.0 + 0.125 * sin(in_uv.yx * (3.14159265 * 2.0) + Time);
#if BINDLESS
        sampler2D Tex = sampler2D(in_samplers0.xy);
        if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
        {
                sampler2D FullbrightTex = sampler2D(in_samplers0.zw);
                fullbright = texture(FullbrightTex, uv).rgb;
        }
        if ((in_flags & CF_USE_EMISSIVE) != 0u)
        {
                sampler2D EmissiveSampler = sampler2D(in_samplers1.xy);
                emissive = texture(EmissiveSampler, in_emissive_uv).rgb * in_emissive_color;
        }
#else
        if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
                fullbright = texture(FullbrightTex, uv).rgb;
        if ((in_flags & CF_USE_EMISSIVE) != 0u)
                emissive = texture(EmissiveTex, in_emissive_uv).rgb * in_emissive_color;
#endif
        vec4 result = texture(Tex, uv);
        float maskIndex = in_stage_params1.z;
        if (maskIndex >= 0.0)
        {
                int maskChannel = int(maskIndex + 0.5);
                if (maskChannel == 0)
                {
                        result.g = 0.0;
                        result.b = 0.0;
                }
                else if (maskChannel == 1)
                {
                        result.r = 0.0;
                        result.b = 0.0;
                }
                else if (maskChannel == 2)
                {
                        result.r = 0.0;
                        result.g = 0.0;
                }
                else if (maskChannel == 3)
                {
                        result.rgb = vec3(1.0);
                        result.a = clamp(result.a, 0.0, 1.0);
                }
        }
        result.rgb += fullbright;
        result.rgb += emissive;

        if ((in_flags & CF_TC_ENVMAP) != 0u)
        {
                vec3 dpdx = dFdx(in_pos);
                vec3 dpdy = dFdy(in_pos);
                vec3 normal = cross(dpdx, dpdy);
                float normal_len2 = dot(normal, normal);
                if (normal_len2 <= 1.0e-8)
                        normal = vec3(0.0, 0.0, 1.0);
                else
                        normal *= inversesqrt(normal_len2);

                vec3 view_vec = -in_pos;
                float view_len2 = dot(view_vec, view_vec);
                vec3 view_dir = view_len2 > 0.0 ? view_vec * inversesqrt(view_len2) : vec3(0.0, 0.0, 1.0);
                vec3 reflect_dir = reflect(-view_dir, normal);
                vec2 env_uv = reflect_dir.xy * 0.5 + 0.5;
#if DITHER >= 2
                vec3 env_color = texture(Tex, env_uv, -1.0).rgb;
#elif DITHER
                vec3 env_color = texture(Tex, env_uv, -0.5).rgb;
#else
                vec3 env_color = texture(Tex, env_uv).rgb;
#endif
                result.rgb = clamp(result.rgb + env_color, 0.0, 1.0);
        }

        result.rgb = clamp(result.rgb, 0.0, 1.0);
        vec4 fogData = ((in_flags & CF_CUSTOM_FOG) != 0u) ? in_fog_color : Fog;
        result.rgb = ApplyFog(result.rgb, in_pos, fogData);
        result.a *= in_alpha;
        out_fragcolor = result;
#if !OIT
        vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
        vec2 velocityOut = vec2(0.0);
        if (result.a >= 0.999)
                velocityOut = velocity * result.a;
        out_velocity = vec4(velocityOut, 0.0, 0.0);
#endif
#if DITHER
	if (Fog.w > 0.)
	{
		out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
		out_fragcolor.rgb += SCREEN_SPACE_NOISE() * ScreenDither;
		out_fragcolor.rgb *= out_fragcolor.rgb;
	}
#else
	out_fragcolor.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}
