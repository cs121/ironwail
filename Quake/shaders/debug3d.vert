#include "frame_uniforms.glsl"

float FogNoise(vec2 worldPos)
{
	return fract(sin(dot(worldPos, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-Fog.w * dot(p, p));
	float noise = FogNoise((p + EyePos).xy);
	fog *= mix(0.8, 1.2, noise);
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}


layout(location=0) in vec3 in_pos;
layout(location=1) in vec4 in_color;

layout(location=0) out vec4 out_color;

void main()
{
	gl_Position = ViewProj * vec4(in_pos, 1.0);
	out_color = in_color;
}
