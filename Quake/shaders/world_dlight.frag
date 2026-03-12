#if BINDLESS
#extension GL_ARB_bindless_texture : require
#else
layout(binding=0) uniform sampler2D Tex;
#endif
layout(binding=2) uniform sampler2D LMTex; // unused, kept for binding slot consistency
layout(binding=3) uniform sampler2D LMTexDir; // unused
layout(binding=7) uniform sampler2D SunShadowTex; // reserved for interface parity
layout(binding=8) uniform samplerCubeArray DLightShadowTex;

#include "frame_uniforms.glsl"

#ifndef ALPHATEST
#define ALPHATEST 0
#endif

layout(location=0) uniform float DLightScale;

layout(location=30) uniform mat4 ShadowSunViewProj;
layout(location=34) uniform vec4 ShadowEnableDebug;   // x=enabled, y=sun, z=dlight, w=debug mode
layout(location=35) uniform vec4 ShadowDLightIndices; // selected light indices (float encoded ints)
layout(location=36) uniform vec4 ShadowBiasCounts;    // x=num dlight slots, y=sun bias, z=dlight bias
layout(location=37) uniform vec4 ShadowPCFTexel;      // x=sun pcf radius, y=dlight pcf radius, z=1/sun size, w=1/dlight size
layout(location=45) uniform vec4 RimLightParams0; // x=enable, y=intensity, z=power, w=shadowed
layout(location=46) uniform vec4 RimLightParams1; // x=world_enable, y=model_enable, z=sun_scale, w=dlight_scale

float FogAttenuation(vec3 p)
{
	float fog = exp2(-abs(Fog.w) * dot(p, p));
	return clamp(fog, 0.0, 1.0);
}

#define MAX_LIGHTS    64
#define LIGHT_TILES_X 32
#define LIGHT_TILES_Y 16
#define LIGHT_TILES_Z 32

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

layout(location=0) flat in uint in_flags;
layout(location=1) flat in float in_alpha;
layout(location=2) in vec3 in_pos;
layout(location=3) in vec2 in_uv;
layout(location=4) in float in_depth;
layout(location=5) noperspective in vec2 in_coord;
layout(location=6) in vec3 in_normal;
#if BINDLESS
layout(location=7) flat in uvec4 in_samplers0;
layout(location=8) flat in uvec2 in_samplers1;
#endif

layout(location=0) out vec4 out_fragcolor;
layout(location=1) out vec4 out_velocity;

float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float ComputeFalloff(float x)
{
	return x * x * (3.0 - 2.0 * x);
}

uvec3 ComputeLightTileCoord(vec2 tile_coord, float view_depth)
{
	uint tx = uint(clamp(floor(tile_coord.x), 0.0, float(LIGHT_TILES_X - 1)));
	uint ty = uint(clamp(floor(tile_coord.y), 0.0, float(LIGHT_TILES_Y - 1)));
	float z = max(view_depth, 1e-6);
	float tzf = clamp(log2(z) * ZLogScale + ZLogBias, 0.0, float(LIGHT_TILES_Z - 1));
	return uvec3(tx, ty, uint(tzf));
}

