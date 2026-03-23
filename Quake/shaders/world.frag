#if BINDLESS
	#extension GL_ARB_bindless_texture : require
#else
	layout(binding=0) uniform sampler2D Tex;
	layout(binding=1) uniform sampler2D FullbrightTex;
	layout(binding=4) uniform sampler2D EmissiveTex;
#endif
layout(binding=2) uniform sampler2D LMTex;
layout(binding=3) uniform sampler2D LMTexDir;
layout(binding=7) uniform sampler2DArray SunShadowTex;
layout(binding=8) uniform samplerCubeArray DLightShadowTex;

uniform mat4 ShadowSunViewProj[4];
uniform vec4 ShadowSunSplits;
uniform int ShadowSunCascadeCount;
layout(location=34) uniform vec4 ShadowEnableDebug;   // x=enabled, y=sun, z=dlight, w=debug mode
layout(location=35) uniform vec4 ShadowDLightIndices; // selected light indices (float encoded ints)
layout(location=36) uniform vec4 ShadowBiasCounts;    // x=num dlight slots, y=sun bias, z=dlight bias, w=receiver bias scale
layout(location=37) uniform vec4 ShadowPCFTexel;      // x=sun pcf radius, y=dlight pcf radius, z=1/sun size, w=1/dlight size
layout(location=45) uniform vec4 RimLightParams0; // x=enable, y=intensity, z=power, w=shadowed
layout(location=46) uniform vec4 RimLightParams1; // x=world_enable, y=model_enable, z=sun_scale, w=dlight_scale
#include "frame_uniforms.glsl"

// Fog.w is treated as a signed-friendly density; use abs(Fog.w) so negative CPU values do not invert attenuation.
vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-abs(Fog.w) * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}

float FogAttenuation(vec3 p)
{
	float fog = exp2(-abs(Fog.w) * dot(p, p));
	return clamp(fog, 0.0, 1.0);
}

float SanitizeScalar(float x)
{
	return (x == x) ? clamp(x, 0.0, 65504.0) : 0.0;
}

vec3 SanitizeColor(vec3 color)
{
	return vec3(
		SanitizeScalar(color.x),
		SanitizeScalar(color.y),
		SanitizeScalar(color.z));
}

#define MAX_LIGHTS    64
#define LIGHT_TILES_X 32
#define LIGHT_TILES_Y 16
#define LIGHT_TILES_Z 32

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
	CF_ALPHA_TEST         = 16u,
	CF_MAT_BLOOM          = 128u,
	CF_MAT_EMISSIVE       = 256u,
	CF_MAT_GODRAY         = 512u,
	CF_MAT_TRANS          = 1024u,
	CF_MAT_SKY            = 2048u,
	CF_MAT_HAS_SHADER     = 4096u;

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

// Inputs
layout(location=0)  flat in uint  in_flags;
layout(location=1)  flat in float in_alpha;
layout(location=2)  in vec3  in_pos;
#if MODE == 1
	layout(location=3) centroid in vec2 in_uv;
#else
	layout(location=3) in vec2 in_uv;
#endif
layout(location=4)  centroid in vec2 in_lmuv;
layout(location=5)  in float in_depth;
layout(location=6)  noperspective in vec2 in_coord;
layout(location=7)  flat in vec4  in_styles;
layout(location=8)  flat in float in_lmofs;
#if BINDLESS
	layout(location=9)  flat in uvec4 in_samplers0;
	layout(location=10) flat in uvec2 in_samplers1;
#endif
layout(location=11) noperspective in vec4 in_curr_clip;
layout(location=12) noperspective in vec4 in_prev_clip;
layout(location=13) in vec3 in_normal;
layout(location=14) in vec3 in_lightgrid;
layout(location=15) in float in_skyvisibility;
layout(location=16) flat in vec4 in_stage_color;
layout(location=17) flat in uint in_tcgen;
layout(location=18) flat in vec3 in_bmodel_relight;

