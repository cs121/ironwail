#if BINDLESS
        #extension GL_ARB_shader_draw_parameters : require
        #define DRAW_ID                 gl_DrawIDARB
#else
        layout(location=0) uniform int DrawID;
        #define DRAW_ID                 DrawID
#endif

#include "frame_uniforms.glsl"

#define LIGHT_TILES_X 32
#define LIGHT_TILES_Y 16
#define LIGHT_TILES_Z 32
#define MAX_LIGHTS    64

struct Light
{
        vec3    origin;
        float   radius;
        vec3    color;
        float   minlight;
};

layout(std430, binding=0) restrict readonly buffer LightBuffer
{
        vec2    LightStyles[64];
        Light   Lights[];
};

float GetLightStyle(int index)
{
        float result;
        if (index < 64)
                result = mix(LightStyles[index].x, LightStyles[index].y, LightmapParams.w);
        else
                result = 1.0;
        return result;
}

struct Call
{
        uint    flags;
        float   wateralpha;
#if BINDLESS
        uvec2   txhandle;
        uvec2   fbhandle;
        uvec2   emhandle;
#else
        int             baseinstance;
        int             padding;
#endif // BINDLESS
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
        vec4    mat[3];
        vec4    prev_mat[3];
        float   alpha;
        float   pad0;
        float   pad1;
        float   pad2;
};

layout(std430, binding=2) restrict readonly buffer InstanceBuffer
{
        Instance instance_data[];
};

vec3 TransformPosition(vec3 p, vec4 mat[3])
{
        mat4x3 world = transpose(mat3x4(mat[0], mat[1], mat[2]));
        return (world * vec4(p, 1.0)).xyz;
}

vec3 Transform(vec3 p, Instance instance)
{
        return TransformPosition(p, instance.mat);
}

vec3 TransformDirection(vec3 n, Instance instance)
{
        mat4x3 world = transpose(mat3x4(instance.mat[0], instance.mat[1], instance.mat[2]));
        return (world * vec4(n, 0.0)).xyz;
}

layout(location=0) in vec3 in_pos;
layout(location=1) in vec4 in_uv;
layout(location=2) in float in_lmofs;
layout(location=3) in ivec4 in_styles;
layout(location=4) in vec3 in_normal;

layout(location=0) flat out uint out_flags;
layout(location=1) out vec3 out_pos;
layout(location=2) out vec3 out_normal;
layout(location=3) out vec2 out_uv;
layout(location=4) out vec2 out_lmuv;
layout(location=5) flat out vec4 out_styles;
layout(location=6) flat out float out_lmofs;
#if BINDLESS
        layout(location=7) flat out uvec4 out_samplers0;
#endif

void main()
{
        Call call = call_data[DRAW_ID];
        int instance_id = GET_INSTANCE_ID(call);
        Instance instance = instance_data[instance_id];
        vec3 world_pos = Transform(in_pos, instance);
        vec3 world_normal = TransformDirection(in_normal, instance);
        vec4 clip_pos = ViewProj * vec4(world_pos, 1.0);

        gl_Position = clip_pos;
        out_pos = world_pos;
        out_normal = normalize(world_normal);
        out_uv = in_uv.xy;
        out_lmuv = in_uv.zw;
        out_flags = call.flags;
        out_styles.x = GetLightStyle(in_styles.x);
        if (in_styles.y == 255)
                out_styles.yzw = vec3(-1.);
        else if (in_styles.z == 255)
                out_styles.yzw = vec3(GetLightStyle(in_styles.y), -1., -1.);
        else
                out_styles.yzw = vec3(
                        GetLightStyle(in_styles.y),
                        GetLightStyle(in_styles.z),
                        GetLightStyle(in_styles.w)
                );
        if ((call.flags & 4u) != 0u)
                out_styles.xy = vec2(1., -1.);
        out_lmofs = in_lmofs;
#if BINDLESS
        out_samplers0.xy = call.txhandle;
        out_samplers0.zw = out_samplers0.xy;
#endif
}
