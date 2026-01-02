#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-Fog.w * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}

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

#define DITHER_NOISE(uv) (bayer01(ivec2(uv)) - 0.5)
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

layout(binding=0) uniform sampler2D Tex;

layout(location=0) in vec2 in_uv;
layout(location=1) in vec4 in_color;
layout(location=2) in vec3 in_pos;

layout(location=0) out vec4 out_fragcolor;
layout(location=1) out vec4 out_velocity;

void main()
{
	vec4 result = texture(Tex, in_uv) * in_color;
	if (result.a <= 0.001)
		discard;

	result.rgb = ApplyFog(result.rgb, in_pos);
	out_fragcolor = result;
	out_velocity = vec4(0.0, 0.0, 0.0, 2.0);
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
