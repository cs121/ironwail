struct InstanceData
{
        vec4    WorldMatrix[3];
        vec4    PrevWorldMatrix[3];
        vec4    LightColor; // xyz=color, w=alpha
        int     Pose1;
        int     Pose2;
        float   Blend;
        int     Flags;
};

layout(std430, binding=1) restrict readonly buffer InstanceBuffer
{
        mat4    ViewProj;
        mat4    PrevViewProj;
        vec3    EyePos;
        float   _Pad0;
        vec4    Fog;
        float   ScreenDither;
        float   _Pad1;
        float   _Pad2;
        float   _Pad3;
        InstanceData instances[];
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

vec3 GetPosePosition(uint pose)
{
        mat3x4 blendmat = BonePoses[pose + in_indices.x] * in_weights.x;
        blendmat += BonePoses[pose + in_indices.y] * in_weights.y;
        if (in_weights.z + in_weights.w > 0.0)
        {
                blendmat += BonePoses[pose + in_indices.z] * in_weights.z;
                blendmat += BonePoses[pose + in_indices.w] * in_weights.w;
        }
        mat4x3 anim = transpose(blendmat);
        return (anim * vec4(in_pos, 1.0)).xyz;
}
#else
layout(location=0) in vec2 in_uv;

layout(std430, binding=2) restrict readonly buffer BlendShapeBuffer
{
        uvec2 PackedPosNor[];
};

vec3 GetPosePosition(uint pose)
{
        uvec2 data = PackedPosNor[pose + gl_VertexID];
        return vec3((data.xxx >> uvec3(0, 8, 16)) & 255u);
}
#endif

layout(location=0) out vec4 out_color;
layout(location=1) out vec3 out_pos;

void main()
{
        InstanceData inst = instances[gl_InstanceID];
        vec3 pos1 = GetPosePosition(inst.Pose1);
        vec3 pos2 = GetPosePosition(inst.Pose2);
        vec3 local_pos = mix(pos1, pos2, inst.Blend);

        mat4x3 world = transpose(mat3x4(inst.WorldMatrix[0], inst.WorldMatrix[1], inst.WorldMatrix[2]));
        vec3 world_pos = (world * vec4(local_pos, 1.0)).xyz;

        gl_Position = ViewProj * vec4(world_pos, 1.0);
        out_color = inst.LightColor;
        out_pos = world_pos - EyePos;
}
