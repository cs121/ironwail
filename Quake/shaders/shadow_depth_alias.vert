struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	LightColor;
	vec4	DLightColor;
	vec4	ShadowLightPosRange;
	uint	ShadowLightIndex;
	uint	_PadShadow1;
	uint	_PadShadow2;
	uint	_PadShadow3;
	int		Pose1;
	int		Pose2;
	float	Blend;
	int		Flags;
};

layout(std430, binding=1) restrict readonly buffer AliasFrameBlock
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
	vec4	ShadowDebug;
	mat4	ShadowDlightViewProj[4];
	vec4	ShadowDlightAtlas[4];
	vec4	ShadowDlightInfo[4];
	vec4	ShadowDlightParams;
	InstanceData instances[];
} AliasFrameBuffer;

struct PoseVertex
{
	vec3 pos;
};

#if MD5
	layout(location=0) in vec3 in_pos;
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
		PoseVertex pv;
		pv.pos = (anim * vec4(in_pos, 1.0)).xyz;
		return pv;
	}
#else
	layout(std430, binding=2) restrict readonly buffer BlendShapeBuffer
	{
		uvec2 PackedPosNor[];
	};

	PoseVertex GetPoseVertex(uint pose)
	{
		uvec2 data = PackedPosNor[pose + gl_VertexID];
		PoseVertex pv;
		pv.pos = vec3((data.xxx >> uvec3(0, 8, 16)) & 255);
		return pv;
	}
#endif

void main()
{
	InstanceData inst = AliasFrameBuffer.instances[gl_InstanceID];
	PoseVertex pose1 = GetPoseVertex(inst.Pose1);
	PoseVertex pose2 = GetPoseVertex(inst.Pose2);
	vec3 local_vert = mix(pose1.pos, pose2.pos, inst.Blend);
	mat4x3 worldmatrix = transpose(mat3x4(inst.WorldMatrix[0], inst.WorldMatrix[1], inst.WorldMatrix[2]));
	vec3 world_vert = (worldmatrix * vec4(local_vert, 1.0)).xyz;
	gl_Position = AliasFrameBuffer.ShadowViewProj * vec4(world_vert, 1.0);
}
