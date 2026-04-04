#include "frame_uniforms.glsl"

struct DecalVertex
{
	vec3 pos;
	float u;
	float v;
	uint color;
	uint pad0;
	uint pad1;
};

struct DecalInstance
{
	uint first_vert;
	uint num_verts;
	float fade_alpha;
	uint light_rgba;
	vec4 atlas;
};

layout(std430, binding=1) restrict readonly buffer DecalVertexBlock
{
	DecalVertex verts[];
};

layout(std430, binding=2) restrict readonly buffer DecalInstanceBlock
{
	DecalInstance insts[];
};

layout(location=0) uniform int InstanceBase;

layout(location=0) out vec2 out_uv;
layout(location=1) out vec4 out_color;

void main()
{
	uint inst_index = uint(max(0, InstanceBase) + gl_InstanceID);
	DecalInstance inst = insts[inst_index];
	uint tri = uint(gl_VertexID / 3);
	uint corner = uint(gl_VertexID % 3);
	uint local_vert = 0u;
	bool valid = inst.num_verts >= 3u && tri < (inst.num_verts - 2u);

	if (valid)
	{
		if (corner == 0u)
			local_vert = 0u;
		else if (corner == 1u)
			local_vert = tri + 1u;
		else
			local_vert = tri + 2u;
	}

	DecalVertex v = verts[inst.first_vert + local_vert];
	vec2 base_uv = vec2(v.u, v.v);
	out_uv = inst.atlas.xy + base_uv * (inst.atlas.zw - inst.atlas.xy);
	out_color = unpackUnorm4x8(v.color);
	out_color.rgb *= unpackUnorm4x8(inst.light_rgba).rgb;
	out_color.a *= inst.fade_alpha;

	if (!valid || out_color.a <= 0.0)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		out_uv = vec2(0.0, 0.0);
		out_color = vec4(0.0);
		return;
	}

	gl_Position = ViewProj * vec4(v.pos, 1.0);
}
