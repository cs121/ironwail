struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	NormalMatrix[3];
	vec4	LightColor; // xyz=LightColor w=Alpha
	vec4	DLightColor; // xyz=DLightColor
	vec4	DLightDir;   // xyz=dominant dlight direction
	vec4	StaticLightDir; // xyz=dominant static light direction
	float	SkyVisibility;
	vec3	_PadSky;
	int		Pose1;
	int		Pose2;
	float	Blend;
	int		Flags;
};

layout(std430, binding=1) restrict readonly buffer AliasFrameBlock
{
	mat4	ViewProj;
	mat4	PrevViewProj;
	vec3	EyePos;
	float	_Pad0;
	vec4	Fog;
	float	ScreenDither;
	float	Overbright;
	float	ModelHalfLambert;
	float	DLightDebugModels;
	float	DLightDirectionalMix;
	float	PPDLightModelEnable;
	float	PPDLightModelDebug; // 0=cpu, 1=blend, 2=gpu-prefer
	vec4	AmbientSkyParams; // x: enabled, y: scale, z: debug mode, w: unused
	vec4	AmbientSkyTint;   // rgb: tint, w: cap
	float	_Pad1[3];
	InstanceData instances[];
} AliasFrameBuffer;

struct Light
{
	vec3 origin;
	float radius;
	vec3 color;
	float minlight;
};

layout(std430, binding=0) restrict readonly buffer LightBuffer
{
	vec2 LightStyles[64];
	Light Lights[];
};

layout(binding=0) uniform sampler2D Tex;
layout(binding=1) uniform sampler2D FullbrightTex;
layout(binding=2) uniform sampler2D EmissiveTex;
layout(binding=3) uniform sampler2D NormalTex;
layout(binding=7) uniform sampler2DArray SunShadowTex;
layout(binding=8) uniform samplerCubeArray DLightShadowTex;

uniform mat4 ShadowSunViewProj[4];
uniform vec4 ShadowSunSplits;
uniform int ShadowSunCascadeCount;
layout(location=34) uniform vec4 ShadowEnableDebug;   // x=enabled, y=sun, z=dlight, w=debug mode
layout(location=35) uniform vec4 ShadowDLightIndices; // selected light indices (float encoded ints)
layout(location=36) uniform vec4 ShadowBiasCounts;    // x=num dlight slots, y=sun bias, z=dlight bias, w=receiver bias scale
layout(location=37) uniform vec4 ShadowPCFTexel;      // x=sun pcf radius, y=dlight pcf radius, z=1/sun size, w=1/dlight size
layout(location=38) uniform vec4 ShadowSunDirEnabled; // xyz dir, w enabled
layout(location=39) uniform vec4 ShadowSunColorIntensity;
layout(location=40) uniform float ShadowSunVisibility;
layout(location=41) uniform float ShadowNumLights;
layout(location=42) uniform vec4 ShadowDLightConfig0; // x: scale, y: radius scale, z: falloff mode, w: exp
layout(location=43) uniform vec4 ShadowDLightConfig1; // x: core boost, y: core exp, z: soft knee, w: ndotl mix
layout(location=44) uniform vec4 ShadowDLightConfig2; // x: saturation chop
layout(location=45) uniform vec4 RimLightParams0; // x=enable, y=intensity, z=power, w=shadowed
layout(location=46) uniform vec4 RimLightParams1; // x=world_enable, y=model_enable, z=sun_scale, w=dlight_scale

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

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-abs(AliasFrameBuffer.Fog.w) * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(AliasFrameBuffer.Fog.rgb, clr, fog);
}

float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
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
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

vec2 ComputeVelocity(vec4 curr_clip, vec4 prev_clip)
{
	const float EPS = 1e-6;
	float inv_curr_w = abs(curr_clip.w) > EPS ? 1.0 / curr_clip.w : 0.0;
	float inv_prev_w = abs(prev_clip.w) > EPS ? 1.0 / prev_clip.w : 0.0;
	vec2 curr_ndc = curr_clip.xy * inv_curr_w;
	vec2 prev_ndc = prev_clip.xy * inv_prev_w;
	return (curr_ndc - prev_ndc) * 0.5;
}

