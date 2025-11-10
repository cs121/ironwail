#if BINDLESS
	#extension GL_ARB_shader_draw_parameters : require
	#define DRAW_ID			gl_DrawIDARB
#else
	layout(location=0) uniform int DrawID;
	#define DRAW_ID			DrawID
#endif

#include "frame_uniforms.glsl"

#define MAX_EFFECT_STAGES 4

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
	uvec2	envhandle;
#else
	int		baseinstance;
	int		padding;
#endif // BINDLESS
        vec4    tcmod_matrix;
        vec4    tcmod_translate;
        vec4    tcmod_params0;
        vec4    tcmod_params1;
        vec4    tcgen_params;
        vec4    tcgen_s;
        vec4    tcgen_t;
        vec4    emissive_matrix;
        vec4    emissive_translate;
        vec4    emissive_color;
        vec4    fog_color;
        vec4    alpha_params0;
        vec4    alpha_params1;
        vec4    alpha_params2;
	vec4	env_params0;
	vec4	env_params1;
	vec4	env_params2;
	vec4	effect_info;
	vec4	effect_matrix[MAX_EFFECT_STAGES];
	vec4	effect_translate[MAX_EFFECT_STAGES];
	vec4	effect_color[MAX_EFFECT_STAGES];
	vec4	effect_alpha_params0[MAX_EFFECT_STAGES];
	vec4	effect_alpha_params1[MAX_EFFECT_STAGES];
	vec4	effect_alpha_params2[MAX_EFFECT_STAGES];
	vec4	effect_tcmod_params0[MAX_EFFECT_STAGES];
	vec4	effect_tcmod_params1[MAX_EFFECT_STAGES];
	vec4	effect_flags[MAX_EFFECT_STAGES];
#if BINDLESS
	uvec2	effect_handles[MAX_EFFECT_STAGES];
#endif
};
const uint
        CF_USE_POLYGON_OFFSET = 1u,
        CF_USE_FULLBRIGHT = 2u,
        CF_NOLIGHTMAP = 4u,
	CF_USE_EMISSIVE = 8u,
	CF_ALPHA_TEST = 16u,
	CF_TC_STRETCH = 32u,
	CF_TC_TURB = 64u,
	CF_TC_ENVMAP = 128u,
        CF_CUSTOM_FOG = 256u,
        CF_EFFECTS = 512u
;

const int IW_TCGEN_BASE        = 0;
const int IW_TCGEN_LIGHTMAP    = 1;
const int IW_TCGEN_ENVIRONMENT = 2;
const int IW_TCGEN_VECTOR      = 3;

const int IW_TC_ALIGN_OBJECT = 0;
const int IW_TC_ALIGN_WORLD  = 1;
const int IW_TC_ALIGN_SCREEN = 2;

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

vec3 TransformPrev(vec3 p, Instance instance)
{
	return TransformPosition(p, instance.prev_mat);
}

layout(location=0) in vec3 in_pos;
layout(location=1) in vec4 in_uv;
layout(location=2) in float in_lmofs;
layout(location=3) in ivec4 in_styles;


layout(location=0) flat out uint out_flags;
layout(location=1) flat out float out_alpha;
layout(location=2) out vec2 out_uv;
layout(location=3) out vec3 out_pos;
#if BINDLESS
        layout(location=4) flat out uvec4 out_samplers0;
        layout(location=5) flat out uvec2 out_samplers1;
#endif
layout(location=6) noperspective out vec4 out_curr_clip;
layout(location=7) noperspective out vec4 out_prev_clip;

layout(location=8) out vec2 out_emissive_uv;
layout(location=9) flat out vec3 out_emissive_color;
layout(location=10) flat out vec4 out_stage_params1;
layout(location=11) flat out vec4 out_alpha_params0;
layout(location=12) flat out vec4 out_alpha_params1;
layout(location=13) flat out vec4 out_alpha_params2;
layout(location=14) flat out vec4 out_fog_color;

float EvaluateWave(vec4 params, int func)
{
        float base = params.x;
        float amplitude = params.y;
        float phase = params.z;
        float frequency = params.w;
        float cycle = Time * frequency + phase / 360.0;
        float wave = 0.0;
        const float TAU = 6.28318530718;

        if (func == 0)
        {
                float angle = cycle * TAU;
                wave = sin(angle);
        }
        else if (func == 1)
        {
                float f = fract(cycle);
                wave = (abs(f * 2.0 - 1.0) * 2.0) - 1.0;
        }
        else if (func == 2)
        {
                float angle = cycle * TAU;
                wave = sign(sin(angle));
                if (wave == 0.0)
                        wave = 1.0;
        }
        else if (func == 3)
        {
                float f = fract(cycle);
                wave = f * 2.0 - 1.0;
        }
        else
        {
                float angle = cycle * TAU;
                wave = sin(angle);
        }

        return base + amplitude * wave;
}