// Utility: ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));
	v = (v ^ (v << 2)) & 0x3333u;
	v = (v ^ (v << 1)) & 0x5555u;
	v |= v >> 7;
	v = bitfieldReverse(v) >> 24;
	return float(v) * (1.0/256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

// Hash without Sine
float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float whitenoise(vec2 p)
{
	return whitenoise01(p) - 0.5;
}

// Forward+ prep: shared tile coordinate helper for upcoming clustered light lists.
uvec3 ComputeLightTileCoord(vec2 tile_coord, float view_depth)
{
	uint tx = uint(clamp(floor(tile_coord.x), 0.0, float(LIGHT_TILES_X - 1)));
	uint ty = uint(clamp(floor(tile_coord.y), 0.0, float(LIGHT_TILES_Y - 1)));
	float z = max(view_depth, 1e-6);
	float tzf = clamp(log2(z) * ZLogScale + ZLogBias, 0.0, float(LIGHT_TILES_Z - 1));
	return uvec3(tx, ty, uint(tzf));
}

// Convert uniform to triangle distribution
float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.0;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

#define DITHER_NOISE(uv)      tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE()  DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING()    bayer(ivec2(gl_FragCoord.xy))

vec4 SampleLightmap(vec2 uv)
{
	return texture(LMTex, uv);
}

vec3 SampleLightmapDir(vec2 uv)
{
	// OPT: fma-friendly; normalize is unavoidable
	vec3 dir = texture(LMTexDir, uv).xyz * 2.0 - 1.0;
	return normalize(dir);
}

vec2 ComputeVelocity(vec4 curr_clip, vec4 prev_clip)
{
	const float EPS = 1e-6;
	float inv_curr_w = (abs(curr_clip.w) > EPS) ? (1.0 / curr_clip.w) : 0.0;
	float inv_prev_w = (abs(prev_clip.w) > EPS) ? (1.0 / prev_clip.w) : 0.0;
	vec2 curr_ndc = curr_clip.xy * inv_curr_w;
	vec2 prev_ndc = prev_clip.xy * inv_prev_w;
	return (curr_ndc - prev_ndc) * 0.5;
}

float DepthToCanonical(float depth)
{
#if REVERSED_Z
	return 1.0 - depth;
#else
	return depth;
#endif
}

int ShadowSlotForLight(int lightIndex)
{
	if (abs(ShadowDLightIndices.x - float(lightIndex)) < 0.5) return 0;
	if (abs(ShadowDLightIndices.y - float(lightIndex)) < 0.5) return 1;
	if (abs(ShadowDLightIndices.z - float(lightIndex)) < 0.5) return 2;
	if (abs(ShadowDLightIndices.w - float(lightIndex)) < 0.5) return 3;
	return -1;
}

int ShadowCascadeForWorldPos(vec3 worldPos)
{
	int cascades = clamp(ShadowSunCascadeCount, 1, 4);
	float dist = length(worldPos - EyePos);
	if (cascades <= 1) return 0;
	if (dist <= ShadowSunSplits.x || cascades == 1) return 0;
	if (dist <= ShadowSunSplits.y || cascades == 2) return 1;
	if (dist <= ShadowSunSplits.z || cascades == 3) return 2;
	return cascades - 1;
}

float SampleSunShadow(vec3 worldPos)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.y < 0.5)
		return 1.0;

	int cascade = ShadowCascadeForWorldPos(worldPos);
	vec4 clip = ShadowSunViewProj[cascade] * vec4(worldPos, 1.0);
	if (abs(clip.w) <= 1e-6)
		return 1.0;

	vec3 ndc = clip.xyz / clip.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	// Sun shadow map is always rendered with a standard (non-reversed) ortho
	// projection on the CPU side, so depth lives in [0,1] with 0=near, 1=far.
	float depth = ndc.z * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth < 0.0 || depth > 1.0)
		return 1.0;

	float bias = max(ShadowBiasCounts.y, 0.0);
	float receiver_bias = max(fwidth(depth), 0.0) * max(ShadowBiasCounts.w, 0.0);
	bias += receiver_bias;
	float pcf = max(ShadowPCFTexel.x, 0.0) * max(ShadowPCFTexel.z, 0.0);
	float sum = 0.0;
	float taps = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			float closest = texture(SunShadowTex, vec3(uv + vec2(float(x), float(y)) * pcf, float(cascade))).r;
			// Standard map: receiver is lit when its depth (minus bias) is <= stored depth.
			sum += (depth - bias <= closest) ? 1.0 : 0.0;
			taps += 1.0;
		}
	}
	return (taps > 0.0) ? (sum / taps) : 1.0;
}

float SampleSunShadowDepth(vec3 worldPos)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.y < 0.5)
		return 1.0;

	int cascade = ShadowCascadeForWorldPos(worldPos);
	vec4 clip = ShadowSunViewProj[cascade] * vec4(worldPos, 1.0);
	if (abs(clip.w) <= 1e-6)
		return 1.0;

	vec3 ndc = clip.xyz / clip.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
		return 1.0;

	return texture(SunShadowTex, vec3(uv, float(cascade))).r;
}