const int ALIAS_FLAG_NO_MOTION_BLUR = 1;
const int ALIAS_FLAG_VIEWMODEL = 2;
const int ALIAS_FLAG_LIGHTNING = 4;
const int SHADOW_DLIGHT_MAX = 4;
const int SHADOW_LIGHT_MAX = 64;

#if MODE == 2
	layout(location=0) noperspective in vec2 in_texcoord;
#else
	layout(location=0) in vec2 in_texcoord;
#endif
layout(location=1) in vec4 in_color;
layout(location=2) in vec3 in_pos;
layout(location=3) noperspective in vec4 in_curr_clip;
layout(location=4) noperspective in vec4 in_prev_clip;
layout(location=5) flat in int in_flags;
layout(location=6) in vec3 in_normal;
layout(location=7) in vec4 in_tangent;
layout(location=8) in float in_dlight_vis;
layout(location=9) in vec3 in_dlight_color;
layout(location=10) in float in_sky_visibility;

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
		// WBOIT: single atomic write — matches world.frag convention exactly.
		// BUG FIX: two-step write (assign then *= out_accum.a) is undefined on
		// some drivers; collapsed into one expression.
		out_accum  = vec4(color.rgb * color.a * weight, color.a * weight);
		out_reveal = color.a;
	}

	#define main main_body
#else
	layout(location=0) out vec4 OUT_COLOR;
	layout(location=1) out vec4 out_velocity;
#endif

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
	float dist = length(worldPos - AliasFrameBuffer.EyePos);
	if (cascades <= 1) return 0;
	if (dist <= ShadowSunSplits.x || cascades == 1) return 0;
	if (dist <= ShadowSunSplits.y || cascades == 2) return 1;
	if (dist <= ShadowSunSplits.z || cascades == 3) return 2;
	return cascades - 1;
}

