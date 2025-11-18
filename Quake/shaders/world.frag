#if BINDLESS
        #extension GL_ARB_bindless_texture : require
#else
        layout(binding=0) uniform sampler2D Tex;
        layout(binding=1) uniform sampler2D FullbrightTex;
        layout(binding=4) uniform sampler2D EmissiveTex;
        layout(binding=5) uniform samplerCube EnvTex;
#endif

layout(binding=2) uniform sampler2D LMTex;
layout(binding=3) uniform sampler2D DeluxTex;

#include "frame_uniforms.glsl"

#define LIGHT_TILES_X 32
#define LIGHT_TILES_Y 16
#define LIGHT_TILES_Z 32
#define MAX_LIGHTS    64
#define MAX_EFFECT_STAGES 4

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

layout(rg32ui, binding=0) uniform readonly uimage3D LightClusters;

vec3 ApplyFog(vec3 clr, vec3 p, vec4 fogData)
{
    float fog = exp2(-fogData.w * dot(p, p));
    fog = clamp(fog, 0.0, 1.0);
    return mix(fogData.rgb, clr, fog);
}

const uint
    CF_USE_POLYGON_OFFSET = 1u,
    CF_USE_FULLBRIGHT = 2u,
    CF_NOLIGHTMAP = 4u,
    CF_USE_EMISSIVE = 8u,
    CF_ALPHA_TEST = 16u,
    CF_TC_STRETCH = 32u,
    CF_TC_TURB = 64u,
    CF_TC_ENVMAP = 128u,
    CF_CUSTOM_FOG = 256u,
    CF_EFFECTS = 512u
;

struct Call
{
    uint    flags;
    float   wateralpha;
#if BINDLESS
    uvec2   txhandle;
    uvec2   fbhandle;
    uvec2   emhandle;
    uvec2   envhandle;
#else
    int             baseinstance;
    int             padding;
#endif
    vec4    tcmod_matrix;
    vec4    tcmod_translate;
    vec4    tcmod_params0;
    vec4    tcmod_params1;
    vec4    tcgen_params;
    vec4    tcgen_s;
    vec4    tcgen_t;
    vec4    emissive_matrix;
    vec4    emissive_translate;
    vec4    emissive_color;
    vec4    fog_color;
    vec4    alpha_params0;
    vec4    alpha_params1;
    vec4    alpha_params2;
    vec4    env_params0;
    vec4    env_params1;
    vec4    env_params2;
    vec4    effect_info;
    vec4    effect_matrix[MAX_EFFECT_STAGES];
    vec4    effect_translate[MAX_EFFECT_STAGES];
    vec4    effect_color[MAX_EFFECT_STAGES];
    vec4    effect_alpha_params0[MAX_EFFECT_STAGES];
    vec4    effect_alpha_params1[MAX_EFFECT_STAGES];
    vec4    effect_alpha_params2[MAX_EFFECT_STAGES];
    vec4    effect_tcmod_params0[MAX_EFFECT_STAGES];
    vec4    effect_tcmod_params1[MAX_EFFECT_STAGES];
    vec4    effect_flags[MAX_EFFECT_STAGES];
#if BINDLESS
    uvec2   effect_handles[MAX_EFFECT_STAGES];
#endif
};

layout(std430, binding=1) restrict readonly buffer CallBuffer
{
    Call call_data[];
};

const int IW_RGB_VERTEX   = 0;
const int IW_RGB_CONST    = 1;
const int IW_RGB_IDENTITY = 2;
const int IW_RGB_ENTITY   = 3;
const int IW_RGB_WAVE     = 4;

const int IW_BLEND_NONE      = 0;
const int IW_BLEND_ALPHA     = 1;
const int IW_BLEND_ADD       = 2;
const int IW_BLEND_MUL       = 3;
const int IW_BLEND_PREMUL    = 4;
const int IW_BLEND_ADD_ALPHA = 5;

const int IW_WAVE_SIN      = 0;
const int IW_WAVE_TRIANGLE = 1;
const int IW_WAVE_SQUARE   = 2;
const int IW_WAVE_SAW      = 3;
const int IW_WAVE_FRESNEL  = 4;

// ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
    coord &= 15;
    coord.y ^= coord.x;
    uint v = uint(coord.y | (coord.x << 8));
    v = (v ^ (v << 2)) & 0x3333;
    v = (v ^ (v << 1)) & 0x5555;
    v |= v >> 7;
    v = bitfieldReverse(v) >> 24;
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

vec2 ComputeVelocity(vec4 curr_clip, vec4 prev_clip)
{
    const float EPS = 1e-6;
    float inv_curr_w = abs(curr_clip.w) > EPS ? 1.0 / curr_clip.w : 0.0;
    float inv_prev_w = abs(prev_clip.w) > EPS ? 1.0 / prev_clip.w : 0.0;
    vec2 curr_ndc = curr_clip.xy * inv_curr_w;
    vec2 prev_ndc = prev_clip.xy * inv_prev_w;
    return (curr_ndc - prev_ndc) * 0.5;
}

vec3 ComputeSunLight(vec3 pos, vec3 normal)
{
    return vec3(0.0);
}

float EvaluateWave(vec4 params, int func)
{
    float base = params.x;
    float amplitude = params.y;
    float phase = params.z;
    float frequency = params.w;
    float cycle = Time * frequency + phase / 360.0;
    float wave = 0.0;
    const float TAU = 6.28318530718;

    if (func == 0)
    {
        float angle = cycle * TAU;
        wave = sin(angle);
    }
    else if (func == 1)
    {
        float f = fract(cycle);
        wave = (abs(f * 2.0 - 1.0) * 2.0) - 1.0;
    }
    else if (func == 2)
    {
        float angle = cycle * TAU;
        wave = sign(sin(angle));
        if (wave == 0.0)
            wave = 1.0;
    }
    else if (func == 3)
    {
        float f = fract(cycle);
        wave = f * 2.0 - 1.0;
    }
    else
    {
        float angle = cycle * TAU;
        wave = sin(angle);
    }

    return base + amplitude * wave;
}

float SampleMaskChannel(vec4 texel, int maskChannel)
{
    if (maskChannel == 0)
        return texel.r;
    if (maskChannel == 1)
        return texel.g;
    if (maskChannel == 2)
        return texel.b;
    return texel.a;
}

vec4 ApplyColorMask(vec4 texel, float maskIndex)
{
    if (maskIndex < 0.0)
        return texel;

    int maskChannel = int(maskIndex + 0.5);
    if (maskChannel == 0)
    {
        texel.g = 0.0;
        texel.b = 0.0;
    }
    else if (maskChannel == 1)
    {
        texel.r = 0.0;
        texel.b = 0.0;
    }
    else if (maskChannel == 2)
    {
        texel.r = 0.0;
        texel.g = 0.0;
    }
    else if (maskChannel == 3)
    {
        texel.rgb = vec3(1.0);
        texel.a = clamp(texel.a, 0.0, 1.0);
    }

    return texel;
}

float ComputeStageAlpha(vec4 texel, vec4 params0, vec4 params1)
{
    int type = int(params0.x + 0.5);
    if (type == 1)
        return clamp(params0.y, 0.0, 1.0);
    if (type == 2)
        return 1.0;
    if (type == 3)
    {
        vec4 waveParams = vec4(params0.z, params0.w, params1.x, params1.y);
        int func = int(params1.w + 0.5);
        return clamp(EvaluateWave(waveParams, func), 0.0, 1.0);
    }
    if (type == 4)
    {
        int maskChannel = int(params1.z + 0.5);
        return clamp(SampleMaskChannel(texel, maskChannel), 0.0, 1.0);
    }

    return clamp(texel.a, 0.0, 1.0);
}

bool AlphaTestPass(float alpha, int func, float ref)
{
    const float EPS = 1.0e-4;
    switch (func)
    {
    case 1:
        return false;
    case 2:
        return alpha < ref;
    case 3:
        return abs(alpha - ref) <= EPS;
    case 4:
        return alpha <= ref;
    case 5:
        return alpha > ref;
    case 6:
        return abs(alpha - ref) > EPS;
    case 7:
        return alpha >= ref;
    case 8:
        return true;
    default:
        return true;
    }
}

