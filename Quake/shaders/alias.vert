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
	float	_Pad[5];
	InstanceData instances[];
};

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
		return PoseVertex((anim * vec4(in_pos, 1.0)).xyz, normalize((anim * vec4(in_nor.xyz, 0.0)).xyz));
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
		vec3 pos = vec3((data.xxx >> uvec3(0, 8, 16)) & 255u) / 255.0;
		vec3 nor = unpackSnorm4x8(data.y).xyz;
		return PoseVertex(pos, normalize(nor));
	}

#endif // MD5

float r_avertexnormal_dot(vec3 vertexnormal, vec3 dir)
{
	float d = dot(vertexnormal, dir);
	if (ModelHalfLambert > 0.5)
		return d * 0.5 + 0.5;
	// Standard lighting model
	if (d < 0.0)
		return 1.0 + d * (13.0 / 44.0);
	else
		return 1.0 + d;
}

#if MODE == 2
	layout(location=0) noperspective out vec2 out_texcoord;
#else
	layout(location=0) out vec2 out_texcoord;
#endif
layout(location=1) out vec4 out_color;
layout(location=2) out vec3 out_pos;
layout(location=3) noperspective out vec4 out_curr_clip;
layout(location=4) noperspective out vec4 out_prev_clip;
layout(location=5) flat out int out_flags;
layout(location=6) out vec3 vNormal;

const int ALIAS_FLAG_VIEWMODEL = 2;
const float SQRT2_INV = 0.7071067811865476;

void main()
{
	InstanceData inst = instances[gl_InstanceID];
	out_texcoord = in_uv;
	
	// Get interpolated pose
	PoseVertex pose1 = GetPoseVertex(inst.Pose1);
	PoseVertex pose2 = GetPoseVertex(inst.Pose2);
	vec3 local_vert = mix(pose1.pos, pose2.pos, inst.Blend);
	vec3 local_normal = normalize(mix(pose1.nor, pose2.nor, inst.Blend));
	
	// Construct world matrices
	mat4x3 worldmatrix = transpose(mat3x4(inst.WorldMatrix[0], inst.WorldMatrix[1], inst.WorldMatrix[2]));
	mat4x3 prev_worldmatrix = transpose(mat3x4(inst.PrevWorldMatrix[0], inst.PrevWorldMatrix[1], inst.PrevWorldMatrix[2]));
	
	// Transform vertices to world space
	vec3 world_vert = (worldmatrix * vec4(local_vert, 1.0)).xyz;
	vec3 prev_world_vert = (prev_worldmatrix * vec4(local_vert, 1.0)).xyz;
	
	// Calculate clip space positions
	vec4 curr_clip = ViewProj * vec4(world_vert, 1.0);
	vec4 prev_clip = PrevViewProj * vec4(prev_world_vert, 1.0);
	
	gl_Position = curr_clip;
	out_curr_clip = curr_clip;
	out_prev_clip = prev_clip;
	out_flags = inst.Flags;
	out_pos = world_vert - EyePos;
	
	// Calculate lighting orientation
	mat3 orientation = mat3(
		normalize(worldmatrix[0].xyz),
		normalize(worldmatrix[1].xyz),
		normalize(worldmatrix[2].xyz)
	);
	orientation = transpose(orientation);
	vec3 shadevector = normalize(orientation[0] + orientation[2]) * SQRT2_INV;
	
	// Calculate lighting
	float dot1 = r_avertexnormal_dot(pose1.nor, shadevector);
	float dot2 = r_avertexnormal_dot(pose2.nor, shadevector);
	float lighting = mix(dot1, dot2, inst.Blend);
	
	// Transform normal to world space
	mat3 normalMatrix = mat3(worldmatrix[0].xyz, worldmatrix[1].xyz, worldmatrix[2].xyz);
	vNormal = normalize(normalMatrix * local_normal);
	
	// Calculate lighting contributions
	vec3 dlight = inst.DLightColor.rgb * lighting * Overbright;
	vec3 ambient = max(inst.LightColor.rgb - inst.DLightColor.rgb, vec3(0.0)) * lighting * Overbright;
	
	// Different color calculation for viewmodels
	bool is_viewmodel = (inst.Flags & ALIAS_FLAG_VIEWMODEL) != 0;
	vec3 final_color = is_viewmodel ? (ambient + dlight) : (inst.LightColor.rgb * lighting * Overbright);
	
    out_color = clamp(vec4(final_color, inst.LightColor.a), vec4(0.0), vec4(Overbright));
}