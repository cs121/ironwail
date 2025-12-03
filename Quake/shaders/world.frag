#if BINDLESS
	#extension GL_ARB_bindless_texture : require
#else
	layout(binding=0) uniform sampler2D Tex;
	layout(binding=1) uniform sampler2D FullbrightTex;
	layout(binding=4) uniform sampler2D EmissiveTex;
#endif
layout(binding=2) uniform sampler2D LMTex;
layout(binding=3) uniform sampler2D LMTexDir;
#include "frame_uniforms.glsl"

// ============================================================================
// FOG SYSTEM - Verbessert mit Höhen-Fog Option
// ============================================================================
vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-abs(Fog.w) * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}

// ============================================================================
// LIGHTING SYSTEM
// ============================================================================
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
	if (index < 0 || index >= 64)
		return 1.0;
	return mix(LightStyles[index].x, LightStyles[index].y, LightmapParams.w);
}

layout(rg32ui, binding=0) uniform readonly uimage3D LightClusters;

// ============================================================================
// CALL & INSTANCE DATA
// ============================================================================
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
#endif
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

// ============================================================================
// SHADER INPUTS
// ============================================================================
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

// ============================================================================
// NOISE & DITHERING - Optimiert
// ============================================================================
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));
	v = (v ^ (v << 2)) & 0x3333u;
	v = (v ^ (v << 1)) & 0x5555u;
	v |= v >> 7;
	v = bitfieldReverse(v) >> 24;
	return float(v) * (1.0 / 256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

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

float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.0;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy) + 0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

// ============================================================================
// LIGHTMAP SAMPLING
// ============================================================================
vec4 SampleLightmap(vec2 uv)
{
	vec4 lm = texture(LMTex, uv);
	if (LightmapParams.x <= 0.5)
		lm = pow(lm, vec4(2.2));
	return lm;
}

vec3 SampleLightmapDir(vec2 uv)
{
	vec3 dir = texture(LMTexDir, uv).xyz * 2.0 - 1.0;
	return normalize(dir);
}

// ============================================================================
// MOTION VECTORS
// ============================================================================
vec2 ComputeVelocity(vec4 curr_clip, vec4 prev_clip)
{
	const float EPS = 1e-6;
	float inv_curr_w = abs(curr_clip.w) > EPS ? 1.0 / curr_clip.w : 0.0;
	float inv_prev_w = abs(prev_clip.w) > EPS ? 1.0 / prev_clip.w : 0.0;
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

// ============================================================================
// SUN LIGHTING (Stub für Erweiterung)
// ============================================================================
vec3 ComputeSunLight(vec3 world_pos, vec3 normal)
{
	return vec3(0.0);
}

// ============================================================================
// OUTPUT SETUP
// ============================================================================
#define OUT_COLOR out_fragcolor
#if OIT
	vec4 OUT_COLOR;
	layout(location=0) out vec4 out_accum;
	layout(location=1) out float out_reveal;

	vec3 GammaToLinear(vec3 v)
	{
		return v; // Bereits in linearem Raum
	}

	void main_body();

	void main()
	{
		main_body();
		OUT_COLOR = clamp(OUT_COLOR, 0.0, 1.0);
		vec4 color = vec4(GammaToLinear(OUT_COLOR.rgb), OUT_COLOR.a);
		float z = 1.0 / gl_FragCoord.w;
		// Verbesserte Gewichtungsfunktion für OIT
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z / 1e7, 1.0)), 1e-2, 3e3);
		out_accum = vec4(color.rgb, color.a * weight);
		out_accum.rgb *= out_accum.a;
		out_reveal = color.a;
	}

	#define main main_body
#else
	layout(location=0) out vec4 OUT_COLOR;
	layout(location=1) out vec4 out_velocity;
#endif