vec3 ApplyEnvBlend(vec3 baseColor, vec3 envColor, float amount, int blendMode)
{
    float alpha = clamp(amount, 0.0, 1.0);
    vec3 blended = baseColor;

    switch (blendMode)
    {
    case IW_BLEND_NONE:
    case IW_BLEND_ALPHA:
        blended = mix(baseColor, envColor, alpha);
        break;
    case IW_BLEND_ADD:
        blended = clamp(baseColor + envColor * amount, 0.0, 1.0);
        break;
    case IW_BLEND_MUL:
        blended = baseColor * mix(vec3(1.0), envColor, alpha);
        break;
    case IW_BLEND_PREMUL:
    case IW_BLEND_ADD_ALPHA:
        blended = clamp(baseColor + envColor * alpha, 0.0, 1.0);
        break;
    default:
        blended = mix(baseColor, envColor, alpha);
        break;
    }

    return blended;
}

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
layout(location=20) flat in uvec2 in_samplers2;
#endif
layout(location=11) noperspective in vec4 in_curr_clip;
layout(location=12) noperspective in vec4 in_prev_clip;
layout(location=13) in vec2 in_emissive_uv;
layout(location=14) flat in vec3 in_emissive_color;
layout(location=15) flat in vec4 in_stage_params1;
layout(location=16) flat in vec4 in_alpha_params0;
layout(location=17) flat in vec4 in_alpha_params1;
layout(location=18) flat in vec4 in_alpha_params2;
layout(location=19) flat in vec4 in_fog_color;
layout(location=21) flat in vec4 in_env_params0;
layout(location=22) flat in vec4 in_env_params1;
layout(location=23) flat in vec4 in_env_params2;
layout(location=24) flat in int in_call_index;
layout(location=25) in vec2 in_effect_uv[MAX_EFFECT_STAGES];
#if !BINDLESS
layout(binding=6) uniform sampler2D EffectTex[MAX_EFFECT_STAGES];
#endif

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

// === Performance-Schalter (optional) ===
#ifndef USE_COARSE_DERIVATIVES
#define USE_COARSE_DERIVATIVES 1   // 1: dFdxCoarse/dFdyCoarse wenn verfügbar, sonst fallback
#endif
#ifndef FAST_SPECULAR_POWER
#define FAST_SPECULAR_POWER 16.0   // 16 lässt sich extrem schnell potenzieren
#endif
#ifndef USE_BRANCHLESS_ALPHA
#define USE_BRANCHLESS_ALPHA 1     // MODE==1: branchless Alpha-Test (weiterhin kompatibel)
#endif

// === Hilfsfunktionen ===
float saturate(float x){ return clamp(x,0.0,1.0); }
vec3  saturate(vec3 v){ return clamp(v,0.0,1.0); }

float smoothstep01(float x)
{
    x = saturate(x);
    return x * x * (3.0 - 2.0 * x);
}

vec3 clamp_preserving_hue(vec3 value, vec3 limit)
{
    vec3 positive = max(value, vec3(0.0));
    vec3 max_value = max(limit, vec3(0.0));
    float scale = 1.0;

    if (positive.x > max_value.x)
        scale = min(scale, max_value.x > 0.0 ? max_value.x / positive.x : 0.0);
    if (positive.y > max_value.y)
        scale = min(scale, max_value.y > 0.0 ? max_value.y / positive.y : 0.0);
    if (positive.z > max_value.z)
        scale = min(scale, max_value.z > 0.0 ? max_value.z / positive.z : 0.0);

    return min(positive * scale, max_value);
}

// schneller rsqrt/len
float fastLen(vec3 v){ return length(v); } // fallback
float fastInvLen(vec3 v){
    float lsq = dot(v,v);
    return (lsq>0.0) ? inversesqrt(lsq) : 0.0;
}
vec3  fastNorm(vec3 v){
    float inv = fastInvLen(v);
    return (inv>0.0) ? v*inv : vec3(0.0,0.0,1.0);
}

vec3 DecodeDelux(vec3 encoded, vec3 fallback)
{
    vec3 normal = encoded * 2.0 - 1.0;
    float len2 = dot(normal, normal);
    if (len2 <= 1.0e-6)
        return fallback;
    return normal * inversesqrt(len2);
}

