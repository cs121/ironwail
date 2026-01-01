#if BINDLESS
	#extension GL_ARB_bindless_texture : require
#else
	layout(binding=0) uniform sampler2D Tex;
	layout(binding=1) uniform sampler2D FullbrightTex;
	layout(binding=4) uniform sampler2D EmissiveTex;
#endif
layout(binding=2) uniform sampler2D LMTex;
layout(binding=3) uniform sampler2D LMTexDir;
layout(binding=5) uniform sampler2DShadow ShadowMap;
layout(binding=6) uniform sampler2D ShadowMapDepth;
#include "frame_uniforms.glsl"
#define SHADOW_SUN 1
#include "shadow_sample.glsl"

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

float GetLightStyle(int index)
{
	return (index < 64) ? mix(LightStyles[index].x, LightStyles[index].y, LightmapParams.w) : 1.0;
}

layout(rg32ui, binding=0) uniform readonly uimage3D LightClusters;

struct Call
{
	uint	flags;
	float	wateralpha;
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
	CF_USE_FULLBRIGHT = 2u,
	CF_NOLIGHTMAP = 4u,
	CF_USE_EMISSIVE = 8u,
	CF_ALPHA_TEST = 16u,
	CF_MAT_BLOOM = 128u,
	CF_MAT_EMISSIVE = 256u,
	CF_MAT_GODRAY = 512u,
	CF_MAT_TRANS = 1024u,
	CF_MAT_SKY = 2048u,
	CF_MAT_HAS_SHADER = 4096u,
	CF_MAT_NO_SHADOW_RECEIVE = 8192u;

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
layout(location=0) flat in uint in_flags;
layout(location=1) flat in float in_alpha;
layout(location=2) in vec3 in_pos;
#if MODE == 1
	layout(location=3) centroid in vec2 in_uv;
#else
	layout(location=3) in vec2 in_uv;
#endif
layout(location=4) centroid in vec2 in_lmuv;
layout(location=5) in float in_depth;
layout(location=6) noperspective in vec2 in_coord;
layout(location=7) flat in vec4 in_styles;
layout(location=8) flat in float in_lmofs;
#if BINDLESS
	layout(location=9) flat in uvec4 in_samplers0;
	layout(location=10) flat in uvec2 in_samplers1;
#endif
layout(location=11) noperspective in vec4 in_curr_clip;
layout(location=12) noperspective in vec4 in_prev_clip;
layout(location=13) in vec3 in_normal;
layout(location=14) in vec3 in_lightgrid;
layout(location=15) flat in vec4 in_stage_color;

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

// Convert uniform to triangle distribution
float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.0;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

vec4 SampleLightmap(vec2 uv)
{
	vec4 lm = texture(LMTex, uv);
	return lm;
}

vec3 SampleLightmapDir(vec2 uv)
{
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

vec3 ComputeSunLight(vec3 world_pos, vec3 normal)
{
	return vec3(0.0);
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
		out_accum = vec4(color.rgb * color.a * weight, color.a * weight);
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
	vec3 emissive = vec3(0.0);
	vec2 uv = in_uv;
	
#if MODE == 2
	uv = uv * 2.0 + 0.125 * sin(uv.yx * (3.14159265 * 2.0) + Time);
#endif

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

#if MODE == 1
	if (result.a < 0.666)
		discard;
#endif

        int debug_mode = int(ColorSpaceParams.x + 0.5);
        if (debug_mode == 1)
        {
                out_fragcolor = vec4(result.rgb, 1.0);
#if !OIT
                out_velocity = vec4(0.0);
#endif
                return;
        }

        int shader_debug = int(ShaderParams.x + 0.5);
        if (shader_debug >= 2)
        {
                vec3 debug_color = vec3(0.0);
                if ((in_flags & CF_MAT_BLOOM) != 0u)
                        debug_color += vec3(1.0, 0.0, 1.0);
                if ((in_flags & CF_MAT_EMISSIVE) != 0u)
                        debug_color += vec3(1.0, 1.0, 0.0);
                if ((in_flags & CF_MAT_GODRAY) != 0u)
                        debug_color += vec3(0.0, 1.0, 1.0);
                if ((in_flags & CF_MAT_TRANS) != 0u)
                        debug_color += vec3(0.0, 1.0, 0.0);
                if ((in_flags & CF_MAT_SKY) != 0u)
                        debug_color += vec3(0.0, 0.0, 1.0);
                if (all(lessThanEqual(debug_color, vec3(0.0))))
                        debug_color = result.rgb;
                out_fragcolor = vec4(clamp(debug_color, 0.0, 1.0), 1.0);
#if !OIT
                out_velocity = vec4(0.0);
#endif
                return;
        }

        bool additive_dlights = DLightParams.x > 0.5;
        bool dlight_debug = DLightParams.y > 0.5;

	// Lightmap sampling
	vec2 lmuv = in_lmuv;
	vec3 total_lightmap = vec3(1.0);
	vec3 specular_light = vec3(0.0);
#if DITHER
	vec2 lmsize = vec2(textureSize(LMTex, 0).xy) * 16.0;
#endif

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
				static_light = vec3(
					dot(in_styles, lm0),
					dot(in_styles, lm1),
					dot(in_styles, lm2)
				);
			}
		}

