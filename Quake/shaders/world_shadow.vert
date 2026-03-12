#if BINDLESS
#extension GL_ARB_shader_draw_parameters : require
#define DRAW_ID gl_DrawIDARB
#else
layout(location=0) uniform int DrawID;
#define DRAW_ID DrawID
#endif

// BUG FIX: location=0 kollidierte mit dem vertex attrib in_pos (location=0).
// ShadowViewProj belegt locations 0-3 (mat4 = 4 konsekutive slots) — in_pos war
// dadurch undefiniert. Verschoben auf location=4 (erste freie Slot nach mat4).
layout(location=4) uniform mat4 ShadowViewProj;

struct Call
{
	uint flags;
	uint tcgen;
	float wateralpha;
	float _pad0;
	vec2 polygon_offset;
	vec4 stage_color;
#if BINDLESS
	uvec2 txhandle;
	uvec2 fbhandle;
	uvec2 emhandle;
#else
	int baseinstance;
	int padding;
#endif
};

const uint CF_USE_POLYGON_OFFSET = 1u;

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
	vec4 mat[3];
	vec4 prev_mat[3];
	float alpha;
	float pad0;
	float pad1;
	float pad2;
};

layout(std430, binding=2) restrict readonly buffer InstanceBuffer
{
	Instance instance_data[];
};

layout(location=0) in vec3 in_pos;
layout(location=0) out vec3 out_world_pos;  // location=0 als Output ist korrekt (anderes Interface)

void main()
{
	Call call = call_data[DRAW_ID];
	int instance_id = GET_INSTANCE_ID(call);
	Instance instance = instance_data[instance_id];
	mat4x3 world = transpose(mat3x4(instance.mat[0], instance.mat[1], instance.mat[2]));
	vec3 world_pos = (world * vec4(in_pos, 1.0)).xyz;
	vec4 clip = ShadowViewProj * vec4(world_pos, 1.0);
	/* Shadow-map depth is rendered in classic non-reversed space. */
	const float ZBIAS = 1.0 / 1024.0;
	if ((call.flags & CF_USE_POLYGON_OFFSET) != 0u)
	{
		float zoffset = (call.polygon_offset.x + call.polygon_offset.y) * ZBIAS;
		clip.z += zoffset;
	}
	gl_Position = clip;
	out_world_pos = world_pos;
}
