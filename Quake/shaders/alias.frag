struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	LightColor; // xyz=LightColor w=Alpha
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
	float	_Pad1;
	float	_Pad2;
	float	_Pad3;
	InstanceData instances[];
};
layout(std140, binding=0) uniform FrameDataUBO
{
        mat4    _FrameViewProj;
        mat4    _FramePrevViewProj;
        vec4    _FrameFog;
        vec4    _FrameSkyFog;
        vec3    _FrameWindDir;
        float   _FrameWindPhase;
        float   _FrameScreenDither;
        float   _FrameTextureDither;
        float   _FrameOverbright;
        float   _FrameDynamicLightClamp;
        float   _FrameLightmapStrength;
        float   _FrameRimAlias;
        float   _FrameRimWorld;
        float   _FrameRimExponent;
        float   _FramePadRim;
        vec3    _FrameEyePos;
        float   _FrameTime;
        vec3    _FramePrevEyePos;
        float   _FrameDeltaTime;
        float   _FrameZLogScale;
        float   _FrameZLogBias;
        uint    NumLights;
        uint    _FramePrevFrameValid;
        uint    _FramePad1;
        uint    _FramePad2;
};

struct Light
{
        vec3    origin;
        float   radius;
        vec3    color;
        float   minlight;
};

layout(std430, binding=0) restrict readonly buffer LightBuffer
{
        float   LightStyles[64];
        Light   Lights[];
};

// ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));	// 0  0  0  0 | x3 x2 x1 x0 |  0  0  0  0 | y3 y2 y1 y0
	v = (v ^ (v << 2)) & 0x3333;				// 0  0 x3 x2 |  0  0 x1 x0 |  0  0 y3 y2 |  0  0 y1 y0
	v = (v ^ (v << 1)) & 0x5555;				// 0 x3  0 x2 |  0 x1  0 x0 |  0 y3  0 y2 |  0 y1  0 y0
	v |= v >> 7;								// 0 x3  0 x2 |  0 x1  0 x0 | x3 y3 x2 y2 | x1 y1 x0 y0
	v = bitfieldReverse(v) >> 24;				// 0  0  0  0 |  0  0  0  0 | y0 x0 y1 x1 | y2 x2 y3 x3
	return float(v) * (1.0/256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

// Hash without Sine
// https://www.shadertoy.com/view/4djSRW 
float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * .1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float whitenoise(vec2 p)
{
	return whitenoise01(p) - 0.5;
}

// Convert uniform distribution to triangle-shaped distribution
// Input in [0..1], output in [-1..1]
// Based on https://www.shadertoy.com/view/4t2SDh 
float tri(float x)
{
        float orig = x * 2.0 - 1.0;
        uint signbit = floatBitsToUint(orig) & 0x80000000u;
        x = sqrt(abs(orig)) - 1.;
        x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
        return x;
}

#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

float saturate(float x){ return clamp(x, 0.0, 1.0); }

float smoothstep01(float x)
{
        x = saturate(x);
        return x * x * (3.0 - 2.0 * x);
}

float r_avertexnormal_dot(vec3 vertexnormal, vec3 dir)
{
        float d = dot(vertexnormal, dir);
        if (d < 0.0)
                return 1.0 + d * (13.0 / 44.0);
        else
                return 1.0 + d;
}

vec3 ComputeDynamicLights(vec3 world_pos, vec3 normal)
{
        vec3 accum = vec3(0.0);
        uint count = NumLights;
        if (count == 0u)
                return accum;

        float normal_len_sq = dot(normal, normal);
        if (normal_len_sq <= 0.0)
                return accum;

        vec3 plane_n = normal * inversesqrt(normal_len_sq);
        float plane_w = dot(world_pos, plane_n);

        for (uint i = 0u; i < count; ++i)
        {
                Light light = Lights[i];
                float radius = light.radius;
                if (radius <= 0.0)
                        continue;

                float dist = dot(light.origin, plane_n) - plane_w;
                float absdist = abs(dist);
                float radial_radius = radius - absdist;
                float minlight = light.minlight;
                if (radial_radius <= minlight)
                        continue;

                vec3 local_pos = light.origin - plane_n * dist;
                vec3 L = local_pos - world_pos;
                float Llen2 = dot(L, L);
                float sdist = sqrt(max(Llen2, 0.0));

                float minlight_span = max(radial_radius - minlight, 1e-4);
                float attenuation = smoothstep01((minlight_span - sdist) / minlight_span);
                float falloff = smoothstep01((radial_radius - sdist) / max(radial_radius, 1e-4));
                float axial_falloff = smoothstep01((radius - absdist) / max(radius, 1e-4));
                falloff *= falloff;

                if (attenuation <= 0.0 || falloff <= 0.0 || axial_falloff <= 0.0)
                        continue;

                float distance_sq = Llen2 + dist * dist;
                if (distance_sq <= 0.0)
                        continue;
                float inv_radius_sq = 1.0 / max(radius * radius, 1e-4);
                float distance_falloff = 1.0 / (1.0 + distance_sq * inv_radius_sq);

                vec3 light_vec = L + plane_n * dist;
                vec3 Ldir = light_vec * inversesqrt(distance_sq);
                float diffuse = max(dot(plane_n, Ldir), 0.0);
                float influence = max(diffuse, minlight);
                float intensity = attenuation * falloff * axial_falloff * distance_falloff;

                accum += light.color * (intensity * influence);
        }

        return accum * (1.0 / 200.0);
}

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
const int ALIAS_FLAG_VIEWMODEL = 1 << 1;
const int ALIAS_FLAG_PLAYER = 1 << 2;
const int ALIAS_FLAG_FULLBRIGHT_HACK = 1 << 3;
const int ALIAS_FLAG_ITEM = 1 << 4;
const int ALIAS_FLAG_FORCE_FULLBRIGHT = 1 << 5;

layout(binding=0) uniform sampler2D Tex;
layout(binding=1) uniform sampler2D FullbrightTex;
layout(binding=2) uniform sampler2D EmissiveTex;

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
layout(location=6) in vec3 in_world_pos;
layout(location=7) in vec3 in_world_normal;
layout(location=8) in vec3 in_local_normal;
layout(location=9) flat in vec3 in_shadevector;

#define OUT_COLOR out_fragcolor
#if OIT
	vec4 OUT_COLOR;
	layout(location=0) out vec4 out_accum;
	layout(location=1) out float out_reveal;

	vec3 GammaToLinear(vec3 v)
	{
#if 0
		return v*v;
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
		float z = 1./gl_FragCoord.w;
#if 0
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/2e5, 2.0)), 1e-2, 3e3);
#else
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);
#endif
		out_accum = vec4(color.rgb, color.a * weight);
		out_accum.rgb *= out_accum.a;
		out_reveal = color.a;
	}

	#define main main_body
#else
        layout(location=0) out vec4 OUT_COLOR;
        layout(location=1) out vec4 out_velocity;
#endif // OIT

