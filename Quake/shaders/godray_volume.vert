#include "frame_uniforms.glsl"

layout(location=0) in vec3 in_pos;

layout(location=0) out vec3 v_view_pos;

void main()
{
	v_view_pos = (View * vec4(in_pos, 1.0)).xyz;
	gl_Position = ViewProj * vec4(in_pos, 1.0);
}