float SampleDLightShadow(int lightIndex, vec3 worldPos, vec3 lightPos, float radius)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.z < 0.5)
		return 1.0;

	int slot = ShadowSlotForLight(lightIndex);
	if (slot < 0)
		return 1.0;

	vec3 dir = worldPos - lightPos;
	float dist = length(dir);
	if (dist <= 1e-5 || radius <= 1e-5)
		return 1.0;

	vec3 dirN = dir / dist;
	// INVARIANTE: ref = dist/radius wird mit stored = dist_occluder/farPlane verglichen.
	// Korrekt NUR wenn farPlane (ShadowLightPosFar.w) == radius beim Shadow-Pass.
	// CPU muss ShadowLightPosFar.w = l.radius setzen — sonst falsche Tiefenvergleiche.
	float ref = dist / radius;
	float bias = max(ShadowBiasCounts.z, 0.0);
	float receiver_bias = max(fwidth(ref), 0.0) * max(ShadowBiasCounts.w, 0.0);
	bias += receiver_bias;
	float pcf = max(ShadowPCFTexel.y, 0.0) * max(ShadowPCFTexel.w, 0.0) * 2.0;

	vec3 axis = (abs(dirN.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(axis, dirN));
	vec3 bitangent = normalize(cross(dirN, tangent));
	float vis = 0.0;
	float taps = 0.0;
	for (int i = 0; i < 4; ++i)
	{
		vec2 o = vec2((i & 1) == 0 ? -1.0 : 1.0, (i < 2) ? -1.0 : 1.0) * pcf;
		vec3 sampleDir = normalize(dirN + tangent * o.x + bitangent * o.y);
		float closest = texture(DLightShadowTex, vec4(sampleDir, float(slot))).r;
		vis += (ref - bias <= closest) ? 1.0 : 0.0;
		taps += 1.0;
	}
	return vis / max(taps, 1.0);
}

float ComputeCombinedDLightShadow(vec3 worldPos)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.z < 0.5)
		return 1.0;

	float accum = 0.0;
	float wsum = 0.0;
	int slots = int(clamp(ShadowBiasCounts.x, 0.0, 4.0));
	for (int slot = 0; slot < slots; ++slot)
	{
		int idx = int(round((slot == 0) ? ShadowDLightIndices.x :
				    (slot == 1) ? ShadowDLightIndices.y :
				    (slot == 2) ? ShadowDLightIndices.z : ShadowDLightIndices.w));
		if (idx < 0)
			continue;
		Light l = Lights[idx];
		vec3 d = worldPos - l.origin;
		float dist = length(d);
		if (dist >= l.radius || l.radius <= 1e-5)
			continue;
		float w = 1.0 - dist / l.radius;
		float vis = SampleDLightShadow(idx, worldPos, l.origin, l.radius);
		accum += vis * w;
		wsum += w;
	}

	return (wsum > 1e-6) ? (accum / wsum) : 1.0;
}

float SampleFirstDLightDepth(vec3 worldPos)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.z < 0.5)
		return 1.0;
	if (ShadowBiasCounts.x < 0.5)
		return 1.0;

	int idx = int(round(ShadowDLightIndices.x));
	if (idx < 0)
		return 1.0;

	Light l = Lights[idx];
	vec3 dir = worldPos - l.origin;
	float len = length(dir);
	if (len <= 1e-5)
		return 1.0;

	return texture(DLightShadowTex, vec4(dir / len, 0.0)).r;
}

#define OUT_COLOR out_fragcolor

#if OIT
	vec4 OUT_COLOR;
	layout(location=0) out vec4 out_accum;
	layout(location=1) out float out_reveal;

	vec3 GammaToLinear(vec3 v)
	{
		return v;
	}

	void main_body();

	void main()
	{
		main_body();
		OUT_COLOR = clamp(OUT_COLOR, 0.0, 1.0);
		vec4 color = vec4(GammaToLinear(OUT_COLOR.rgb), OUT_COLOR.a);
		float z = 1.0 / gl_FragCoord.w;
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);
		// WBOIT: weighted-blended OIT (McGuire & Bavoil 2013).
		// accum.rgb = pre-multiplied color * weight
		// accum.a   = alpha * weight  (used for normalization in composite pass)
		// reveal    = alpha            (product across layers, written additively)
		out_accum  = vec4(color.rgb * color.a * weight, color.a * weight);
		out_reveal = color.a;
	}

	#define main main_body