// schneller Specular für feste Power (hier 16)
float specPow16(float x){
    x = saturate(x);
    float x2 = x*x;     // ^2
    float x4 = x2*x2;   // ^4
    float x8 = x4*x4;   // ^8
    float x16= x8*x8;   // ^16
    return x16;
}

#if USE_COARSE_DERIVATIVES && (__VERSION__ >= 450)
#define DFDX dFdxCoarse
#define DFDY dFdyCoarse
#else
#define DFDX dFdx
#define DFDY dFdy
#endif

// === Dein bestehender Code/Uniforms/Structs unverändert bis auf unten ===
// ... (alles aus deinem Original unverändert belassen) ...

// Ersetze die Normalberechnung & Lichtakkumulation im Hauptteil:
void main()
{
    Call call = call_data[in_call_index];
    vec3 fullbright = vec3(0.);
    vec3 emissive   = vec3(0.);
    vec2 uv = in_uv;
#if MODE == 2
    uv = uv * 2.0 + 0.125 * sin(uv.yx * (3.14159265 * 2.0) + Time);
#endif

#if BINDLESS
    sampler2D Tex = sampler2D(in_samplers0.xy);
    // branchless Fullbright/Emissive (Sampler nur benutzen, wenn Flag gesetzt)
    if ((in_flags & CF_USE_FULLBRIGHT) != 0u){
        sampler2D FullbrightTex = sampler2D(in_samplers0.zw);
        fullbright = texture(FullbrightTex, uv).rgb;
    }
    if ((in_flags & CF_USE_EMISSIVE) != 0u){
        sampler2D EmissiveSampler = sampler2D(in_samplers1.xy);
        emissive = texture(EmissiveSampler, in_emissive_uv).rgb * in_emissive_color;
    }
#else
    if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
        fullbright = texture(FullbrightTex, uv).rgb;
    if ((in_flags & CF_USE_EMISSIVE) != 0u)
        emissive = texture(EmissiveTex, in_emissive_uv).rgb * in_emissive_color;
#endif

    // Textur: kleinere negative LOD-Bias nur wenn DITHER aktiv
#if DITHER >= 2
    vec4 result = texture(Tex, uv, -1.0);
#elif DITHER
    vec4 result = texture(Tex, uv, -0.5);
#else
    vec4 result = texture(Tex, uv);
#endif

    result = ApplyColorMask(result, in_stage_params1.z);

    float baseAlpha = clamp(in_alpha, 0.0, 1.0);
    float stageAlpha = ComputeStageAlpha(result, in_alpha_params0, in_alpha_params1);
    if (in_alpha_params2.z > 0.5)
    {
        int alphaFunc = int(in_alpha_params2.x + 0.5);
        float alphaRef = clamp(in_alpha_params2.y, 0.0, 1.0);
        if (!AlphaTestPass(stageAlpha, alphaFunc, alphaRef))
            discard;
    }
    float finalAlpha = clamp(baseAlpha * stageAlpha, 0.0, 1.0);
    result.a = finalAlpha;

    // Lightmap fetch: unverändert, aber mit weniger temporären Vektoren
    vec2 lmuv = in_lmuv;
#if DITHER
    vec2 lmsize = vec2(textureSize(LMTex, 0).xy) * 16.0;
    lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;
#endif
    vec4 lm0 = textureLod(LMTex, lmuv, 0.0);
    vec3 static_light;
    if (in_styles.y < 0.0){
        static_light = in_styles.x * lm0.xyz;
    }else{
        vec4 lm1 = textureLod(LMTex, vec2(lmuv.x + in_lmofs, lmuv.y), 0.0);
        if (in_styles.z < 0.0){
            static_light = in_styles.x * lm0.xyz + in_styles.y * lm1.xyz;
        }else{
            vec4 lm2 = textureLod(LMTex, vec2(lmuv.x + in_lmofs*2.0, lmuv.y), 0.0);
            static_light = vec3(
                dot(in_styles, lm0),
                dot(in_styles, lm1),
                dot(in_styles, lm2)
            );
        }
    }

    static_light *= LightmapStrength;

    // schnellere Flächennormalen aus Derivaten
    vec3 dn = cross(DFDX(in_pos), DFDY(in_pos));
    vec3 geom_normal = fastNorm(dn);
    vec3 surface_normal = geom_normal;

    if ((DeluxEnabled & 1u) != 0u)
    {
        vec3 accum = vec3(0.0);
        float weight = 0.0;
        vec3 dir0 = DecodeDelux(textureLod(DeluxTex, lmuv, 0.0).xyz, geom_normal);
        float w0 = max(in_styles.x, 0.0);
        accum += dir0 * w0;
        weight += w0;

        if (in_styles.y >= 0.0)
        {
            vec2 uv1 = vec2(lmuv.x + in_lmofs, lmuv.y);
            vec3 dir1 = DecodeDelux(textureLod(DeluxTex, uv1, 0.0).xyz, geom_normal);
            float w1 = max(in_styles.y, 0.0);
            accum += dir1 * w1;
            weight += w1;

            if (in_styles.z >= 0.0)
            {
                vec2 uv2 = vec2(lmuv.x + in_lmofs * 2.0, lmuv.y);
                vec3 dir2 = DecodeDelux(textureLod(DeluxTex, uv2, 0.0).xyz, geom_normal);
                float w2 = max(in_styles.z, 0.0);
                accum += dir2 * w2;
                weight += w2;
            }
        }

        if (weight > 0.0)
        {
            vec3 normalized = fastNorm(accum);
            surface_normal = normalized;
        }
        else
        {
            surface_normal = dir0;
        }
    }

    if ((DeluxEnabled & 2u) != 0u)
    {
        float lambert = max(dot(surface_normal, geom_normal), 0.0);
        static_light *= lambert;
    }

    vec3 total_light = clamp_preserving_hue(static_light, vec3(1.0));
    vec3 specular_light = vec3(0.0);

    vec3 to_eye = EyePos - in_pos;
    float inv_view_len = fastInvLen(to_eye);
    vec3  view_dir = (inv_view_len>0.0) ? to_eye * inv_view_len : vec3(0.0,0.0,1.0);

    // Dynamisches Licht (Cluster): identische Logik, aber mit weniger temporären und teurem length()
    if (NumLights > 0u){
        ivec3 c;
        c.x = int(floor(in_coord.x));
        c.y = int(floor(in_coord.y));
        c.z = int(floor(log2(in_depth) * ZLogScale + ZLogBias));
        uvec2 clusterdata = imageLoad(LightClusters, c).xy;

        if ((clusterdata.x | clusterdata.y) != 0u){
            vec3 dynamic_light = vec3(0.0);

            vec3 plane_n = surface_normal;
            float plane_w = dot(in_pos, plane_n);

            // zwei 32er-Blocks als Maske
            for (uint i=0u, ofs=0u; i<2u; ++i, ofs+=32u){
                uint mask = clusterdata[i];
                while (mask != 0u){
                    uint bit = mask & (~mask + 1u);  // niedrigstes gesetztes Bit
                    int  j   = findLSB(mask);
                    mask ^= bit;

                    Light l = Lights[ofs + uint(j)];

                    float dist = dot(l.origin, plane_n) - plane_w;
                    float absdist = abs(dist);
                    float rad  = l.radius - absdist;
                    float minl = l.minlight;
                    if (rad < minl) continue;

                    vec3 local_pos = l.origin - plane_n * dist;
                    vec3 L = local_pos - in_pos;

                    float Llen2 = dot(L,L);
                    if (Llen2 <= 0.0) continue;
                    float Linv = inversesqrt(Llen2);
                    float sdist = 1.0 / Linv; // ≈ length(L)

                    float minlight_span = max(rad - minl, 1e-4);
                    float attenuation = smoothstep01((minlight_span - sdist) / minlight_span);
                    float falloff     = smoothstep01((rad - sdist) / max(rad, 1e-4));
                    float axialFalloff = smoothstep01((l.radius - absdist) / max(l.radius, 1e-4));
                    float distance_sq = Llen2 + dist * dist;
                    float inv_radius_sq = 1.0 / max(l.radius * l.radius, 1e-4);
                    float distance_falloff = 1.0 / (1.0 + distance_sq * inv_radius_sq);
                    falloff *= falloff;

                    if (attenuation > 0.0 && falloff > 0.0 && axialFalloff > 0.0){
                        vec3  ldir = L * Linv;
                        float ndotl = max(dot(surface_normal, ldir), 0.0);
                        float intensity = attenuation * falloff * axialFalloff * distance_falloff;
                        vec3  light_contrib = intensity * l.color;

                        if (ndotl > 0.0){
                            // Half-Vektor ohne sqrt: H = normalize(Ldir + V)
                            vec3  h  = ldir + view_dir;
                            float hl2 = dot(h,h);
                            if (hl2 > 0.0){
                                float hinv = inversesqrt(hl2);
                                float ndoth = max(dot(surface_normal, h*hinv), 0.0);

                                // schneller Specular mit fixer Power
                                float specTerm = specPow16(ndoth) * ndotl * (0.4); // SPECULAR_SCALE 0.4
                                specular_light += light_contrib * specTerm;
                            }
                        }
                        dynamic_light += light_contrib;
                    }
                }
            }
            // saturating Add (bleibt <= 1) with hue preservation
            if (dynamic_light.x > 0.0 || dynamic_light.y > 0.0 || dynamic_light.z > 0.0)
            {
                float max_dynamic = DynamicLightClamp;
                vec3  cap = vec3(max_dynamic);
                total_light = clamp_preserving_hue(total_light + dynamic_light, cap);
            }
        }
    }

    // Rim
    if (RimWorld > 0.0){
        float ndv = max(dot(surface_normal, view_dir), 0.0);
        float fres = pow(saturate(1.0 - ndv), RimExponent);
        vec3 rim = vec3(RimWorld * fres);
        vec3 rim_remaining = max(vec3(0.0), vec3(1.0) - total_light);
        if (rim_remaining.x > 0.0 || rim_remaining.y > 0.0 || rim_remaining.z > 0.0)
            total_light += clamp_preserving_hue(rim, rim_remaining);
    }

    // Sun (deine Funktion bleibt Stub/kompatibel)
    vec3 sun_light = ComputeSunLight(in_pos, surface_normal);
    vec3 sun_remaining = max(vec3(0.0), vec3(1.0) - total_light);
    if (sun_remaining.x > 0.0 || sun_remaining.y > 0.0 || sun_remaining.z > 0.0)
        total_light += clamp_preserving_hue(sun_light, sun_remaining);

#if DITHER >= 2
    vec3 total_light_unit = clamp_preserving_hue(total_light, vec3(1.0));
    vec3 total_lightmap = clamp_preserving_hue(floor(total_light_unit * 63.0 + 0.5) * (Overbright/63.0), vec3(1.0));
#else
    vec3 total_lightmap = clamp_preserving_hue(total_light * Overbright, vec3(1.0));
#endif

#if MODE != 1
    result.rgb = mix(result.rgb, result.rgb * total_lightmap, result.a);
#else
    result.rgb *= total_lightmap;
#endif

    int effect_count = int(call.effect_info.x + 0.5);
    if ((call.flags & CF_EFFECTS) == 0u)
        effect_count = 0;
    effect_count = clamp(effect_count, 0, MAX_EFFECT_STAGES);
    for (int i = 0; i < effect_count; ++i)
    {
#if BINDLESS
        vec4 effect_texel = texture(sampler2D(call.effect_handles[i]), in_effect_uv[i]);
#else
        vec4 effect_texel = texture(EffectTex[i], in_effect_uv[i]);
#endif
        effect_texel = ApplyColorMask(effect_texel, call.effect_tcmod_params1[i].z);
        float stageAlpha = ComputeStageAlpha(effect_texel, call.effect_alpha_params0[i], call.effect_alpha_params1[i]);
        vec4 alpha_ctrl = call.effect_alpha_params2[i];
        if (alpha_ctrl.z > 0.5)
        {
            int alphaFunc = int(alpha_ctrl.x + 0.5);
            float alphaRef = clamp(alpha_ctrl.y, 0.0, 1.0);
            if (!AlphaTestPass(stageAlpha, alphaFunc, alphaRef))
                continue;
        }
        float finalStageAlpha = clamp(stageAlpha, 0.0, 1.0);
        vec3 stage_rgb = effect_texel.rgb * call.effect_color[i].rgb;
        int blendMode = int(call.effect_flags[i].y + 0.5);
        if (blendMode == IW_BLEND_PREMUL)
        {
            vec3 premul_rgb = stage_rgb * finalStageAlpha;
            result.rgb = premul_rgb + result.rgb * (1.0 - finalStageAlpha);
        }
        else
        {
            result.rgb += stage_rgb * finalStageAlpha;
        }
    }

    result.rgb += fullbright + emissive;

    // Specular clamp+Add
    vec3 spec_clamped = clamp_preserving_hue(specular_light, vec3(Overbright));
    result.rgb += spec_clamped * saturate(result.a);

    if ((in_flags & CF_TC_ENVMAP) != 0u)
    {
#if BINDLESS
        samplerCube EnvSampler = samplerCube(in_samplers2.xy);
#else
        samplerCube EnvSampler = EnvTex;
#endif
        vec3 normal = fastNorm(surface_normal);
        vec3 reflect_dir = reflect(-view_dir, normal);
        vec3 env_sample = texture(EnvSampler, reflect_dir).rgb;

        vec3 env_tint = in_env_params0.rgb;
        float env_amount = max(in_env_params0.w, 0.0);
        int rgbGen = int(in_env_params2.y + 0.5);
        int waveFunc = int(in_env_params2.x + 0.5);

        if (rgbGen == IW_RGB_IDENTITY || rgbGen == IW_RGB_VERTEX || rgbGen == IW_RGB_ENTITY)
        {
            env_tint = vec3(1.0);
        }
        else if (rgbGen != IW_RGB_CONST && rgbGen != IW_RGB_WAVE)
        {
            env_tint = vec3(1.0);
        }

        if (rgbGen == IW_RGB_WAVE)
        {
            float wave_value;
            if (waveFunc == IW_WAVE_FRESNEL)
            {
                float ndv = clamp(dot(normal, view_dir), 0.0, 1.0);
                float inv_ndv = 1.0 - ndv;
                float fresnel_power = in_env_params1.z;
                if (fresnel_power <= 0.0)
                    fresnel_power = 5.0;
                float fresnel = pow(clamp(inv_ndv, 0.0, 1.0), fresnel_power);
                wave_value = clamp(in_env_params1.x + in_env_params1.y * fresnel, 0.0, 1.0);
            }
            else
            {
                vec4 wave_params = vec4(in_env_params1.x, in_env_params1.y, in_env_params1.z, in_env_params1.w);
                wave_value = clamp(EvaluateWave(wave_params, waveFunc), 0.0, 1.0);
            }
            env_amount *= wave_value;
        }

        vec3 env_color = env_sample * env_tint;
        int blendMode = int(in_env_params2.z + 0.5);
        result.rgb = ApplyEnvBlend(result.rgb, env_color, env_amount, blendMode);
    }

    result = clamp(result, 0.0, 1.0);
    vec4 fogData = ((in_flags & CF_CUSTOM_FOG) != 0u) ? in_fog_color : Fog;
    result.rgb = ApplyFog(result.rgb, in_pos - EyePos, fogData);

    result.a = finalAlpha;

#if OIT
    OUT_COLOR = result; // Rest bleibt wie gehabt im OIT-Zweig
#else
    OUT_COLOR = result;
    // Motion Vectors nur für voll opak
    vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
    vec2 velocityOut = (result.a >= 0.999) ? (velocity * result.a) : vec2(0.0);
    out_velocity = vec4(velocityOut, 0.0, 0.0);
#endif

#if DITHER == 1
    vec3 dpos = fwidth(in_pos);
    float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);
    farblend *= farblend;
    OUT_COLOR.rgb = sqrt(OUT_COLOR.rgb);
    float luma = dot(OUT_COLOR.rgb, vec3(0.25, 0.625, 0.125));
    float nearnoise = tri(whitenoise01(lmuv * lmsize)) * luma * TextureDither;
    float farnoise  = (Fog.w > 0.0) ? SCREEN_SPACE_NOISE() * ScreenDither : 0.0;
    OUT_COLOR.rgb += mix(nearnoise, farnoise, farblend);
    OUT_COLOR.rgb *= OUT_COLOR.rgb;
#endif

#if DITHER >= 2
    OUT_COLOR.rgb = floor(OUT_COLOR.rgb * 255.0 + 0.5) * (1.0/255.0);
#elif DITHER == 0
    OUT_COLOR.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}
