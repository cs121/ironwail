#if BINDLESS
	#extension GL_ARB_shader_draw_parameters : require
	#define DRAW_ID			gl_DrawIDARB
#else
	layout(location=0) uniform int DrawID;
	#define DRAW_ID			DrawID
#endif

#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-Fog.w * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}

struct Call
{
	uint	flags;
	float	wateralpha;
#if BINDLESS
	uvec2	txhandle;
	uvec2	fbhandle;
	uvec2	emhandle;
#else
	int		baseinstance;
	int		padding;
#endif // BINDLESS
        uvec4   tcgen_mode;
        vec4    tcgen_basis0;
        vec4    tcgen_basis1;
        vec4    emissive_tcgen_basis0;
        vec4    emissive_tcgen_basis1;
        vec4    tcmod_matrix;
        vec4    tcmod_translate;
        vec4    emissive_matrix;
        vec4    emissive_translate;
        vec4    emissive_color;
};
const uint
	CF_USE_POLYGON_OFFSET = 1u,
	CF_USE_FULLBRIGHT = 2u,
	CF_NOLIGHTMAP = 4u,
	CF_USE_EMISSIVE = 8u
;
const uint TCGEN_OBJECT = 0u;
const uint TCGEN_WORLD = 1u;
const uint TCGEN_SCREEN = 2u;

bool ScreenViewportValid()
{
        return Frame.ScreenTexScale.z > 0.0 && Frame.ScreenTexScale.w > 0.0;
}

vec2 EvaluateBaseUV(uint mode, vec4 basis0, vec4 basis1, vec3 world_pos, vec4 clip_pos, vec2 object_uv)
{
        if (mode == TCGEN_WORLD)
        {
                return vec2(dot(basis0.xyz, world_pos) + basis0.w,
                            dot(basis1.xyz, world_pos) + basis1.w);
        }
        if (mode == TCGEN_SCREEN)
        {
                vec2 screen01 = clip_pos.xy / clip_pos.w * 0.5 + 0.5;
                if (ScreenViewportValid())
                        return screen01 * Frame.ScreenTexScale.zw;
                return screen01;
        }
        return object_uv;
}

vec2 FinalizeUV(uint mode, vec2 uv)
{
        if (mode == TCGEN_SCREEN && ScreenViewportValid())
                return uv * Frame.ScreenTexScale.xy;
        return uv;
}

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

vec3 TransformPosition(vec3 p, vec4 mat[3])
{
	mat4x3 world = transpose(mat3x4(mat[0], mat[1], mat[2]));
	return (world * vec4(p, 1.0)).xyz;
}

vec3 Transform(vec3 p, Instance instance)
{
	return TransformPosition(p, instance.mat);
}

layout(location=0) in vec3 in_pos;
layout(location=1) in vec4 in_uv;
layout(location=2) in float in_lmofs;
layout(location=3) in ivec4 in_styles;


layout(location=0) out vec3 out_dir;
#if BINDLESS
	layout(location=1) flat out uvec4 out_samplers;
#endif

void main()
{
	Call call = call_data[DRAW_ID];
	int instance_id = GET_INSTANCE_ID(call);
	Instance instance = instance_data[instance_id];
	vec3 pos = Transform(in_pos, instance);
	gl_Position = ViewProj * vec4(pos, 1.0);
	out_dir = pos - EyePos;
	out_dir.z *= 3.0; // flatten the sphere
#if BINDLESS
	out_samplers.xy = call.txhandle;
	out_samplers.zw = call.fbhandle;
#endif
}