		vec3 lightgrid = mix(vec3(1.0), in_lightgrid, LightgridParams.x);

		if (LightgridParams.y > 0.5)
		{
			OUT_COLOR = vec4(lightgrid, 1.0);
#if !OIT
			out_velocity = vec4(0.0);
#endif
			return;
		}

		if (dlight_debug)
		{
			static_light = vec3(0.0);
			fullbright = vec3(0.0);
			emissive = vec3(0.0);
		}

		// Directional lightmap
		if (LightmapParams.z > 0.5)
		{
			vec3 dir = SampleLightmapDir(lmuv);
			float ndl = max(dot(dir, vec3(0.0, 0.0, 1.0)), 0.0);
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

		// Surface normal computation
		vec3 surface_normal = in_normal;
		float surface_normal_len = length(surface_normal);

		if (surface_normal_len > 0.0)
		{
			surface_normal /= surface_normal_len;
		}
		else
		{
			vec3 surface_normal_vec = cross(dFdx(in_pos), dFdy(in_pos));
			float geom_len = length(surface_normal_vec);
			surface_normal = (geom_len > 0.0) ? (surface_normal_vec / geom_len) : vec3(0.0, 0.0, 1.0);
		}

		if (!gl_FrontFacing)
			surface_normal = -surface_normal;

		bool shadow_enabled = ShadowDebug.x > 0.5;
		int shadow_mode = int(ShadowDebug.y + 0.5);
		float shadow_range = 1.0;
		float shadow_term = 1.0;
		bool shadow_receiver = (in_flags & CF_MAT_SKY) == 0u;
		shadow_receiver = shadow_receiver && (in_flags & CF_MAT_EMISSIVE) == 0u;
		shadow_receiver = shadow_receiver && (in_flags & CF_USE_EMISSIVE) == 0u;
		shadow_receiver = shadow_receiver && (in_flags & CF_MAT_NO_SHADOW_RECEIVE) == 0u;
		if (shadow_enabled && shadow_receiver && (shadow_mode == 0 || shadow_mode == 4))
			shadow_term = ShadowVisibility(in_pos, surface_normal, shadow_range);
		bool lightgrid_shadow = LightgridParams.z > 0.5 && LightgridParams.x > 0.5;

		if (shadow_enabled && shadow_mode >= 1)
		{
			out_fragcolor = vec4(ShadowDebugColor(in_pos, shadow_term), 1.0);
#if !OIT
			out_velocity = vec4(0.0);
#endif
			return;
		}

		vec3 total_light;
		vec3 clamped_static = clamp(static_light, 0.0, 1.0);
		vec3 ambient_term = vec3(0.0);
		vec3 direct_term = vec3(0.0);

		if (lightgrid_shadow)
		{
			ambient_term = clamped_static * lightgrid;
			direct_term = clamped_static - ambient_term;
			float shadow_scale = shadow_enabled ? shadow_term : 1.0;
			total_light = ambient_term + direct_term * shadow_scale;
		}
		else
		{
			ambient_term = vec3(0.0);
			direct_term = clamped_static;
			float shadow_scale = shadow_enabled ? shadow_term : 1.0;
			total_light = ambient_term + direct_term * shadow_scale;
			total_light *= lightgrid;
		}
	
	// View direction
	vec3 to_eye = EyePos - in_pos;
	float view_length = length(to_eye);
	vec3 view_dir = (view_length > 0.0) ? (to_eye / view_length) : vec3(0.0, 0.0, 1.0);

	const float SPECULAR_POWER = 16.0;
	const float SPECULAR_SCALE = 0.4;

        // Dynamic lights (clustered lighting)
        if (!additive_dlights && NumLights > 0u)
        {
                ivec3 cluster_coord = ivec3(
                        int(floor(in_coord.x)),
			int(floor(in_coord.y)),
			int(floor(log2(in_depth) * ZLogScale + ZLogBias))
		);
		
		uvec2 clusterdata = imageLoad(LightClusters, cluster_coord).xy;
		
		if ((clusterdata.x | clusterdata.y) != 0u)
		{
			vec3 dynamic_light = vec3(0.0);
			float dynamic_light_noise = 1.0 - whitenoise01(in_pos.xy) * 0.15;
			vec4 plane = vec4(surface_normal, dot(in_pos, surface_normal));
			
			for (uint i = 0u, ofs = 0u; i < 2u; i++, ofs += 32u)
			{
				uint mask = clusterdata[i];
				while (mask != 0u)
				{
					int j = findLSB(mask);
					mask ^= 1u << j;
					Light l = Lights[ofs + uint(j)];
					
					// Light culling
					float rad = l.radius;
					float dist = dot(l.origin, plane.xyz) - plane.w;
					rad -= abs(dist);
					float minlight = l.minlight;
					
					if (rad <= 0.0 || rad < minlight)
						continue;
					
					vec3 local_pos = l.origin - plane.xyz * dist;
					minlight = rad - minlight;
					vec3 light_vec = local_pos - in_pos;
					float surface_dist = length(light_vec);
					float attenuation = clamp((minlight - surface_dist) / 16.0, 0.0, 1.0);
					float normalized_dist = surface_dist / rad;
					float falloff = pow(1.0 - clamp(normalized_dist, 0.0, 1.0), 1.5);
					vec3 light_contrib = attenuation * falloff * l.color * dynamic_light_noise;
					dynamic_light += light_contrib;
					
					// Specular calculation
					if (attenuation > 0.0 && falloff > 0.0 && surface_dist > 0.0)
					{
						vec3 light_dir = light_vec / surface_dist;
						float ndotl = max(dot(surface_normal, light_dir), 0.0);
						
						if (ndotl > 0.0)
						{
							vec3 half_vec = light_dir + view_dir;
							float half_len = length(half_vec);
							
							if (half_len > 0.0)
							{
								half_vec /= half_len;
								float ndoth = max(dot(surface_normal, half_vec), 0.0);
								float spec = pow(ndoth, SPECULAR_POWER) * ndotl;
								specular_light += light_contrib * spec * SPECULAR_SCALE;
							}
						}
					}
				}
			}
			total_light += max(min(dynamic_light, 1.0 - total_light), 0.0);
		}
	}

	// Sun light
        vec3 sun_light = dlight_debug ? vec3(0.0) : ComputeSunLight(in_pos, surface_normal);
	total_light += max(min(sun_light, 1.0 - total_light), 0.0);

		// Apply lighting
#if DITHER >= 2
		vec3 clamped_light = clamp(total_light, 0.0, 1.0);
		total_lightmap = clamp(floor(clamped_light * 63.0 + 0.5) * (Overbright / 63.0), 0.0, Overbright);
#else
		total_lightmap = clamp(total_light * Overbright, 0.0, Overbright);
#endif
	}

#if MODE != 1
	result.rgb = mix(result.rgb, result.rgb * total_lightmap, result.a);
#else
	result.rgb *= total_lightmap;
#endif