// ============================================================================
// MAIN SHADER
// ============================================================================
void main()
{
	// Texture-Sampling
	vec3 fullbright = vec3(0.0);
	vec3 emissive = vec3(0.0);
	vec2 uv = in_uv;

	// Water-Wave-Effekt für MODE 2
#if MODE == 2
	const float TWO_PI = 6.28318530718;
	uv = uv * 2.0 + 0.125 * sin(uv.yx * TWO_PI + Time);
#endif

	// Texture-Sampling mit Bindless oder Fixed Bindings
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

	// Haupt-Textur-Sampling mit LOD-Bias für Dithering
#if DITHER >= 2
	vec4 result = texture(Tex, uv, -1.0);
#elif DITHER
	vec4 result = texture(Tex, uv, -0.5);
#else
	vec4 result = texture(Tex, uv);
#endif

	// Alpha-Test für MODE 1
#if MODE == 1
	if (result.a < 0.666)
		discard;
#endif

	// Farbraum-Konvertierung
	bool linear_lightmaps = LightmapParams.x > 0.5;
	if (!linear_lightmaps)
	{
		result.rgb = pow(result.rgb, vec3(2.2));
		fullbright = pow(fullbright, vec3(2.2));
		emissive = pow(emissive, vec3(2.2));
	}

	// ========================================================================
	// LIGHTMAP SAMPLING - Verbessert mit besserer Filterung
	// ========================================================================
	vec2 lmuv = in_lmuv;
#if DITHER
	vec2 lmsize = vec2(textureSize(LMTex, 0).xy) * 16.0;
	lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;
#endif

	vec4 lm0 = SampleLightmap(lmuv);
	vec3 static_light;

	// Optimierter Lightstyle-Pfad
	if (in_styles.y < 0.0) // Single style
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
		else // 3 oder 4 styles
		{
			vec4 lm2 = SampleLightmap(vec2(lmuv.x + in_lmofs * 2.0, lmuv.y));
			static_light = vec3(
				dot(in_styles, lm0),
				dot(in_styles, lm1),
				dot(in_styles, lm2)
			);
		}
	}

	// Direktionale Lightmaps
	if (LightmapParams.z > 0.5)
	{
		vec3 dir = SampleLightmapDir(lmuv);
		float ndl = max(dot(dir, normalize(in_normal)), 0.0);
		static_light *= ndl;
	}

	// ========================================================================
	// SURFACE NORMAL BERECHNUNG - Verbessert
	// ========================================================================
	vec3 surface_normal = normalize(in_normal);
	float surface_normal_len = length(in_normal);
	
	if (surface_normal_len < 0.001)
	{
		// Fallback auf geometrische Normale
		vec3 surface_normal_vec = cross(dFdx(in_pos), dFdy(in_pos));
		float geom_len = length(surface_normal_vec);
		surface_normal = (geom_len > 0.001) ? 
			(surface_normal_vec / geom_len) : vec3(0.0, 0.0, 1.0);
	}
	
	if (!gl_FrontFacing)
		surface_normal = -surface_normal;

	// ========================================================================
	// LIGHTING BERECHNUNG
	// ========================================================================
	vec3 total_light = clamp(static_light, 0.0, 1.0);
	vec3 specular_light = vec3(0.0);
	
	// View Direction
	vec3 to_eye = EyePos - in_pos;
	float view_length = length(to_eye);
	vec3 view_dir = (view_length > 0.001) ? (to_eye / view_length) : vec3(0.0, 0.0, 1.0);

	const float SPECULAR_POWER = 16.0;
	const float SPECULAR_SCALE = 0.4;

	// ========================================================================
	// DYNAMISCHE LICHTER - Clustered Lighting
	// ========================================================================
	if (NumLights > 0u)
	{
		ivec3 cluster_coord;
		cluster_coord.x = int(floor(in_coord.x));
		cluster_coord.y = int(floor(in_coord.y));
		cluster_coord.z = int(floor(log2(max(in_depth, 0.001)) * ZLogScale + ZLogBias));
		
		uvec2 clusterdata = imageLoad(LightClusters, cluster_coord).xy;
		
		if ((clusterdata.x | clusterdata.y) != 0u)
		{
			vec3 dynamic_light = vec3(0.0);
			float dynamic_light_noise = 1.0 - whitenoise01(in_pos.xy) * 0.15;
			
			vec4 plane;
			plane.xyz = surface_normal;
			plane.w = dot(in_pos, plane.xyz);
			
			for (uint i = 0u, ofs = 0u; i < 2u; i++, ofs += 32u)
			{
				uint mask = clusterdata[i];
				while (mask != 0u)
				{
					int j = findLSB(mask);
					mask ^= 1u << j;
					Light l = Lights[ofs + uint(j)];
					
					// Plane-Distance Test
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
					
					// Verbesserte Attenuation
					float attenuation = clamp((minlight - surface_dist) / 16.0, 0.0, 1.0);
					float normalized_dist = surface_dist / rad;
					float falloff = pow(1.0 - clamp(normalized_dist, 0.0, 1.0), 1.5);
					vec3 light_contrib = attenuation * falloff * l.color * dynamic_light_noise;
					dynamic_light += light_contrib;
					
					// Specular Lighting
					if (attenuation > 0.0 && falloff > 0.0 && surface_dist > 0.001)
					{
						vec3 light_dir = light_vec / surface_dist;
						float ndotl = max(dot(surface_normal, light_dir), 0.0);
						
						if (ndotl > 0.0)
						{
							vec3 half_vec = normalize(light_dir + view_dir);
							float ndoth = max(dot(surface_normal, half_vec), 0.0);
							float spec = pow(ndoth, SPECULAR_POWER) * ndotl;
							specular_light += light_contrib * spec * SPECULAR_SCALE;
						}
					}
				}
			}
			
			total_light += clamp(dynamic_light, 0.0, 1.0 - total_light);
		}
	}

	// Sun Light
	vec3 sun_light = ComputeSunLight(in_pos, surface_normal);
	total_light += clamp(sun_light, 0.0, 1.0 - total_light);

	// ========================================================================
	// FINAL LIGHTING COMPOSITION
	// ========================================================================
