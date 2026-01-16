#include "frame_uniforms.glsl"

layout(location=0) in vec3 in_pos;

void main()
{
	gl_Position = ViewProj * vec4(in_pos, 1.0);
}
