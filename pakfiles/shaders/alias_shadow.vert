struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	NormalMatrix[3];
	vec4	LightColor;
	vec4	DLightColor;
	vec4	DLightDir;
	vec4	StaticLightDir;
	/* Keep identical std430 layout to alias.vert/alias.frag because
	 * both regular and shadow passes consume the same CPU instance buffer. */
	float	SkyVisibility;
	vec3	_PadSky;
	int		Pose1;
	int		Pose2;
	float	Blend;
	int		Flags;
};

layout(std430, binding=1) restrict readonly buffer AliasShadowInstanceBlock
{
	InstanceData instances[];
} AliasShadowBuffer;

// BUG FIX: location=0 fuer mat4 belegte Slots 0-3, was mit den vertex attribs
// in_pos(0), in_nor(1), in_uv(2), in_weights(3) im MD5-Pfad kollidierte.
// Verschoben auf location=8 (nach den 5 MD5-Attribs und sicher frei).
layout(location=8) uniform mat4 ShadowViewProj;

struct PoseVertex
{
	vec3 pos;
	vec3 nor;
};

#if MD5
	layout(location=0) in vec3 in_pos;
	layout(location=1) in vec4 in_nor;
	layout(location=2) in vec2 in_uv;
	layout(location=3) in vec4 in_weights;
	layout(location=4) in ivec4 in_indices;

	layout(std430, binding=2) restrict readonly buffer PoseBuffer
	{
		mat3x4 BonePoses[];
	};

	PoseVertex GetPoseVertex(uint pose)
	{
		mat3x4 blendmat = BonePoses[pose + in_indices.x] * in_weights.x;
		blendmat += BonePoses[pose + in_indices.y] * in_weights.y;
		if (in_weights.z + in_weights.w > 0.0)
		{
			blendmat += BonePoses[pose + in_indices.z] * in_weights.z;
			blendmat += BonePoses[pose + in_indices.w] * in_weights.w;
		}
		mat4x3 anim = transpose(blendmat);
		return PoseVertex((anim * vec4(in_pos, 1.0)).xyz, (anim * vec4(in_nor.xyz, 0.0)).xyz);
	}
#else
	layout(location=0) in vec2 in_uv;

	layout(std430, binding=2) restrict readonly buffer BlendShapeBuffer
	{
		uvec2 PackedPosNor[];
	};

	PoseVertex GetPoseVertex(uint pose)
	{
		uvec2 data = PackedPosNor[pose + gl_VertexID];
		return PoseVertex(vec3((data.xxx >> uvec3(0, 8, 16)) & 255), unpackSnorm4x8(data.y).xyz);
	}
#endif

layout(location=0) out vec3 out_world_pos;

void main()
{
	InstanceData inst = AliasShadowBuffer.instances[gl_InstanceID];
	PoseVertex pose1 = GetPoseVertex(uint(inst.Pose1));
	PoseVertex pose2 = GetPoseVertex(uint(inst.Pose2));
	vec3 local_vert = mix(pose1.pos, pose2.pos, inst.Blend);
	mat4x3 worldmatrix = transpose(mat3x4(inst.WorldMatrix[0], inst.WorldMatrix[1], inst.WorldMatrix[2]));
	vec3 world_vert = (worldmatrix * vec4(local_vert, 1.0)).xyz;
	gl_Position = ShadowViewProj * vec4(world_vert, 1.0);
	out_world_pos = world_vert;
}
