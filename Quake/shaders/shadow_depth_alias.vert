struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	LightColor; // xyz=LightColor w=Alpha
	vec4	DLightColor; // xyz=DLightColor
	int		Pose1;
	int		Pose2;
	float	Blend;
	int		Flags;
};

layout(std430, binding=1) restrict readonly buffer InstanceBuffer
{
	mat4	ViewProj;
	mat4	PrevViewProj;
	vec3	EyePos;
	float	_Pad0;
	vec4	Fog;
	float	ScreenDither;
	float	Overbright;
	float	ModelHalfLambert;
	float	_Pad1;
	mat4	ShadowViewProj;
	vec4	ShadowParams;
	vec4	ShadowControl;
	vec4	ShadowDebug;
	vec4	ShadowSunDir;
	InstanceData instances[];
};

struct PoseVertex
{
	vec3 pos;
	vec3 nor;
};

layout(location=0) out vec4 v_light_clip;

#if MD5
	layout(location=0) in vec3 in_pos;
	layout(location=1) in vec4 in_weights;
	layout(location=2) in ivec4 in_indices;

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
		return PoseVertex((anim * vec4(in_pos, 1.0)).xyz, vec3(0.0));
	}

#else
	layout(std430, binding=2) restrict readonly buffer BlendShapeBuffer
	{
		uvec2 PackedPosNor[];
	};

	PoseVertex GetPoseVertex(uint pose)
	{
		uvec2 data = PackedPosNor[pose + gl_VertexID];
		return PoseVertex(vec3((data.xxx >> uvec3(0, 8, 16)) & 255), unpackSnorm4x8(data.y).xyz);
	}

#endif // MD5

void main()
{
	InstanceData inst = instances[gl_InstanceID];
	PoseVertex pose1 = GetPoseVertex(inst.Pose1);
	PoseVertex pose2 = GetPoseVertex(inst.Pose2);
	vec3 alias_pos = mix(pose1.pos, pose2.pos, inst.Blend);
	mat4 model = mat4(
		vec4(inst.WorldMatrix[0].xyz, 0.0),
		vec4(inst.WorldMatrix[1].xyz, 0.0),
		vec4(inst.WorldMatrix[2].xyz, 0.0),
		vec4(inst.WorldMatrix[0].w, inst.WorldMatrix[1].w, inst.WorldMatrix[2].w, 1.0)
	);
	vec3 world_pos = (model * vec4(alias_pos, 1.0)).xyz;
	v_light_clip = ShadowViewProj * vec4(world_pos, 1.0);
	gl_Position = v_light_clip;
}