	result.rgb += fullbright + emissive;
	
	// Add specular
	vec3 spec_clamped = clamp(specular_light, vec3(0.0), vec3(Overbright));
	result.rgb += spec_clamped * clamp(result.a, 0.0, 1.0);
	
	// Tone mapping
	if (LightmapParams.y > 0.5)
		result.rgb = result.rgb / (vec3(1.0) + result.rgb);
	
	result.rgb *= in_stage_color.rgb;
	result.a = in_alpha * in_stage_color.a;
	result = clamp(result, 0.0, 1.0);
	result.rgb = ApplyFog(result.rgb, in_pos - EyePos);

	out_fragcolor = result;

#if !OIT
	vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
	vec2 velocityOut = (result.a >= 0.999) ? (velocity * result.a) : vec2(0.0);
	float materialMask = ((in_flags & CF_MAT_BLOOM) != 0u) ? 1.0 : 0.0;
	if ((in_flags & CF_MAT_EMISSIVE) != 0u)
		materialMask += 4.0;
	if ((in_flags & CF_MAT_TRANS) != 0u)
		materialMask += 2.0;
	out_velocity = vec4(velocityOut, 0.0, materialMask);
#endif

	// Dithering
#if DITHER == 1
	vec3 dpos = fwidth(in_pos);
	float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);
	farblend *= farblend;
	out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
	float luma = dot(out_fragcolor.rgb, vec3(0.25, 0.625, 0.125));
	float nearnoise = tri(whitenoise01(lmuv * lmsize)) * luma * TextureDither;
	float farnoise = (Fog.w > 0.0) ? SCREEN_SPACE_NOISE() * ScreenDither : 0.0;
	out_fragcolor.rgb += mix(nearnoise, farnoise, farblend);
	out_fragcolor.rgb *= out_fragcolor.rgb;
#endif

#if DITHER >= 2
	out_fragcolor.rgb = floor(out_fragcolor.rgb * 255.0 + 0.5) * (1.0/255.0);
#elif DITHER == 0
	out_fragcolor.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}
