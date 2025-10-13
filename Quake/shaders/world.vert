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

#define LIGHT_TILES_X 32
#define LIGHT_TILES_Y 16
#define LIGHT_TILES_Z 32
#define MAX_LIGHTS    64

struct Light
{
	vec3	origin;
	float	radius;
	vec3	color;
	float	minlight;
};

layout(std430, binding=0) restrict readonly buffer LightBuffer
{
	float	LightStyles[64];
	Light	Lights[];
};

float GetLightStyle(int index)
{
	float result;
	if (index < 64)
		result = LightStyles[index];
	else
		result = 1.0;
	return result;
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
};
const uint
	CF_USE_POLYGON_OFFSET = 1u,
	CF_USE_FULLBRIGHT = 2u,
	CF_NOLIGHTMAP = 4u,
	CF_USE_EMISSIVE = 8u,
	CF_ALPHA_TEST = 16u
;

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
layout(location=2) out vec3 out_pos;
#if MODE == 1
	layout(location=3) centroid out vec2 out_uv;
#else
	layout(location=3) out vec2 out_uv;
#endif
layout(location=4) centroid out vec2 out_lmuv;
layout(location=5) out float out_depth;
layout(location=6) noperspective out vec2 out_coord;
layout(location=7) flat out vec4 out_styles;
layout(location=8) flat out float out_lmofs;
#if BINDLESS
	layout(location=9) flat out uvec4 out_samplers0;
        layout(location=10) flat out uvec2 out_samplers1;
#endif
layout(location=11) noperspective out vec4 out_curr_clip;
layout(location=12) noperspective out vec4 out_prev_clip;

void main()
{
	Call call = call_data[DRAW_ID];
	int instance_id = GET_INSTANCE_ID(call);
	Instance instance = instance_data[instance_id];
	vec3 world_pos = Transform(in_pos, instance);
	vec3 prev_world_pos = TransformPrev(in_pos, instance);
	vec4 curr_clip = ViewProj * vec4(world_pos, 1.0);
	vec4 prev_clip = PrevViewProj * vec4(prev_world_pos, 1.0);
#if REVERSED_Z
	const float ZBIAS = -1./1024;
#else
	const float ZBIAS =  1./1024;
#endif
	if ((call.flags & CF_USE_POLYGON_OFFSET) != 0u)
	{
		curr_clip.z += ZBIAS;
		prev_clip.z += ZBIAS;
	}
	gl_Position = curr_clip;
	out_curr_clip = curr_clip;
	out_prev_clip = prev_clip;
	out_pos = world_pos;
	out_uv = in_uv.xy;
	out_lmuv = in_uv.zw;
	out_depth = gl_Position.w;
	out_coord = (gl_Position.xy / gl_Position.w * 0.5 + 0.5) * vec2(LIGHT_TILES_X, LIGHT_TILES_Y);
	out_flags = call.flags;
#if MODE == 2
	out_alpha = instance.alpha < 0.0 ? call.wateralpha : instance.alpha;
#else
	out_alpha = instance.alpha < 0.0 ? 1.0 : instance.alpha;
#endif
	out_styles.x = GetLightStyle(in_styles.x);
	if (in_styles.y == 255)
		out_styles.yzw = vec3(-1.);
	else if (in_styles.z == 255)
		out_styles.yzw = vec3(GetLightStyle(in_styles.y), -1., -1.);
	else
		out_styles.yzw = vec3
		(
			GetLightStyle(in_styles.y),
			GetLightStyle(in_styles.z),
			GetLightStyle(in_styles.w)
		);
	if ((call.flags & CF_NOLIGHTMAP) != 0u)
		out_styles.xy = vec2(1., -1.);
	out_lmofs = in_lmofs;
#if BINDLESS
	out_samplers0.xy = call.txhandle;
	if ((call.flags & CF_USE_FULLBRIGHT) != 0u)
		out_samplers0.zw = call.fbhandle;
	else
		out_samplers0.zw = out_samplers0.xy;
	out_samplers1.xy = call.emhandle;
#endif
}