void main()
{
        Call call = call_data[DRAW_ID];
        int instance_id = GET_INSTANCE_ID(call);
        Instance instance = instance_data[instance_id];
        vec3 world_pos = Transform(in_pos, instance);
        vec3 prev_world_pos = TransformPrev(in_pos, instance);
        vec4 curr_clip = ViewProj * vec4(world_pos, 1.0);
        vec4 prev_clip = PrevViewProj * vec4(prev_world_pos, 1.0);
        gl_Position = curr_clip;
        out_curr_clip = curr_clip;
        out_prev_clip = prev_clip;
        out_flags = call.flags;
        int tcGenMode = int(call.tcgen_params.x + 0.5);
        int tcAlignMode = int(call.tcgen_params.y + 0.5);
        vec2 base_uv = in_uv.xy;

        if (tcGenMode == IW_TCGEN_LIGHTMAP)
        {
                base_uv = in_uv.zw;
        }
        else if (tcGenMode == IW_TCGEN_VECTOR)
        {
                base_uv = vec2(dot(call.tcgen_s.xyz, world_pos) + call.tcgen_s.w,
                                dot(call.tcgen_t.xyz, world_pos) + call.tcgen_t.w);
        }
        else if (tcGenMode == IW_TCGEN_ENVIRONMENT)
        {
                vec3 normal = call.tcgen_s.xyz;
                float len = length(normal);
                if (len < 1e-4)
                        normal = vec3(0.0, 0.0, 1.0);
                else
                        normal /= len;
                vec3 view_dir = normalize(world_pos - EyePos);
                vec3 reflect_dir = reflect(view_dir, normal);
                float denom = 2.0 * sqrt(reflect_dir.x * reflect_dir.x + reflect_dir.y * reflect_dir.y + (reflect_dir.z + 1.0) * (reflect_dir.z + 1.0));
                if (denom > 1e-5)
                        base_uv = reflect_dir.xy / denom + 0.5;
                else
                        base_uv = reflect_dir.xy * 0.5 + 0.5;
        }

        if (tcAlignMode == IW_TC_ALIGN_WORLD && tcGenMode != IW_TCGEN_VECTOR)
        {
                base_uv = vec2(dot(call.tcgen_s.xyz, world_pos) + call.tcgen_s.w,
                                dot(call.tcgen_t.xyz, world_pos) + call.tcgen_t.w);
        }
        else if (tcAlignMode == IW_TC_ALIGN_SCREEN)
        {
                float inv_w = abs(curr_clip.w) > 1e-6 ? 1.0 / curr_clip.w : 0.0;
                vec2 ndc = curr_clip.xy * inv_w;
                base_uv = ndc * 0.5 + 0.5;
        }

        vec2 transformed_uv = vec2(dot(call.tcmod_matrix.xy, base_uv),
        dot(call.tcmod_matrix.zw, base_uv)) + call.tcmod_translate.xy;
        if ((call.flags & CF_TC_STRETCH) != 0u)
        {
                int func = int(call.tcmod_params1.w + 0.5);
                float scale = EvaluateWave(call.tcmod_params0, func);
                transformed_uv = (transformed_uv - 0.5) * scale + 0.5;
        }
        if ((call.flags & CF_TC_TURB) != 0u)
        {
                float amp = call.tcmod_params1.x;
                float freq = call.tcmod_params1.y;
                const vec2 turbScale = vec2(0.125, 0.125);
                float angle = Time * freq * 6.28318530718 + dot(world_pos.xy, turbScale);
                float offset = sin(angle) * amp;
                transformed_uv += vec2(offset);
        }
        out_uv = transformed_uv;
        vec2 emissive_uv = vec2(dot(call.emissive_matrix.xy, base_uv),
                                dot(call.emissive_matrix.zw, base_uv)) + call.emissive_translate.xy;
        out_emissive_uv = emissive_uv;
        out_pos = world_pos - EyePos;
        out_alpha = instance.alpha < 0.0 ? call.wateralpha : instance.alpha;
#if BINDLESS
        out_samplers0.xy = call.txhandle;
        if ((call.flags & CF_USE_FULLBRIGHT) != 0u)
                out_samplers0.zw = call.fbhandle;
        else
                out_samplers0.zw = out_samplers0.xy;
        out_samplers1.xy = call.emhandle;
#endif
        out_emissive_color = call.emissive_color.xyz;
        out_stage_params1 = call.tcmod_params1;
        out_alpha_params0 = call.alpha_params0;
        out_alpha_params1 = call.alpha_params1;
        out_alpha_params2 = call.alpha_params2;
        out_fog_color = call.fog_color;
}