int ShadowSlotForLight(int lightIndex)
{
	if (abs(ShadowDLightIndices.x - float(lightIndex)) < 0.5) return 0;
	if (abs(ShadowDLightIndices.y - float(lightIndex)) < 0.5) return 1;
	if (abs(ShadowDLightIndices.z - float(lightIndex)) < 0.5) return 2;
	if (abs(ShadowDLightIndices.w - float(lightIndex)) < 0.5) return 3;
	return -1;
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
	float ref = dist / radius;
	float bias = max(ShadowBiasCounts.z, 0.0);
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

void main()
{
	vec2 uv = in_uv;
#if BINDLESS
	sampler2D baseSampler = sampler2D(in_samplers0.xy);
	vec4 texel = texture(baseSampler, uv);
#else
	vec4 texel = texture(Tex, uv);
#endif

#if ALPHATEST
	if (texel.a < 0.666)
		discard;
#endif

	float alpha = texel.a * in_alpha;
	vec3 albedo = texel.rgb;
	int debug_mode = int(ColorSpaceParams.x + 0.5);
	if (debug_mode == 1)
	{
		out_fragcolor = vec4(albedo, 1.0);
		out_velocity = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	vec3 surface_normal = normalize(in_normal);
	if (!gl_FrontFacing)
		surface_normal = -surface_normal;
	vec3 to_eye = EyePos - in_pos;
	float to_eye_len_sq = dot(to_eye, to_eye);
	vec3 view_dir = (to_eye_len_sq > 1e-8) ? (to_eye * inversesqrt(to_eye_len_sq)) : vec3(0.0, 0.0, 1.0);
	float rim_factor = 0.0;
	if (RimLightParams0.x > 0.5 && RimLightParams1.x > 0.5)
	{
		float rim_ndotv = 1.0 - clamp(dot(surface_normal, view_dir), 0.0, 1.0);
		rim_factor = pow(max(rim_ndotv, 0.0), max(RimLightParams0.z, 0.5)) * max(RimLightParams0.y, 0.0);
	}
	uvec3 _tile_coord = ComputeLightTileCoord(in_coord, max(in_depth, 1e-6));

	vec3 dynamic_light = vec3(0.0);
	vec3 rim_dlight_accum = vec3(0.0);
	if (NumLights > 0u)
	{
		float dynamic_light_noise = 1.0 - whitenoise01(in_pos.xy) * 0.15;
		const float core_boost = 0.75;
		const float core_exp = 6.0;
		const float knee = 1.5;
		const float ndotl_mix = 0.2;
		const float saturation_chop = 0.1;

		for (uint light_index = 0u; light_index < NumLights; light_index++)
		{
			Light l = Lights[light_index];

			float rad = l.radius;
			vec3 to_light = l.origin - in_pos;
			float rad_sq = rad * rad;
			float dist_sq = dot(to_light, to_light);
			if (dist_sq >= rad_sq)
				continue;
			float surface_dist = sqrt(max(dist_sq, 1e-12));
			float minlight = l.minlight;
			if (rad - surface_dist < minlight)
				continue;

			float normalized_dist = surface_dist / max(rad, 1e-4);
			float x = clamp(1.0 - normalized_dist, 0.0, 1.0);
			float falloff = ComputeFalloff(x);
			float minlight_norm = minlight / max(rad, 1e-4);
			float attenuation = clamp((x - minlight_norm) / max(1.0 - minlight_norm, 1e-4), 0.0, 1.0);
			float intensity = attenuation * falloff;
			float core = 1.0 + core_boost * pow(max(x, 0.0), core_exp);
			float core_intensity = intensity * core;
			float shaped = (knee > 0.0) ? (core_intensity / (core_intensity + knee)) : core_intensity;
			float ndotl = 1.0;
			if (surface_dist > 0.0)
			{
				vec3 light_dir = to_light / surface_dist;
				float ndotl_raw = max(dot(surface_normal, light_dir), 0.0);
				ndotl = mix(1.0, ndotl_raw, ndotl_mix);
			}

			float shadow = SampleDLightShadow(int(light_index), in_pos, l.origin, rad);
			vec3 light_contrib = shaped * ndotl * shadow * l.color * dynamic_light_noise;
			dynamic_light += light_contrib;
			if (rim_factor > 1e-5 && RimLightParams1.w > 0.0)
			{
				float rim_shadow = (RimLightParams0.w > 0.5) ? shadow : 1.0;
				vec3 rim_light_dir = (surface_dist > 0.0) ? (to_light / surface_dist) : vec3(0.0, 0.0, 1.0);
				float backlight = mix(0.35, 1.0, max(dot(-surface_normal, rim_light_dir), 0.0));
				rim_dlight_accum += shaped * rim_shadow * l.color * dynamic_light_noise * backlight;
			}
		}

		if (saturation_chop > 0.0)
		{
			float luma = dot(dynamic_light, vec3(0.299, 0.587, 0.114));
			dynamic_light = mix(dynamic_light, vec3(luma), saturation_chop);
		}
	}

	vec3 contrib = clamp(dynamic_light * DLightScale * Overbright, 0.0, Overbright);
	vec3 color = albedo * contrib;
	if (rim_factor > 1e-5 && RimLightParams1.w > 0.0)
	{
		vec3 rim_light = rim_dlight_accum * RimLightParams1.w;
		color += albedo * rim_light * rim_factor;
	}

	if (ShadowEnableDebug.w > 1.5)
		color = vec3(clamp(length(dynamic_light), 0.0, 1.0));

	color *= FogAttenuation(in_pos - EyePos);

	out_fragcolor = vec4(color, alpha); // FIX: war "alpha * 0.0" → Fragment immer transparent
	out_velocity = vec4(0.0, 0.0, 0.0, 1.0);
}
