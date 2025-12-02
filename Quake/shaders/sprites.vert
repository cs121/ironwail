#include "frame_uniforms.glsl"

const float FOG_NOISE_SCALE = 0.02;

float FogNoise(vec2 worldPos)
{
	vec2 p = worldPos * FOG_NOISE_SCALE;
	float n = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
	return smoothstep(0.25, 0.75, n);
}

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-Fog.w * dot(p, p));
	float noise = FogNoise((p + EyePos).xy);
	fog *= mix(0.9, 1.1, noise);
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}


layout(location=0) in vec3 in_pos;
layout(location=1) in vec2 in_uv;

layout(location=0) out vec2 out_uv;
layout(location=1) out vec3 out_pos;

void main()
{
	gl_Position = ViewProj * vec4(in_pos, 1.0);
	out_pos = in_pos - EyePos;
	out_uv = in_uv;
}
