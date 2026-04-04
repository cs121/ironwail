#include "frame_uniforms.glsl"

// Fog.w is treated as a signed-friendly density; use abs(Fog.w) so negative CPU values do not invert attenuation.
vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-abs(Fog.w) * dot(p, p));
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
