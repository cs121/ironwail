struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	LightColor; // xyz=LightColor w=Alpha
	vec4	DLightColor; // xyz=DLightColor
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
layout(binding=7) uniform sampler2D SunShadowTex;
layout(binding=8) uniform samplerCubeArray DLightShadowTex;

layout(location=30) uniform mat4 ShadowSunViewProj;
layout(location=34) uniform vec4 ShadowEnableDebug;   // x=enabled, y=sun, z=dlight, w=debug mode
layout(location=35) uniform vec4 ShadowDLightIndices; // selected light indices (float encoded ints)
layout(location=36) uniform vec4 ShadowBiasCounts;    // x=num dlight slots, y=sun bias, z=dlight bias
layout(location=37) uniform vec4 ShadowPCFTexel;      // x=sun pcf radius, y=dlight pcf radius, z=1/sun size, w=1/dlight size
layout(location=38) uniform vec4 ShadowSunDirEnabled; // xyz dir, w enabled
layout(location=39) uniform vec4 ShadowSunColorIntensity;
layout(location=40) uniform float ShadowSunVisibility;
layout(location=41) uniform float ShadowNumLights;
layout(location=42) uniform vec4 ShadowDLightConfig0; // x: scale, y: radius scale, z: falloff mode, w: exp
layout(location=43) uniform vec4 ShadowDLightConfig1; // x: core boost, y: core exp, z: soft knee, w: ndotl mix
layout(location=44) uniform vec4 ShadowDLightConfig2; // x: saturation chop

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
layout(location=7) in float in_dlight_vis;
layout(location=8) in vec3 in_dlight_color;

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

	vec4 clip = ShadowSunViewProj * vec4(worldPos, 1.0);
	if (abs(clip.w) <= 1e-6)
		return 1.0;

	vec3 ndc = clip.xyz / clip.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	float depth = ndc.z * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth < 0.0 || depth > 1.0)
		return 1.0;

	float bias = max(ShadowBiasCounts.y, 0.0);
	float pcf = max(ShadowPCFTexel.x, 0.0) * max(ShadowPCFTexel.z, 0.0);
	float sum = 0.0;
	float taps = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			float closest = texture(SunShadowTex, uv + vec2(float(x), float(y)) * pcf).r;
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

	vec4 clip = ShadowSunViewProj * vec4(worldPos, 1.0);
	if (abs(clip.w) <= 1e-6)
		return 1.0;

	vec3 ndc = clip.xyz / clip.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
		return 1.0;

	return texture(SunShadowTex, uv).r;
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
	float ndotl_mix = clamp(ShadowDLightConfig1.w, 0.0, 1.0);
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

	float accum = 0.0;
	float weightSum = 0.0;
	int slots = int(clamp(ShadowBiasCounts.x, 0.0, float(SHADOW_DLIGHT_MAX)));
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

void main()
{
	vec2 uv = in_texcoord;
	vec3 emissive = vec3(0.0);
	vec4 lit_color = in_color;
	vec3 world_pos = in_pos + AliasFrameBuffer.EyePos;
	vec3 world_nor = normalize(in_normal);
	float dlight_shadow = 1.0;
	float sun_shadow = 1.0;
	vec3 dlight_contrib = vec3(0.0);

#if MODE == 2
	uv -= 0.5 / vec2(textureSize(Tex, 0).xy);
	vec4 result = textureLod(Tex, uv, 0.0);
#else
	vec4 result = texture(Tex, uv);
#endif

#if ALPHATEST
	if (result.a < 0.666)
		discard;
	result.rgb *= lit_color.rgb;
#else
	result.rgb *= mix(vec3(1.0), lit_color.rgb, result.a);
#endif

	result.a = lit_color.a;
	vec3 albedo = result.rgb;
	dlight_contrib = ComputeAliasDLightContribution(world_pos, world_nor);
	if (ShadowNumLights < 0.5)
	{
		// Legacy path: per-light shadow already absent from in_dlight_color;
		// apply the aggregated shadow term here.
		// BUG FIX: was missing dlight_shadow multiplication entirely.
		dlight_shadow = ComputeAliasDLightShadow(world_pos);
		dlight_contrib = in_dlight_color * dlight_shadow;
	}
	else
	{
		// Per-light shadow already integrated inside ComputeAliasDLightContribution().
		// Still compute aggregate for debug visualiser.
		dlight_shadow = ComputeAliasDLightShadow(world_pos);
	}
	result.rgb += albedo * dlight_contrib;

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

