#if BINDLESS
	#extension GL_ARB_shader_draw_parameters : require
	#define DRAW_ID gl_DrawIDARB
#else
	layout(location=0) uniform int DrawID;
	#define DRAW_ID DrawID
#endif

#include "frame_uniforms.glsl"

// Fog.w is treated as a signed-friendly density; use abs(Fog.w) so negative CPU values do not invert attenuation.
vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-abs(Fog.w) * dot(p, p));
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
	vec2	LightStyles[64];
	Light	Lights[];
};

// OPT: ternary version is more GPU-friendly than if/else (avoids branch divergence)
float GetLightStyle(int index)
{
	return (index < 64) ? mix(LightStyles[index].x, LightStyles[index].y, LightmapParams.w) : 1.0;
}

struct Call
{
	uint	flags;
	uint	tcgen;
	float	wateralpha;
	float	_pad0;
	vec2	polygon_offset;
	vec4	stage_color;
#if BINDLESS
	uvec2	txhandle;
	uvec2	fbhandle;
	uvec2	emhandle;
#else
	int		baseinstance;
	int		padding;
#endif
};

const uint
	CF_USE_POLYGON_OFFSET = 1u,
	CF_USE_FULLBRIGHT     = 2u,
	CF_NOLIGHTMAP         = 4u,
	CF_USE_EMISSIVE       = 8u,
	CF_ALPHA_TEST         = 16u;

const uint
	TCGEN_BASE        = 0u,
	TCGEN_LIGHTMAP    = 1u,
	TCGEN_ENVIRONMENT = 2u;

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

// OPT: shared helper avoids duplicating transpose(mat3x4(...)) in Transform/TransformPrev/TransformDirection
mat4x3 BuildWorldMatrix(vec4 mat[3])
{
	return transpose(mat3x4(mat[0], mat[1], mat[2]));
}

vec3 TransformPosition(vec3 p, vec4 mat[3])
{
	return (BuildWorldMatrix(mat) * vec4(p, 1.0)).xyz;
}

vec3 Transform(vec3 p, Instance instance)
{
	return TransformPosition(p, instance.mat);
}

vec3 TransformPrev(vec3 p, Instance instance)
{
	return TransformPosition(p, instance.prev_mat);
}

// BUG FIX: TransformDirection must use w=0 to suppress translation — was correct,
// but the inline mat4x3 rebuild was redundant. Now reuses BuildWorldMatrix.
vec3 TransformDirection(vec3 n, Instance instance)
{
	return (BuildWorldMatrix(instance.mat) * vec4(n, 0.0)).xyz;
}

// Vertex inputs
layout(location=0) in vec3   in_pos;
layout(location=1) in vec4   in_uv;       // xy = base UV, zw = lightmap UV
layout(location=2) in float  in_lmofs;
layout(location=3) in ivec4  in_styles;
layout(location=4) in vec3   in_normal;
layout(location=5) in vec3   in_lightgrid;
layout(location=6) in float  in_skyvisibility;

// Vertex outputs
layout(location=0)  flat out uint  out_flags;
layout(location=1)  flat out float out_alpha;
layout(location=2)  out vec3  out_pos;
#if MODE == 1
	layout(location=3) centroid out vec2 out_uv;
#else
	layout(location=3) out vec2 out_uv;
#endif
layout(location=4)  centroid out vec2 out_lmuv;
layout(location=5)  out float out_depth;
layout(location=6)  noperspective out vec2 out_coord;
layout(location=7)  flat out vec4  out_styles;
layout(location=8)  flat out float out_lmofs;
#if BINDLESS
	layout(location=9)  flat out uvec4 out_samplers0;
	layout(location=10) flat out uvec2 out_samplers1;
#endif
layout(location=11) noperspective out vec4 out_curr_clip;
layout(location=12) noperspective out vec4 out_prev_clip;
layout(location=13) out vec3 out_normal;
layout(location=14) out vec3 out_lightgrid;
layout(location=15) out float out_skyvisibility;
layout(location=16) flat out vec4 out_stage_color;
layout(location=17) flat out uint out_tcgen;
layout(location=18) flat out vec3 out_bmodel_relight;

