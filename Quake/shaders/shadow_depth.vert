#if BINDLESS
	#extension GL_ARB_shader_draw_parameters : require
	#define DRAW_ID gl_DrawIDARB
#else
layout(location=0) uniform int DrawID;
	#define DRAW_ID DrawID
#endif

#include "frame_uniforms.glsl"

struct Call
{
	uint	flags;
	uint	tcgen;
	float	wateralpha;
	float	_pad0;
	vec2	polygon_offset;
	vec4	stage_color;
#if BINDLESS
	uvec2	txhandle;
	uvec2	fbhandle;
	uvec2	emhandle;
#else
	int		baseinstance;
	int		padding;
#endif
};

layout(std430, binding=1) restrict readonly buffer CallBuffer
{
	Call call_data[];
};

#if BINDLESS
	#define GET_INSTANCE_ID(call) (gl_BaseInstanceARB + gl_InstanceID)
#else
	#define GET_INSTANCE_ID(call) (call.baseinstance + gl_InstanceID)
#endif

struct Instance
{
	vec4	mat[3];
	vec4	prev_mat[3];
	float	alpha;
	float	pad0;
	float	pad1;
	float	pad2;
};

layout(std430, binding=2) restrict readonly buffer InstanceBuffer
{
	Instance instance_data[];
};

layout(location=0) in vec3 in_pos;

void main()
{
	Call call = call_data[DRAW_ID];
	Instance inst = instance_data[GET_INSTANCE_ID(call)];
	mat4x3 world_matrix = transpose(mat3x4(inst.mat[0], inst.mat[1], inst.mat[2]));
	vec3 world_pos = (world_matrix * vec4(in_pos, 1.0)).xyz;
	gl_Position = ShadowViewProj * vec4(world_pos, 1.0);
}
