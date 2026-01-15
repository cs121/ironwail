#include "frame_uniforms.glsl"

layout(location=0) in vec3 in_pos;

layout(location=0) out vec3 v_world_pos;

void main()
{
	v_world_pos = in_pos;
	gl_Position = ViewProj * vec4(in_pos, 1.0);
}