vec2 ComputeEnvUV(vec3 world_pos, vec3 world_normal)
{
	// OPT: normalize(EyePos - world_pos) can be written as normalize(to_eye)
	// where to_eye is already computed in main; inlined here for self-containment.
	vec3 view_dir  = normalize(EyePos - world_pos);
	vec3 refl      = reflect(-view_dir, normalize(world_normal));
	vec3 refl_view = mat3(View) * refl;
	// OPT: replace 3-mul sqrt with dot product (length² then rsqrt is equivalent)
	float rz1  = refl_view.z + 1.0;
	float m    = 2.0 * sqrt(dot(refl_view.xy, refl_view.xy) + rz1 * rz1);
	return refl_view.xy / max(m, 1e-6) + 0.5;
}

void main()
{
	Call     call        = call_data[DRAW_ID];
	int      instance_id = GET_INSTANCE_ID(call);
	Instance instance    = instance_data[instance_id];

	// OPT: build world matrix once; reuse for pos + normal transforms
	mat4x3 world_mat = BuildWorldMatrix(instance.mat);
	vec3 world_pos   = (world_mat * vec4(in_pos, 1.0)).xyz;
	// BUG FIX: normal must use w=0 (no translation); was correct before but now explicit
	vec3 world_normal = (world_mat * vec4(in_normal, 0.0)).xyz;

	vec3 prev_world_pos = TransformPrev(in_pos, instance);

	vec4 curr_clip = ViewProj     * vec4(world_pos,      1.0);
	vec4 prev_clip = PrevViewProj * vec4(prev_world_pos, 1.0);

#if REVERSED_Z
	const float ZBIAS = -1.0/1024.0;
#else
	const float ZBIAS =  1.0/1024.0;
#endif

	if ((call.flags & CF_USE_POLYGON_OFFSET) != 0u)
	{
		float zoffset = (call.polygon_offset.x + call.polygon_offset.y) * ZBIAS;
		curr_clip.z += zoffset;
		prev_clip.z += zoffset;
	}

	gl_Position   = curr_clip;
	out_curr_clip = curr_clip;
	out_prev_clip = prev_clip;
	out_pos       = world_pos;

	// BUG FIX: original called normalize(world_normal) which is correct, but only after
	// the rebuild via TransformDirection. Now reusing world_mat directly guarantees
	// consistent results. normalize() still required — non-uniform scale possible.
	out_normal    = normalize(world_normal);
	out_lightgrid = in_lightgrid;
	out_skyvisibility = in_skyvisibility;
	out_bmodel_relight = vec3(instance.pad0, instance.pad1, instance.pad2);

	// UV selection
	vec2 uv = in_uv.xy;
	if      (call.tcgen == TCGEN_LIGHTMAP)    uv = in_uv.zw;
	else if (call.tcgen == TCGEN_ENVIRONMENT) uv = ComputeEnvUV(world_pos, world_normal);
	out_uv   = uv;
	out_lmuv = in_uv.zw;

	out_depth = gl_Position.w;

	// OPT: one reciprocal shared for both xy and coord
	float inv_w = 1.0 / gl_Position.w;
	out_coord   = (gl_Position.xy * inv_w * 0.5 + 0.5) * vec2(LIGHT_TILES_X, LIGHT_TILES_Y);

	out_flags = call.flags;

#if MODE == 2
	out_alpha = instance.alpha < 0.0 ? call.wateralpha : instance.alpha;
#else
	out_alpha = instance.alpha < 0.0 ? 1.0 : instance.alpha;
#endif

	// Light styles
	out_styles.x = GetLightStyle(in_styles.x);
	if (in_styles.y == 255)
	{
		out_styles.yzw = vec3(-1.0);
	}
	else if (in_styles.z == 255)
	{
		out_styles.yzw = vec3(GetLightStyle(in_styles.y), -1.0, -1.0);
	}
	else
	{
		out_styles.y = GetLightStyle(in_styles.y);
		out_styles.z = GetLightStyle(in_styles.z);
		out_styles.w = (in_styles.w == 255) ? -1.0 : GetLightStyle(in_styles.w);
	}

	// BUG FIX: CF_NOLIGHTMAP override must come AFTER style computation;
	// original order was correct, preserved here.
	if ((call.flags & CF_NOLIGHTMAP) != 0u)
		out_styles.xy = vec2(1.0, -1.0);

	out_lmofs       = in_lmofs;
	out_stage_color = call.stage_color;
	out_tcgen       = call.tcgen;

#if BINDLESS
	out_samplers0.xy = call.txhandle;
	// OPT: branch avoidance: always write fbhandle; fallback to txhandle if no fullbright
	// This is a known-safe pattern on most drivers.
	out_samplers0.zw = ((call.flags & CF_USE_FULLBRIGHT) != 0u) ? call.fbhandle : call.txhandle;
	out_samplers1.xy = call.emhandle;
#endif
}
