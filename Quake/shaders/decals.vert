#include "frame_uniforms.glsl"

layout(location=0) in vec3 in_pos;
layout(location=1) in vec2 in_uv;
layout(location=2) in vec3 in_color;
layout(location=3) in vec2 in_params;

layout(location=0) out vec2 out_uv;
layout(location=1) out vec3 out_pos;
layout(location=2) out vec3 out_color;
layout(location=3) out vec2 out_params;

void main()
{
	gl_Position = ViewProj * vec4(in_pos, 1.0);
	out_pos = in_pos - EyePos;
	out_uv = in_uv;
	out_color = in_color;
	out_params = in_params;
}
