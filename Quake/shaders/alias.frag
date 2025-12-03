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

layout(std430, binding=1) restrict readonly buffer InstanceBuffer
{
	mat4	ViewProj;
	mat4	PrevViewProj;
	vec3	EyePos;
	float	_Pad0;
	vec4	Fog;
	float	ScreenDither;
	float	Overbright;
	float	ModelHalfLambert;
	float   _Pad[5];
	InstanceData instances[];
};

// ALU-only 16x16 Bayer matrix
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
	float fog = exp2(-abs(Fog.w) * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}

// Hash without Sine
// https://www.shadertoy.com/view/4djSRW 
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

// Convert uniform distribution to triangle-shaped distribution
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
const int ALIAS_FLAG_LIGHTNING = 4;

layout(binding=0) uniform sampler2D Tex;
layout(binding=1) uniform sampler2D FullbrightTex;
layout(binding=2) uniform sampler2D EmissiveTex;
uniform vec3 lightDir;

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
layout(location=6) in vec3 vNormal;

#define OUT_COLOR out_fragcolor

#if OIT
	vec4 OUT_COLOR;
	layout(location=0) out vec4 out_accum;
	layout(location=1) out float out_reveal;

	vec3 GammaToLinear(vec3 v)
	{
		#if 0
			return v * v;
		#else
			return v;
		#endif
	}

	void main_body();

	void main()
	{
		main_body();
		OUT_COLOR = clamp(OUT_COLOR, 0.0, 1.0);
		vec4 color = vec4(GammaToLinear(OUT_COLOR.rgb), OUT_COLOR.a);
		float z = 1.0 / gl_FragCoord.w;
		#if 0
			float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/2e5, 2.0)), 1e-2, 3e3);
		#else
			float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);
		#endif
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
	vec2 uv = in_texcoord;
	vec3 emissive = vec3(0.0);
	
	#if MODE == 2
		uv -= 0.5 / vec2(textureSize(Tex, 0).xy);
		vec4 result = textureLod(Tex, uv, 0.0);
	#else
		vec4 result = texture(Tex, uv);
	#endif
	
	#if ALPHATEST
		if (result.a < 0.666)
			discard;
		result.rgb *= in_color.rgb;
	#else
		result.rgb = mix(result.rgb, result.rgb * in_color.rgb, result.a);
	#endif
	
	result.a = in_color.a;
	
	vec3 fullbright;
	#if MODE == 2
		fullbright = textureLod(FullbrightTex, uv, 0.0).rgb;
		emissive = textureLod(EmissiveTex, uv, 0.0).rgb;
	#else
		fullbright = texture(FullbrightTex, uv).rgb;
		emissive = texture(EmissiveTex, uv).rgb;
	#endif
	
	// Lighting calculation
	vec3 normal = normalize(vNormal);
	float ndotl = max(dot(normal, lightDir), 0.0);
	float hLambert = pow(ndotl * 0.5 + 0.5, 1.3);
	
	vec3 diffuse = result.rgb * hLambert;
	result.rgb = diffuse + fullbright + emissive;
	
	// Lightning effect
	if ((in_flags & ALIAS_FLAG_LIGHTNING) != 0)
	{
		float d = clamp(length(in_texcoord - 0.5) * 2.0, 0.0, 1.0);
		float ghost = pow(1.0 - d, 3.0) * 0.2;
		result.rgb += ghost * vec3(0.5, 0.7, 1.3);
	}
	
	result.rgb = clamp(result.rgb, 0.0, 1.0);
	result.rgb = ApplyFog(result.rgb, in_pos);
	
	OUT_COLOR = result;
	
	#if !OIT
		vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
		float viewModelMask = ((in_flags & ALIAS_FLAG_NO_MOTION_BLUR) != 0) ? 1.0 : 0.0;
		vec2 velocityOut = vec2(0.0);
		
		if (viewModelMask < 0.5 && result.a >= 0.999)
			velocityOut = velocity * result.a;
		
		out_velocity = vec4(velocityOut, viewModelMask, 0.0);
	#endif
	
	// Dithering
	#if MODE == 1 || MODE == 2
		if (abs(Fog.w) > 0.0)
		{
			OUT_COLOR.rgb = sqrt(OUT_COLOR.rgb);
			OUT_COLOR.rgb += SCREEN_SPACE_NOISE() * ScreenDither;
			OUT_COLOR.rgb *= OUT_COLOR.rgb;
		}
	#else
		OUT_COLOR.rgb += SUPPRESS_BANDING() * ScreenDither;
	#endif
}