void main()
{
        vec2 uv = in_texcoord;
        vec3 emissive = vec3(0.0);
#if MODE == 2
        uv -= 0.5 / vec2(textureSize(Tex, 0).xy);
        vec4 baseSample = textureLod(Tex, uv, 0.);
#else
        vec4 baseSample = texture(Tex, uv);
#endif
#if ALPHATEST
        if (baseSample.a < 0.666)
                discard;
#endif
        vec3 baseColor = baseSample.rgb;
        vec3 fullbright;
#if MODE == 2
        fullbright = textureLod(FullbrightTex, uv, 0.).rgb;
        emissive = textureLod(EmissiveTex, uv, 0.).rgb;
#else
        fullbright = texture(FullbrightTex, uv).rgb;
        emissive = texture(EmissiveTex, uv).rgb;
#endif
        vec3 localNormal = normalize(in_local_normal);
        vec3 shadevector = normalize(in_shadevector);
        float shade = r_avertexnormal_dot(localNormal, shadevector);
        vec3 lighting = (in_color.rgb * _FrameLightmapStrength) * shade;
        vec3 worldNormal = in_world_normal;
        if (dot(worldNormal, worldNormal) > 0.0)
                worldNormal = normalize(worldNormal);
        else
                worldNormal = vec3(0.0, 0.0, 1.0);
        lighting += ComputeDynamicLights(in_world_pos, worldNormal);
        if (_FrameRimAlias > 0.0)
        {
                vec3 to_eye = EyePos - in_world_pos;
                float inv_len = inversesqrt(max(dot(to_eye, to_eye), 1e-8));
                vec3 view_dir = to_eye * inv_len;
                float ndotv = max(dot(worldNormal, view_dir), 0.0);
                float fresnel = pow(clamp(1.0 - ndotv, 0.0, 1.0), _FrameRimExponent);
                lighting += vec3(_FrameRimAlias * fresnel);
        }
        lighting = max(lighting, vec3(0.0));

        uint overbrightFlag = floatBitsToUint(Fog.w) >> 31u;
        bool useFullbrightHack = ((in_flags & ALIAS_FLAG_FULLBRIGHT_HACK) != 0) && overbrightFlag == 0u;

        if (useFullbrightHack)
        {
                lighting = vec3(256.0 / 200.0);
        }
        else
        {
                float sum = lighting.x + lighting.y + lighting.z;
                if ((in_flags & ALIAS_FLAG_VIEWMODEL) != 0)
                {
                        const float minSum = 72.0 / 200.0;
                        if (sum < minSum)
                        {
                                float add = (minSum - sum) / 3.0;
                                lighting += vec3(add);
                                sum = minSum;
                        }
                }
                else if ((in_flags & ALIAS_FLAG_PLAYER) != 0)
                {
                        const float minSum = 24.0 / 200.0;
                        if (sum < minSum)
                        {
                                float add = (minSum - sum) / 3.0;
                                lighting += vec3(add);
                                sum = minSum;
                        }
                }
                if (overbrightFlag != 0u)
                {
                        const float maxSum = 288.0 / 200.0;
                        if (sum > maxSum)
                        {
                                float scale = maxSum / sum;
                                lighting *= scale;
                                sum = maxSum;
                        }
                }
        }

        lighting = ldexp(lighting, ivec3(int(overbrightFlag)));

#if ALPHATEST
        vec3 shadedColor = baseColor * lighting;
#else
        vec3 shadedColor = mix(baseColor, baseColor * lighting, baseSample.a);
#endif

        if (((in_flags & ALIAS_FLAG_ITEM) != 0) || ((in_flags & ALIAS_FLAG_FORCE_FULLBRIGHT) != 0))
                shadedColor += fullbright;
        shadedColor += emissive;
        shadedColor = clamp(shadedColor, 0.0, 1.0);

	vec4 result = vec4(shadedColor, in_color.a);
	if ((in_flags & ALIAS_FLAG_VIEWMODEL) != 0)
		result.a = min(result.a, 0.5);
        float fog = exp2(abs(Fog.w) * -dot(in_pos, in_pos));
        fog = clamp(fog, 0.0, 1.0);
        result.rgb = mix(Fog.rgb, result.rgb, fog);
        out_fragcolor = result;
#if !OIT
        vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
        float viewModelMask = ((in_flags & ALIAS_FLAG_NO_MOTION_BLUR) != 0) ? 1.0 : 0.0;
        vec2 velocityOut = vec2(0.0);
        if (viewModelMask < 0.5 && result.a >= 0.999)
                velocityOut = velocity * result.a;
        out_velocity = vec4(velocityOut, viewModelMask, 0.0);
#endif
#if MODE == 1 || MODE == 2
	// Note: sign bit is used as overbright flag
	if (abs(Fog.w) > 0.)
	{
		out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
		out_fragcolor.rgb += SCREEN_SPACE_NOISE() * ScreenDither;
		out_fragcolor.rgb *= out_fragcolor.rgb;
	}
#else
	out_fragcolor.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}