float ComputeFalloff(float x, float mode, float expval)
{
	if (mode < 0.5)
		return x;
	if (mode < 1.5)
		return x * x;
	if (mode < 2.5)
		return x * x * (3.0 - 2.0 * x);
	return pow(max(x, 0.0), expval);
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

float SampleDLightShadowSlot(vec3 worldPos, vec3 lightPos, float radius, int slot)
{
	vec3 dir = worldPos - lightPos;
	float dist = length(dir);
	if (dist <= 1e-5 || radius <= 1e-5)
		return 1.0;

	vec3 dirN = dir / dist;
	// INVARIANTE: ref = dist/radius korrekt NUR wenn farPlane == radius beim Shadow-Render-Pass.
	// CPU: ShadowLightPosFar.w = l.radius setzen.
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

vec3 ComputeAliasDLightContribution(vec3 worldPos, vec3 worldNormal)
{
	int numLights = int(clamp(ShadowNumLights, 0.0, float(SHADOW_LIGHT_MAX)));
	float falloff_mode = ShadowDLightConfig0.z;
	float falloff_exp = max(ShadowDLightConfig0.w, 0.01);
	float core_boost = max(ShadowDLightConfig1.x, 0.0);
	float core_exp = max(ShadowDLightConfig1.y, 0.01);
	float knee = max(ShadowDLightConfig1.z, 0.0);
	float ndotl_mix = clamp(AliasFrameBuffer.DLightDirectionalMix, 0.0, 1.0);
	float dlight_scale = max(ShadowDLightConfig0.x, 0.0);
	vec3 dynamic_light = vec3(0.0);

	if (numLights <= 0)
		return vec3(0.0);

	float dynamic_light_noise = 1.0 - whitenoise01(worldPos.xy) * 0.15;
	for (int lightIndex = 0; lightIndex < numLights; ++lightIndex)
	{
		Light l = Lights[lightIndex];
		float rad = l.radius;
		vec3 to_light = l.origin - worldPos;
		float dist_sq = dot(to_light, to_light);
		if (dist_sq >= rad * rad)
			continue;

		float surface_dist = sqrt(max(dist_sq, 1e-12));
		if (rad - surface_dist < l.minlight)
			continue;

		float normalized_dist = surface_dist / max(rad, 1e-4);
		float x = clamp(1.0 - normalized_dist, 0.0, 1.0);
		float falloff = ComputeFalloff(x, falloff_mode, falloff_exp);
		float minlight_norm = l.minlight / max(rad, 1e-4);
		float attenuation = clamp((x - minlight_norm) / max(1.0 - minlight_norm, 1e-4), 0.0, 1.0);
		float intensity = attenuation * falloff;
		float core = 1.0 + core_boost * pow(max(x, 0.0), core_exp);
		float core_intensity = intensity * core;
		float shaped = (knee > 0.0) ? (core_intensity / (core_intensity + knee)) : core_intensity;

		float ndotl = 1.0;
		if (surface_dist > 0.0)
		{
			vec3 light_dir = to_light / surface_dist;
			float ndotl_raw = max(dot(worldNormal, light_dir), 0.0);
			ndotl = mix(1.0, ndotl_raw, ndotl_mix);
		}

		float shadow = 1.0;
		int slot = ShadowSlotForLight(lightIndex);
		if (slot >= 0)
			shadow = SampleDLightShadowSlot(worldPos, l.origin, rad, slot);

		dynamic_light += shaped * ndotl * shadow * l.color * dynamic_light_noise;
	}

	float satchop = clamp(ShadowDLightConfig2.x, 0.0, 1.0);
	if (satchop > 0.0)
	{
		float luma = dot(dynamic_light, vec3(0.299, 0.587, 0.114));
		dynamic_light = mix(dynamic_light, vec3(luma), satchop);
	}

	return clamp(dynamic_light * dlight_scale * AliasFrameBuffer.Overbright, 0.0, AliasFrameBuffer.Overbright);
}

float ComputeAliasDLightShadow(vec3 worldPos)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.z < 0.5)
		return 1.0;

	int numLights = int(clamp(ShadowNumLights, 0.0, float(SHADOW_LIGHT_MAX)));
	float accum = 0.0;
	float weightSum = 0.0;
	int slots = int(clamp(ShadowBiasCounts.x, 0.0, float(SHADOW_DLIGHT_MAX)));
	for (int slot = 0; slot < slots; ++slot)
	{
		int idx = int(round((slot == 0) ? ShadowDLightIndices.x :
				    (slot == 1) ? ShadowDLightIndices.y :
				    (slot == 2) ? ShadowDLightIndices.z : ShadowDLightIndices.w));
		if (idx < 0 || idx >= numLights)
			continue;
		Light l = Lights[idx];
		vec3 d = worldPos - l.origin;
		float dist = length(d);
		if (dist >= l.radius || l.radius <= 1e-5)
			continue;
		float w = 1.0 - dist / l.radius;
		float vis = SampleDLightShadowSlot(worldPos, l.origin, l.radius, slot);
		accum += vis * w;
		weightSum += w;
	}
	return (weightSum > 1e-6) ? (accum / weightSum) : 1.0;
}

float SampleFirstDLightDepth(vec3 worldPos)
{
	if (ShadowEnableDebug.x < 0.5 || ShadowEnableDebug.z < 0.5)
		return 1.0;
	if (ShadowBiasCounts.x < 0.5)
		return 1.0;

	int numLights = int(clamp(ShadowNumLights, 0.0, float(SHADOW_LIGHT_MAX)));
	int idx = int(round(ShadowDLightIndices.x));
	if (idx < 0 || idx >= numLights)
		return 1.0;

	Light l = Lights[idx];
	vec3 dir = worldPos - l.origin;
	float len = length(dir);
	if (len <= 1e-5)
		return 1.0;

	return texture(DLightShadowTex, vec4(dir / len, 0.0)).r;
}