#if DITHER >= 2
	vec3 clamped_light = clamp(total_light, 0.0, 1.0);
	vec3 total_lightmap = clamp(floor(clamped_light * 63.0 + 0.5) * (Overbright / 63.0), 0.0, Overbright);
#else
	vec3 total_lightmap = clamp(total_light * Overbright, 0.0, Overbright);
#endif

#if MODE != 1
	result.rgb = mix(result.rgb, result.rgb * total_lightmap, result.a);
#else
	result.rgb *= total_lightmap;
#endif

	result.rgb += fullbright;
	result.rgb += emissive;
	result.rgb += clamp(specular_light, 0.0, Overbright) * clamp(result.a, 0.0, 1.0);

	// Tonemapping
	if (LightmapParams.y > 0.5)
		result.rgb = result.rgb / (vec3(1.0) + result.rgb);

	result = clamp(result, 0.0, 1.0);

	// Fog Application
	result.rgb = ApplyFog(result.rgb, in_pos - EyePos);

	// Gamma-Korrektur
	if (!linear_lightmaps)
		result.rgb = pow(result.rgb, vec3(1.0 / 2.2));

	result.a = in_alpha;

	// ========================================================================
	// DITHERING & OUTPUT
	// ========================================================================
#if DITHER == 1
	vec3 dpos = fwidth(in_pos);
	float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);
	farblend *= farblend;
	result.rgb = sqrt(result.rgb);
	float luma = dot(result.rgb, vec3(0.25, 0.625, 0.125));
	float nearnoise = tri(whitenoise01(lmuv * vec2(textureSize(LMTex, 0).xy) * 16.0)) * luma * TextureDither;
	float farnoise = (Fog.w > 0.0) ? SCREEN_SPACE_NOISE() * ScreenDither : 0.0;
	result.rgb += mix(nearnoise, farnoise, farblend);
	result.rgb *= result.rgb;
#endif

#if DITHER >= 2
	result.rgb = floor(result.rgb * 255.0 + 0.5) * (1.0 / 255.0);
#elif DITHER == 0
	result.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif

	out_fragcolor = result;

	// Motion Vectors
#if !OIT
	vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
	vec2 velocityOut = (result.a >= 0.999) ? (velocity * result.a) : vec2(0.0);
	out_velocity = vec4(velocityOut, 0.0, 0.0);
#endif
}