#else
	layout(location=0) out vec4 OUT_COLOR;
	layout(location=1) out vec4 out_velocity;
#endif

void main()
{
	vec3 fullbright = vec3(0.0);
	vec3 emissive   = vec3(0.0);
	vec2 uv = in_uv;

#if MODE == 2
	uv = uv * 2.0 + 0.125 * sin(uv.yx * (3.14159265 * 2.0) + Time);
#endif

	int tcgen_debug = int(ShaderParams.y + 0.5);
	if (tcgen_debug > 0 && in_tcgen == TCGEN_ENVIRONMENT)
	{
		// BUG FIX: was writing to undeclared "out_fragcolor" alias before OUT_COLOR macro
		// is set up. Use OUT_COLOR consistently.
		OUT_COLOR = vec4(fract(uv), 0.0, 1.0);
#if !OIT
		out_velocity = vec4(0.0);
#endif
		return;
	}

	// Sample textures
#if BINDLESS
	sampler2D Tex = sampler2D(in_samplers0.xy);
	if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
	{
		sampler2D FullbrightTex = sampler2D(in_samplers0.zw);
		fullbright = texture(FullbrightTex, uv).rgb;
	}
	if ((in_flags & CF_USE_EMISSIVE) != 0u)
	{
		sampler2D EmissiveSampler = sampler2D(in_samplers1.xy);
		emissive = texture(EmissiveSampler, uv).rgb;
	}
#else
	if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
		fullbright = texture(FullbrightTex, uv).rgb;
	if ((in_flags & CF_USE_EMISSIVE) != 0u)
		emissive = texture(EmissiveTex, uv).rgb;
#endif

#if DITHER >= 2
	vec4 result = texture(Tex, uv, -1.0);
#elif DITHER
	vec4 result = texture(Tex, uv, -0.5);
#else
	vec4 result = texture(Tex, uv);
#endif
	vec3 albedo = result.rgb;

#if MODE == 1
	if (result.a < 0.666)
		discard;
#endif

	// --- Debug modes (early-out) ---
	int debug_mode = int(ColorSpaceParams.x + 0.5);
	int lighting_debug_view = int(ColorSpaceParams.w + 0.5);
	if (debug_mode == 1)
	{
		// BUG FIX: original wrote to literal "out_fragcolor" which is only a valid
		// identifier when OIT==0. Use OUT_COLOR (resolves to the correct output in both paths).
		OUT_COLOR = vec4(result.rgb, 1.0);
#if !OIT
		out_velocity = vec4(0.0);
#endif
		return;
	}

	int shader_debug = int(ShaderParams.x + 0.5);
	if (shader_debug >= 2)
	{
		vec3 debug_color = vec3(0.0);
		if ((in_flags & CF_MAT_BLOOM)    != 0u) debug_color += vec3(1.0, 0.0, 1.0);
		if ((in_flags & CF_MAT_EMISSIVE) != 0u) debug_color += vec3(1.0, 1.0, 0.0);
		if ((in_flags & CF_MAT_GODRAY)   != 0u) debug_color += vec3(0.0, 1.0, 1.0);
		if ((in_flags & CF_MAT_TRANS)    != 0u) debug_color += vec3(0.0, 1.0, 0.0);
		if ((in_flags & CF_MAT_SKY)      != 0u) debug_color += vec3(0.0, 0.0, 1.0);
		if (all(lessThanEqual(debug_color, vec3(0.0)))) debug_color = result.rgb;
		// BUG FIX: same out_fragcolor alias issue
		OUT_COLOR = vec4(clamp(debug_color, 0.0, 1.0), 1.0);
#if !OIT
		out_velocity = vec4(0.0);
#endif
		return;
	}

	bool additive_dlights = (DLightParams.x > 0.5) && (lighting_debug_view == 0);

	// Lightmap sampling
	vec2 lmuv = in_lmuv;
	vec3 total_lightmap = vec3(1.0);
	vec3 specular_light = vec3(0.0);
	float ndotl_accum = 0.0;
	float ndotl_weight = 0.0;
#if DITHER
	// OPT: compute once, reuse below for dithering
	vec2 lmsize = vec2(textureSize(LMTex, 0).xy) * 16.0;
#endif

	// Surface normal (computed early; needed inside and outside lightmap block)
	vec3 surface_normal = in_normal;
	{
		float surface_normal_len = length(surface_normal);
		if (surface_normal_len > 0.0)
		{
			surface_normal /= surface_normal_len;
		}
		else
		{
			vec3 gn = cross(dFdx(in_pos), dFdy(in_pos));
			float gl = length(gn);
			surface_normal = (gl > 0.0) ? (gn / gl) : vec3(0.0, 0.0, 1.0);
		}
		if (!gl_FrontFacing)
			surface_normal = -surface_normal;
	}

	// View direction (computed once; used by specular lighting)
	vec3 to_eye    = EyePos - in_pos;
	float view_len = length(to_eye);
	// OPT: avoid normalize() call by inlining division guard
	vec3 view_dir  = (view_len > 0.0) ? (to_eye / view_len) : vec3(0.0, 0.0, 1.0);
	float rim_factor = 0.0;
	if (RimLightParams0.x > 0.5 && RimLightParams1.x > 0.5)
	{
		float rim_ndotv = 1.0 - clamp(dot(surface_normal, view_dir), 0.0, 1.0);
		rim_factor = pow(max(rim_ndotv, 0.0), max(RimLightParams0.z, 0.5)) * max(RimLightParams0.y, 0.0);
	}
	vec3 rim_dlight_accum = vec3(0.0);
	vec3 rim_sun_accum = vec3(0.0);

	if ((in_flags & CF_NOLIGHTMAP) == 0u)
	{
#if DITHER
		lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;
#endif

		vec4 lm0 = SampleLightmap(lmuv);
		vec3 static_light;

		if (in_styles.y < 0.0) // Single style fast path
		{
			static_light = in_styles.x * lm0.xyz;
		}
		else
		{
			vec4 lm1 = SampleLightmap(vec2(lmuv.x + in_lmofs, lmuv.y));
			if (in_styles.z < 0.0) // 2 styles
			{
				static_light = in_styles.x * lm0.xyz + in_styles.y * lm1.xyz;
			}
			else // 3 or 4 lightstyles
			{
				vec4 lm2 = SampleLightmap(vec2(lmuv.x + in_lmofs * 2.0, lmuv.y));
				// FIX: dot(in_styles, lmN) war falsch - lmN.w ist Lightmap-Alpha,
				// kein Lightstyle-Gewicht. Korrekter style-gewichteter Sum. lm3 fuer 4 Styles.
				if (in_styles.w < 0.0) // 3 styles
				{
					static_light = in_styles.x * lm0.xyz
					             + in_styles.y * lm1.xyz
					             + in_styles.z * lm2.xyz;
				}
				else // 4 styles
				{
					vec4 lm3 = SampleLightmap(vec2(lmuv.x + in_lmofs * 3.0, lmuv.y));
					static_light = in_styles.x * lm0.xyz
					             + in_styles.y * lm1.xyz
					             + in_styles.z * lm2.xyz
					             + in_styles.w * lm3.xyz;
				}
			}
		}

		// Sun-Shadow entfernt: Sonne ist keine Lichtquelle mehr.
		// DLight-Schatten werden ausschliesslich in world_dlight.frag pro Licht berechnet.
		// static_light geht ungekürzt in total_light ein.

		vec3 lightgrid   = mix(vec3(1.0), in_lightgrid, LightgridParams.x);

		if (LightgridParams.y > 0.5)
		{
			OUT_COLOR = vec4(lightgrid, 1.0);
#if !OIT
			out_velocity = vec4(0.0);
#endif
			return;
		}

		if (LightgridParams.w > 0.5)
		{
			float skyvis_debug = clamp(in_skyvisibility, 0.0, 1.0);
			OUT_COLOR = vec4(vec3(skyvis_debug), 1.0);
#if !OIT
			out_velocity = vec4(0.0);
#endif
			return;
		}

		// Directional lightmap
		if (LightmapParams.z > 0.5)
		{
			vec3 dir = SampleLightmapDir(lmuv);
			// BUG FIX: was dot(dir, vec3(0,0,1)) — must use the actual surface
			// normal so slanted geometry is shaded correctly.
			float ndl = max(dot(dir, surface_normal), 0.0);
			static_light *= ndl;
		}

		if (LightmapParams.x > 0.5)
		{
			vec3 static_light_debug = static_light * lightgrid;
			OUT_COLOR = vec4(clamp(static_light_debug, 0.0, 1.0), 1.0);
#if !OIT
			out_velocity = vec4(0.0);
#endif
			return;
		}

		vec3 clamped_static = clamp(static_light, 0.0, 1.0);
		vec3 total_light    = clamped_static * lightgrid + in_bmodel_relight;
		float sky_visibility = clamp(in_skyvisibility, 0.0, 1.0);
		float sky_upness = clamp(surface_normal.z * 0.5 + 0.5, 0.0, 1.0);
		float sky_room = 1.0 - 0.6 * max(max(clamped_static.r, clamped_static.g), clamped_static.b);
		vec3 sky_diffuse = SkyVisTint.rgb * (ShaderParams.z * mix(0.2, 1.0, sky_upness) * sky_visibility * max(sky_room, 0.0));
		sky_diffuse = min(sky_diffuse, vec3(SkyVisTint.a));
		total_light += max(min(sky_diffuse, 1.0 - total_light), 0.0);

		// Dynamic lights
		if (!additive_dlights && NumLights > 0u)
		{
			vec3  dynamic_light      = vec3(0.0);
			uvec3 tile_coord = ComputeLightTileCoord(in_coord, max(in_depth, 1e-6));
			float dynamic_light_noise = 1.0 - whitenoise01(in_pos.xy) * 0.15;
			dynamic_light_noise *= 1.0 + float(tile_coord.z) * 0.0;
			// OPT: precompute plane dot-product once outside loop
			vec4 plane = vec4(surface_normal, dot(in_pos, surface_normal));

			for (uint light_index = 0u; light_index < NumLights; light_index++)
			{
				Light l = Lights[light_index];

				float rad  = l.radius;
				float dist = dot(l.origin, plane.xyz) - plane.w;
				rad -= abs(dist);
				float minlight = l.minlight;

				if (rad <= 0.0 || rad < minlight)
					continue;

				vec3  local_pos = l.origin - plane.xyz * dist;
				minlight = rad - minlight;
				vec3  light_vec = local_pos - in_pos;
				float dist_sq = dot(light_vec, light_vec);

				// OPT: keep rsqrt path ready for clustered lists where this loop gets shorter.
				if (dist_sq < 1e-12)
					continue;
				float inv_surface_dist = inversesqrt(dist_sq);
				float surface_dist = dist_sq * inv_surface_dist;

				float attenuation   = clamp((minlight - surface_dist) * (1.0/16.0), 0.0, 1.0);
				float normalized_d  = surface_dist / rad;
				// OPT: pow(x,1.5) = x*sqrt(x), avoids generic pow()
				float nc            = clamp(1.0 - normalized_d, 0.0, 1.0);
				float falloff       = nc * sqrt(nc);
				float shadow = SampleDLightShadow(int(light_index), in_pos, l.origin, l.radius);
				vec3  light_contrib = attenuation * falloff * shadow * l.color * dynamic_light_noise;

				// FIX: Shadow-Term fuer dieses DLight berechnen.
				// War faelschlicherweise entfernt - keine DLight-Schatten in world.frag.

				dynamic_light      += light_contrib;
				if (rim_factor > 1e-5 && RimLightParams1.w > 0.0)
				{
					float rim_shadow = (RimLightParams0.w > 0.5) ? shadow : 1.0;
					vec3 light_dir = light_vec * inv_surface_dist;
					float backlight = mix(0.35, 1.0, max(dot(-surface_normal, light_dir), 0.0));
					rim_dlight_accum += (attenuation * falloff) * rim_shadow * l.color * dynamic_light_noise * backlight;
				}

				// Specular
				if (attenuation > 0.0 && falloff > 0.0)
				{
					vec3  light_dir = light_vec * inv_surface_dist;      // already normalised
					float ndotl     = max(dot(surface_normal, light_dir), 0.0);
					float light_weight = attenuation * falloff * shadow;
					ndotl_accum += ndotl * light_weight;
					ndotl_weight += light_weight;

					if (ndotl > 0.0)
					{
						vec3  half_vec = normalize(light_dir + view_dir); // OPT: single normalize
						float ndoth    = max(dot(surface_normal, half_vec), 0.0);
						// SPECULAR_POWER=16 -> pow(x,16) = ((x*x)*(x*x))*((x*x)*(x*x)) — two squarings
						float h2  = ndoth * ndoth;
						float h4  = h2 * h2;
						float h8  = h4 * h4;
						float h16 = h8 * h8;
						float spec = h16 * ndotl;
						specular_light += light_contrib * spec * 0.4; // SPECULAR_SCALE=0.4
					}
				}
			}

			// OPT: clamp(x, 0, 1-total_light) saturates without overflow
			total_light += max(min(dynamic_light, 1.0 - total_light), 0.0);
		}

		// Sun light
		if (SunDirEnabled.w > 0.5)
		{
			/* SunDirEnabled.xyz stores scene->sun direction. Surface lighting needs
			 * incoming-light direction (sun->scene), therefore we negate it. */
			vec3 sun_to_surface = -SunDirEnabled.xyz;
			float ndotl = max(dot(surface_normal, sun_to_surface), 0.0);
			if (ndotl > 0.0)
			{
				float sun_shadow = SampleSunShadow(in_pos);
				vec3 sun_contrib = SunColorIntensity.rgb * SunColorIntensity.a * ndotl * sun_shadow;
				total_light += max(min(sun_contrib, 1.0 - total_light), 0.0);
				if (rim_factor > 1e-5 && RimLightParams1.z > 0.0)
				{
					float rim_shadow = (RimLightParams0.w > 0.5) ? sun_shadow : 1.0;
					float sun_back = max(dot(-surface_normal, sun_to_surface), 0.0);
					rim_sun_accum += SunColorIntensity.rgb * SunColorIntensity.a
						* rim_shadow * mix(0.35, 1.0, sun_back);
				}
			}
			else if (rim_factor > 1e-5 && RimLightParams1.z > 0.0)
			{
				float rim_shadow = (RimLightParams0.w > 0.5) ? SampleSunShadow(in_pos) : 1.0;
				float sun_back = max(dot(-surface_normal, sun_to_surface), 0.0);
				rim_sun_accum += SunColorIntensity.rgb * SunColorIntensity.a
					* rim_shadow * mix(0.35, 1.0, sun_back);
			}
		}

		// Apply lighting
#if DITHER >= 2
		vec3 clamped_light = clamp(total_light, 0.0, 1.0);
		total_lightmap = clamp(floor(clamped_light * 63.0 + 0.5) * (Overbright / 63.0), 0.0, Overbright);
#else
		total_lightmap = clamp(total_light * Overbright, 0.0, Overbright);
#endif
	} // end lightmap block

	// Apply lightmap to albedo
#if MODE != 1
	// Fallback for bmodels without baked lightmaps: CPU fills in_bmodel_relight with
	// sampled static world lighting (0..1). This avoids black pickup boxes.
	if ((in_flags & CF_NOLIGHTMAP) != 0u && dot(in_bmodel_relight, in_bmodel_relight) > 1e-6)
		total_lightmap = clamp(in_bmodel_relight * Overbright, 0.0, Overbright);
	result.rgb = mix(result.rgb, result.rgb * total_lightmap, result.a);
#else
	if ((in_flags & CF_NOLIGHTMAP) != 0u && dot(in_bmodel_relight, in_bmodel_relight) > 1e-6)
		total_lightmap = clamp(in_bmodel_relight * Overbright, 0.0, Overbright);
	result.rgb *= total_lightmap;
#endif

	result.rgb += fullbright + emissive;

	// Add specular
	result.rgb += clamp(specular_light, vec3(0.0), vec3(Overbright)) * clamp(result.a, 0.0, 1.0);
	if (rim_factor > 1e-5)
	{
		vec3 rim_light = rim_sun_accum * RimLightParams1.z + rim_dlight_accum * RimLightParams1.w;
		result.rgb += result.rgb * rim_light * rim_factor;
	}
	result.rgb = SanitizeColor(result.rgb);
	vec3 pre_tonemap_hdr = result.rgb;

	// Tone mapping is handled in postprocess only.

	result.rgb  *= in_stage_color.rgb;
	result.a     = in_alpha * in_stage_color.a;
	result       = clamp(result, 0.0, 1.0);
	float fog_att = FogAttenuation(in_pos - EyePos);

	if (lighting_debug_view > 0)
	{
		vec3 debug_rgb = result.rgb;
		if (lighting_debug_view == 1)
		{
			debug_rgb = clamp(albedo, 0.0, 1.0);
		}
		else if (lighting_debug_view == 2)
		{
			vec3 light_vis = total_lightmap / max(Overbright, 1e-4);
			debug_rgb = clamp(light_vis, 0.0, 1.0);
		}
		else if (lighting_debug_view == 3)
		{
			float ndotl_vis = (ndotl_weight > 1e-5) ? (ndotl_accum / ndotl_weight) : 0.0;
			debug_rgb = vec3(clamp(ndotl_vis, 0.0, 1.0));
		}
		else if (lighting_debug_view == 4)
		{
			debug_rgb = surface_normal * 0.5 + 0.5;
		}
		else if (lighting_debug_view == 5)
		{
			debug_rgb = clamp(specular_light / max(Overbright, 1e-4), 0.0, 1.0);
		}
		else if (lighting_debug_view == 6)
		{
			debug_rgb = vec3(clamp(in_skyvisibility, 0.0, 1.0));
		}
		else if (lighting_debug_view == 7)
		{
			debug_rgb = vec3(clamp(1.0 - fog_att, 0.0, 1.0));
		}
		else if (lighting_debug_view == 8)
		{
			debug_rgb = clamp(log2(vec3(1.0) + max(pre_tonemap_hdr, vec3(0.0))) * 0.25, 0.0, 1.0);
		}

		result.rgb = debug_rgb;
		result.a = 1.0;
		OUT_COLOR = result;
#if !OIT
		out_velocity = vec4(0.0);
#endif
		return;
	}

	if (ShadowEnableDebug.w > 0.5)
	{
		int smode = int(ShadowEnableDebug.w + 0.5);
		if (smode == 1)
			result.rgb = vec3(SampleSunShadow(in_pos));
		else if (smode == 2)
			result.rgb = vec3(ComputeCombinedDLightShadow(in_pos));
		else if (smode == 3)
			result.rgb = vec3(SampleSunShadowDepth(in_pos));
		else if (smode == 4)
			result.rgb = vec3(SampleFirstDLightDepth(in_pos));
		else if (smode == 5)
		{
			int ci = ShadowCascadeForWorldPos(in_pos);
			const vec3 cascade_colors[4] = vec3[4](
				vec3(1.0, 0.25, 0.25),
				vec3(0.25, 1.0, 0.25),
				vec3(0.25, 0.5, 1.0),
				vec3(1.0, 0.85, 0.25));
			result.rgb = cascade_colors[clamp(ci, 0, 3)];
		}
	}

	result.rgb   = mix(Fog.rgb, result.rgb, fog_att);
	result.rgb   = SanitizeColor(result.rgb);

	// BUG FIX: original wrote "out_fragcolor" directly but OUT_COLOR must be used
	// to stay compatible with both OIT and non-OIT paths.
	OUT_COLOR = result;

#if !OIT
	vec2 velocity    = ComputeVelocity(in_curr_clip, in_prev_clip);
	// BUG FIX: original condition (result.a >= 0.999) wrote velocity * result.a, but
	// result.a is already >=0.999 ~= 1; just write velocity directly to avoid
	// subtle sub-pixel ghosting on nearly-opaque geometry.
	vec2 velocityOut = (result.a >= 0.999) ? velocity : vec2(0.0);
	float materialMask = ((in_flags & CF_MAT_BLOOM)    != 0u) ? 1.0 : 0.0;
	if  ((in_flags & CF_MAT_EMISSIVE) != 0u) materialMask += 4.0;
	if  ((in_flags & CF_MAT_TRANS)    != 0u) materialMask += 2.0;
	out_velocity = vec4(velocityOut, 0.0, materialMask);
#endif

	// Dithering
#if DITHER == 1
	vec3  dpos    = fwidth(in_pos);
	float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);
	farblend *= farblend;
	// OPT: sqrt then square — work in sqrt-luminance space for perceptual dither
	OUT_COLOR.rgb = sqrt(OUT_COLOR.rgb);
	float luma     = dot(OUT_COLOR.rgb, vec3(0.25, 0.625, 0.125));
	float nearnoise = tri(whitenoise01(lmuv * lmsize)) * luma * TextureDither;
	float farnoise  = (abs(Fog.w) > 0.0) ? SCREEN_SPACE_NOISE() * ScreenDither : 0.0;
	OUT_COLOR.rgb  += mix(nearnoise, farnoise, farblend);
	OUT_COLOR.rgb  *= OUT_COLOR.rgb;
#endif

#if DITHER >= 2
	OUT_COLOR.rgb = floor(OUT_COLOR.rgb * 255.0 + 0.5) * (1.0/255.0);
#elif DITHER == 0
	OUT_COLOR.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}
