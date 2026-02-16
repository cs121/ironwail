#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-Fog.w * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}

float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));
	v = (v ^ (v << 2)) & 0x3333;
	v = (v ^ (v << 1)) & 0x5555;
	v |= v >> 7;
	v = bitfieldReverse(v) >> 24;
	return float(v) * (1.0/256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

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

layout(binding=0) uniform sampler2D Tex;
layout(location=0) uniform int u_debug_mode = 0;

layout(location=0) in vec2 in_uv;
layout(location=1) in vec3 in_pos;
layout(location=2) in vec3 in_color;
layout(location=3) in vec2 in_params;

layout(location=0) out vec4 out_fragcolor;
layout(location=1) out vec4 out_velocity;

void main()
{
	vec4 texel = texture(Tex, in_uv);
	if (texel.a < (1.0 / 255.0))
		discard;

	vec3 lit = texel.rgb * in_color.rgb;
	float alpha = texel.a * in_params.x;
	float emissive = max(0.0, in_params.y);
	vec3 result = lit + texel.rgb * emissive;

	if (u_debug_mode != 0)
	{
		bool overshoot = any(greaterThan(result, vec3(1.0)));
		bool emissive_path = emissive > 0.0;
		vec3 debug_color = vec3(0.0);
		if (overshoot)
			debug_color.r = 1.0;
		if (emissive_path)
			debug_color.g = 1.0;
		if (!emissive_path)
			debug_color.b = 1.0;
		out_fragcolor = vec4(debug_color, alpha);
	}
	else
	{
		result = ApplyFog(result, in_pos);
		out_fragcolor = vec4(result, alpha);
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
	out_velocity = vec4(0.0, 0.0, 0.0, 2.0);
}
