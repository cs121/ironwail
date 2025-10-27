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
        float   _FrameRimAlias;
        float   _FrameRimWorld;
        float   _FrameRimExponent;
        float   _FramePad0;
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

// ALU-only 16x16 Bayer matrix - optimiert
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

// Hash without Sine - optimiert
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

// Triangle-shaped distribution - optimiert
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

// Optimierte Shading-Funktion mit besserer Energieerhaltung
float r_avertexnormal_dot(vec3 vertexnormal, vec3 dir)
{
        float d = dot(vertexnormal, dir);
        // Verbesserte Wrap-Around Beleuchtung für weichere Schatten
        return d < 0.0 ? 1.0 + d * (13.0 / 44.0) : 1.0 + d;
}

// OPTIMIERT: Besseres Dynamic Lighting mit Distance-Squared-Falloff
vec3 ComputeDynamicLights(vec3 world_pos, vec3 normal)
{
        vec3 accum = vec3(0.0);
        uint count = NumLights;
        if (count == 0u)
                return accum;
        
        // Early-out optimization
        for (uint i = 0u; i < count; ++i)
        {
                Light light = Lights[i];
                vec3 to_light = light.origin - world_pos;
                float dist_sq = dot(to_light, to_light);
                float radius = light.radius;
                float radius_sq = radius * radius;
                
                // Früher Ausschluss für Lichter außerhalb der Reichweite
                if (dist_sq >= radius_sq)
                        continue;
                
                // Optimiert: rsqrt einmal berechnen
                float inv_dist = inversesqrt(max(dist_sq, 1e-8));
                float dist = dist_sq * inv_dist;
                vec3 L = to_light * inv_dist;
                
                // Verbesserte Attenuation: quadratischer Falloff mit smoothstep
                float norm_dist = dist / radius;
                float attenuation = 1.0 - norm_dist;
                attenuation = attenuation * attenuation; // quadratischer Falloff
                
                // Besseres diffuses Shading
                float diffuse = max(dot(normal, L), 0.0);
                
                // Ambient + Diffuse mit besserer Energieverteilung
                float influence = mix(light.minlight, 1.0, diffuse);
                
                accum += light.color * (attenuation * influence * radius);
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
		float z = 1.0 / gl_FragCoord.w;
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
        vec4 baseSample = textureLod(Tex, uv, 0.0);
#else
        vec4 baseSample = texture(Tex, uv);
#endif

#if ALPHATEST
        if (baseSample.a < 0.666)
                discard;
#endif

        vec3 baseColor = baseSample.rgb;
        
        // Kombinierte Texture-Lookups für bessere Cache-Kohärenz
#if MODE == 2
        vec3 fullbright = textureLod(FullbrightTex, uv, 0.0).rgb;
        emissive = textureLod(EmissiveTex, uv, 0.0).rgb;
#else
        vec3 fullbright = texture(FullbrightTex, uv).rgb;
        emissive = texture(EmissiveTex, uv).rgb;
#endif

        // Normalisierung optimiert
        vec3 localNormal = normalize(in_local_normal);
        vec3 shadevector = normalize(in_shadevector);
        float shade = r_avertexnormal_dot(localNormal, shadevector);
        
        // Basis-Beleuchtung
        vec3 lighting = in_color.rgb * shade;
        
        // World Normal Handling optimiert
        vec3 worldNormal = in_world_normal;
        float normal_len_sq = dot(worldNormal, worldNormal);
        worldNormal = normal_len_sq > 0.0 ? worldNormal * inversesqrt(normal_len_sq) : vec3(0.0, 0.0, 1.0);
        
        // Dynamic Lights (verbessert)
        lighting += ComputeDynamicLights(in_world_pos, worldNormal);
        
        // OPTIMIERT: Rim Lighting mit besserer Fresnel-Approximation
        if (_FrameRimAlias > 0.0)
        {
                vec3 to_eye = EyePos - in_world_pos;
                float inv_len_sq = dot(to_eye, to_eye);
                vec3 view_dir = to_eye * inversesqrt(max(inv_len_sq, 1e-8));
                float ndotv = max(dot(worldNormal, view_dir), 0.0);
                
                // Schlick's Approximation für schnellere Berechnung
                float fresnel = pow(1.0 - ndotv, _FrameRimExponent);
                lighting += vec3(_FrameRimAlias * fresnel);
        }
        
        lighting = max(lighting, vec3(0.0));

        // Flag-Handling optimiert
        uint overbrightFlag = floatBitsToUint(Fog.w) >> 31u;
        bool useFullbrightHack = ((in_flags & ALIAS_FLAG_FULLBRIGHT_HACK) != 0) && overbrightFlag == 0u;

        if (useFullbrightHack)
        {
                lighting = vec3(256.0 / 200.0);
        }
        else
        {
                float sum = lighting.x + lighting.y + lighting.z;
                
                // Minimum Lighting für verschiedene Objekttypen
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
                
                // Overbright Clamping
                if (overbrightFlag != 0u)
                {
                        const float maxSum = 288.0 / 200.0;
                        if (sum > maxSum)
                        {
                                lighting *= maxSum / sum;
                        }
                }
        }

        // LDEXP für Overbright-Skalierung
        lighting = ldexp(lighting, ivec3(int(overbrightFlag)));

        // Shading anwenden
#if ALPHATEST
        vec3 shadedColor = baseColor * lighting;
#else
        vec3 shadedColor = mix(baseColor, baseColor * lighting, baseSample.a);
#endif

        // Fullbright und Emissive
        if ((in_flags & ALIAS_FLAG_ITEM) != 0)
                shadedColor += fullbright;
        shadedColor += emissive;
        shadedColor = clamp(shadedColor, 0.0, 1.0);

        // Fog mit optimierter Berechnung
        vec4 result = vec4(shadedColor, in_color.a);
        float fog_density = abs(Fog.w);
        float fog = exp2(fog_density * -dot(in_pos, in_pos));
        fog = clamp(fog, 0.0, 1.0);
        result.rgb = mix(Fog.rgb, result.rgb, fog);
        
        out_fragcolor = result;

#if !OIT
        // Motion Blur Velocity
        vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
        float viewModelMask = ((in_flags & ALIAS_FLAG_NO_MOTION_BLUR) != 0) ? 1.0 : 0.0;
        vec2 velocityOut = vec2(0.0);
        if (viewModelMask < 0.5 && result.a >= 0.999)
                velocityOut = velocity * result.a;
        out_velocity = vec4(velocityOut, viewModelMask, 0.0);
#endif

        // Dithering für Banding-Reduktion
#if MODE == 1 || MODE == 2
	if (fog_density > 0.0)
	{
		out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
		out_fragcolor.rgb += SCREEN_SPACE_NOISE() * ScreenDither;
		out_fragcolor.rgb *= out_fragcolor.rgb;
	}
#else
	out_fragcolor.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}