void main()
{
	vec2 uv = in_texcoord;
	vec3 emissive = vec3(0.0);
	vec4 lit_color = in_color;
	vec3 world_pos = in_pos + AliasFrameBuffer.EyePos;
	vec3 world_nor = normalize(in_normal);
	vec3 world_geo_nor = world_nor;
	float tangent_len = length(in_tangent.xyz);
	vec3 sampled_n = texture(NormalTex, uv).xyz * 2.0 - 1.0;
	bool has_nm = dot(sampled_n.xy, sampled_n.xy) > 1e-5;
	if (tangent_len > 1e-5 && has_nm)
	{
		vec3 T = normalize(in_tangent.xyz);
		vec3 B = normalize(cross(world_geo_nor, T)) * sign(in_tangent.w);
		mat3 TBN = mat3(T, B, world_geo_nor);
		world_nor = normalize(TBN * normalize(sampled_n));
	}
	float dlight_shadow = 1.0;
	float sun_shadow = 1.0;
	vec3 dlight_contrib = vec3(0.0);

#if MODE == 2
	uv -= 0.5 / vec2(textureSize(Tex, 0).xy);
	vec4 result = textureLod(Tex, uv, 0.0);
#else
	vec4 result = texture(Tex, uv);
#endif
	vec3 texel_albedo = result.rgb;
	float texel_alpha = result.a;

#if ALPHATEST
	if (result.a < 0.666)
		discard;
	result.rgb *= lit_color.rgb;
#else
	result.rgb *= mix(vec3(1.0), lit_color.rgb, result.a);
#endif

	result.a = lit_color.a;
	vec3 albedo = result.rgb;
	dlight_shadow = ComputeAliasDLightShadow(world_pos);
	dlight_contrib = in_dlight_color;
	if (AliasFrameBuffer.PPDLightModelEnable > 0.5 && ShadowNumLights > 0.5)
	{
		float model_dlight_mode = clamp(floor(AliasFrameBuffer.PPDLightModelDebug + 0.5), 0.0, 2.0);
		vec3 gpu_dlight = ComputeAliasDLightContribution(world_pos, world_nor);
		float gpu_energy = dot(gpu_dlight, vec3(0.2126, 0.7152, 0.0722));
		if (model_dlight_mode >= 1.5)
		{
			/* GPU-prefer mode keeps CPU as fallback if the per-light list is empty/culled. */
			if (gpu_energy > 1e-6)
				dlight_contrib = gpu_dlight;
		}
		else if (model_dlight_mode >= 0.5)
		{
			float dir_mix = clamp(AliasFrameBuffer.DLightDirectionalMix, 0.0, 1.0);
			if (gpu_energy > 1e-6)
				dlight_contrib = mix(in_dlight_color, gpu_dlight, dir_mix);
		}
	}
#if ALPHATEST
	result.rgb += texel_albedo * dlight_contrib;
#else
	result.rgb += texel_albedo * dlight_contrib * texel_alpha;
#endif

	if (ShadowEnableDebug.x > 0.5 && ShadowSunDirEnabled.w > 0.5 && (in_flags & ALIAS_FLAG_VIEWMODEL) == 0)
	{
		// ShadowSunDirEnabled.xyz is scene->sun; negate for incoming light direction.
		// The vector is pre-normalized on the CPU — no normalize() needed here.
		vec3 sun_to_surface = -ShadowSunDirEnabled.xyz;
		float ndotl = max(dot(world_nor, sun_to_surface), 0.0);
		if (ndotl > 0.0)
		{
			sun_shadow = SampleSunShadow(world_pos);
			result.rgb += albedo * ShadowSunColorIntensity.rgb * ShadowSunColorIntensity.a
				* ndotl * max(ShadowSunVisibility, 0.0) * sun_shadow;
		}
	}

	{
		float sky_enable = clamp(AliasFrameBuffer.AmbientSkyParams.x, 0.0, 1.0);
		float sky_scale = max(AliasFrameBuffer.AmbientSkyParams.y, 0.0);
		float sky_visibility = clamp(in_sky_visibility, 0.0, 1.0);
		float sky_upness = clamp(world_nor.z * 0.5 + 0.5, 0.0, 1.0);
		float sky_room = 1.0 - 0.6 * max(max(lit_color.r, lit_color.g), lit_color.b);
		vec3 sky_ambient = AliasFrameBuffer.AmbientSkyTint.rgb
			* (sky_enable * sky_scale * mix(0.2, 1.0, sky_upness) * sky_visibility * max(sky_room, 0.0));
		sky_ambient = min(sky_ambient, vec3(max(AliasFrameBuffer.AmbientSkyTint.a, 0.0)));
		result.rgb += max(min(sky_ambient, vec3(1.0) - result.rgb), vec3(0.0));

		if (AliasFrameBuffer.AmbientSkyParams.z > 0.5)
		{
			float sky_luma = clamp(dot(sky_ambient, vec3(0.2126, 0.7152, 0.0722)) * 8.0, 0.0, 1.0);
			result.rgb = vec3(sky_visibility, sky_luma, 0.0);
		}
	}

	if (RimLightParams0.x > 0.5 && RimLightParams1.y > 0.5)
	{
		vec3 to_eye = AliasFrameBuffer.EyePos - world_pos;
		float to_eye_len_sq = dot(to_eye, to_eye);
		if (to_eye_len_sq > 1e-8)
		{
			vec3 view_dir = to_eye * inversesqrt(to_eye_len_sq);
			/* Drive rim with geometric normal, not normal-map perturbations.
			 * This keeps the effect on silhouette edges instead of broad
			 * full-surface brightening on viewmodels. */
			float rim_ndotv = 1.0 - clamp(abs(dot(world_geo_nor, view_dir)), 0.0, 1.0);
			float rim_factor = pow(max(rim_ndotv, 0.0), max(RimLightParams0.z, 0.5)) * max(RimLightParams0.y, 0.0);
			if (rim_factor > 1e-5)
			{
				/* Base rim from model lighting so the effect remains visible even
				 * in scenes without explicit sun/dlight contributions. */
				vec3 rim_light = max(lit_color.rgb, vec3(0.0)) * 0.28;
				float use_rim_shadows = (RimLightParams0.w > 0.5) ? 1.0 : 0.0;

				if (RimLightParams1.z > 0.0 && ShadowSunDirEnabled.w > 0.5 && (in_flags & ALIAS_FLAG_VIEWMODEL) == 0)
				{
					vec3 sun_to_surface = -ShadowSunDirEnabled.xyz;
					float sun_back = max(dot(-world_geo_nor, sun_to_surface), 0.0);
					float rim_sun_shadow = (use_rim_shadows > 0.5) ? SampleSunShadow(world_pos) : 1.0;
					float sun_vis = max(ShadowSunVisibility, 0.0);
					rim_light += ShadowSunColorIntensity.rgb * ShadowSunColorIntensity.a
						* sun_vis * rim_sun_shadow * mix(0.35, 1.0, sun_back) * RimLightParams1.z;
				}

				if (RimLightParams1.w > 0.0)
				{
					vec3 rim_dlight = max(in_dlight_color, vec3(0.0));
					if (AliasFrameBuffer.PPDLightModelEnable > 0.5 && ShadowNumLights > 0.5)
						rim_dlight = ComputeAliasDLightContribution(world_pos, world_nor);
					rim_light += rim_dlight * RimLightParams1.w;
				}

				result.rgb += albedo * rim_light * rim_factor;
			}
		}
	}

	vec3 fullbright;
#if MODE == 2
	fullbright = textureLod(FullbrightTex, uv, 0.0).rgb;
	emissive = textureLod(EmissiveTex, uv, 0.0).rgb;
#else
	fullbright = texture(FullbrightTex, uv).rgb;
	emissive = texture(EmissiveTex, uv).rgb;
#endif
	result.rgb += fullbright;
	result.rgb += emissive;

	if ((in_flags & ALIAS_FLAG_LIGHTNING) != 0)
	{
		float d = clamp(length(in_texcoord - 0.5) * 2.0, 0.0, 1.0);
		float ghost = pow(1.0 - d, 3.0) * 0.2;
		result.rgb += ghost * vec3(0.5, 0.7, 1.3);
	}

	result.rgb = clamp(result.rgb, 0.0, 1.0);

	if (ShadowEnableDebug.w > 0.5)
	{
		int smode = int(ShadowEnableDebug.w + 0.5);
		if (smode == 1)
			result.rgb = vec3(sun_shadow);
		else if (smode == 2)
			result.rgb = vec3(dlight_shadow);
		else if (smode == 3)
			result.rgb = vec3(SampleSunShadowDepth(world_pos));
		else if (smode == 4)
			result.rgb = vec3(SampleFirstDLightDepth(world_pos));
		else if (smode == 5)
		{
			int ci = ShadowCascadeForWorldPos(world_pos);
			const vec3 cascade_colors[4] = vec3[4](
				vec3(1.0, 0.25, 0.25),
				vec3(0.25, 1.0, 0.25),
				vec3(0.25, 0.5, 1.0),
				vec3(1.0, 0.85, 0.25));
			result.rgb = cascade_colors[clamp(ci, 0, 3)];
		}
	}

	result.rgb = ApplyFog(result.rgb, in_pos);

	if (AliasFrameBuffer.DLightDebugModels > 0.5)
	{
		float mode = AliasFrameBuffer.DLightDebugModels;
		float debug_vis = clamp(max(dot(dlight_contrib, vec3(0.2126, 0.7152, 0.0722)), in_dlight_vis), 0.0, 1.0);
		if (mode > 4.5)
		{
			result.rgb = vec3(fract(AliasFrameBuffer.EyePos.x * 0.01), fract(AliasFrameBuffer.EyePos.y * 0.01), fract(AliasFrameBuffer.EyePos.z * 0.01));
		}
		else if (mode > 3.5)
		{
			result.rgb = vec3(debug_vis);
		}
		else if (mode > 2.5)
		{
			vec3 L = normalize(vec3(0.3, 0.5, 0.8));
			float ndotl = max(dot(world_nor, L), 0.0);
			result.rgb = vec3(ndotl);
		}
		else if (mode > 1.5)
		{
			result.rgb = world_nor * 0.5 + 0.5;
		}
		else if (mode > 0.75)
		{
			result.rgb = fract(world_pos * 0.01);
		}
		else
		{
			result.rgb = vec3(debug_vis);
		}
	}

	OUT_COLOR = result;

#if !OIT
	vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
	float viewModelMask = ((in_flags & ALIAS_FLAG_NO_MOTION_BLUR) != 0) ? 1.0 : 0.0;
	vec2 velocityOut = vec2(0.0);
	// BUG FIX: was writing velocity * result.a — multiply is wrong, alpha ~=1 anyway
	// for the opaque gate; write raw velocity to avoid sub-pixel ghosting.
	if (viewModelMask < 0.5 && result.a >= 0.999)
		velocityOut = velocity;
	out_velocity = vec4(velocityOut, viewModelMask, 1.0);
#endif

#if MODE == 1 || MODE == 2
	if (abs(AliasFrameBuffer.Fog.w) > 0.0)
	{
		OUT_COLOR.rgb = sqrt(OUT_COLOR.rgb);
		OUT_COLOR.rgb += SCREEN_SPACE_NOISE() * AliasFrameBuffer.ScreenDither;
		OUT_COLOR.rgb *= OUT_COLOR.rgb;
	}
#else
	OUT_COLOR.rgb += SUPPRESS_BANDING() * AliasFrameBuffer.ScreenDither;
#endif
}
