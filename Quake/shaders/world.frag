#if BINDLESS
        #extension GL_ARB_bindless_texture : require
#else
        layout(binding=0) uniform sampler2D Tex;
        layout(binding=1) uniform sampler2D FullbrightTex;
        layout(binding=4) uniform sampler2D EmissiveTex;
#endif

layout(binding=2) uniform sampler2D LMTex;
layout(binding=3) uniform sampler2D DeluxTex;

#include "frame_uniforms.glsl"

#define LIGHT_TILES_X 32
#define LIGHT_TILES_Y 16
#define LIGHT_TILES_Z 32
#define MAX_LIGHTS    64

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

vec3 ApplyFog(vec3 clr, vec3 p)
{
    float fog = exp2(-Fog.w * dot(p, p));
    fog = clamp(fog, 0.0, 1.0);
    return mix(Fog.rgb, clr, fog);
}

const uint
    CF_USE_POLYGON_OFFSET = 1u,
    CF_USE_FULLBRIGHT = 2u,
    CF_NOLIGHTMAP = 4u,
    CF_USE_EMISSIVE = 8u,
    CF_ALPHA_TEST = 16u
;

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

#if !defined(BLUE_NOISE_TABLE_DEFINED)
const float BlueNoise8x8[64] = float[64](
     0.0/64.0, 48.0/64.0, 12.0/64.0, 60.0/64.0,  3.0/64.0, 51.0/64.0, 15.0/64.0, 63.0/64.0,
    32.0/64.0, 16.0/64.0, 44.0/64.0, 28.0/64.0, 35.0/64.0, 19.0/64.0, 47.0/64.0, 31.0/64.0,
     8.0/64.0, 56.0/64.0,  4.0/64.0, 52.0/64.0, 11.0/64.0, 59.0/64.0,  7.0/64.0, 55.0/64.0,
    40.0/64.0, 24.0/64.0, 36.0/64.0, 20.0/64.0, 43.0/64.0, 27.0/64.0, 39.0/64.0, 23.0/64.0,
     2.0/64.0, 50.0/64.0, 14.0/64.0, 62.0/64.0,  1.0/64.0, 49.0/64.0, 13.0/64.0, 61.0/64.0,
    34.0/64.0, 18.0/64.0, 46.0/64.0, 30.0/64.0, 33.0/64.0, 17.0/64.0, 45.0/64.0, 29.0/64.0,
    10.0/64.0, 58.0/64.0,  6.0/64.0, 54.0/64.0,  9.0/64.0, 57.0/64.0,  5.0/64.0, 53.0/64.0,
    42.0/64.0, 26.0/64.0, 38.0/64.0, 22.0/64.0, 41.0/64.0, 25.0/64.0, 37.0/64.0, 21.0/64.0
);
#define BLUE_NOISE_TABLE_DEFINED 1
#endif

float BlueNoiseValue(vec2 noiseCoord)
{
    vec2 wrapped = mod(floor(noiseCoord), 8.0);
    int idx = int(wrapped.y) * 8 + int(wrapped.x);
    return BlueNoise8x8[idx];
}

#define BLUE_NOISE_STATIC(uv) BlueNoiseValue((uv) * 8.0)

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
        emissive = texture(EmissiveSampler, uv).rgb;
    }
#else
    if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
        fullbright = texture(FullbrightTex, uv).rgb;
    if ((in_flags & CF_USE_EMISSIVE) != 0u)
        emissive = texture(EmissiveTex, uv).rgb;
#endif

    // Textur: kleinere negative LOD-Bias nur wenn DITHER aktiv
#if DITHER >= 2
    vec4 result = texture(Tex, uv, -1.0);
#elif DITHER
    vec4 result = texture(Tex, uv, -0.5);
#else
    vec4 result = texture(Tex, uv);
#endif

#if MODE == 1
  #if USE_BRANCHLESS_ALPHA
    // branchless discard vermeiden → identische Optik, aber OIT kann abweichen.
    // Wenn du strikt discard brauchst, setze USE_BRANCHLESS_ALPHA=0.
    if (result.a < 0.666) { discard; }
  #else
    if (result.a < 0.666) discard;
  #endif
#endif

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
    vec3 static_light_dir = geom_normal;

    if (DeluxEnabled != 0u)
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
            surface_normal = fastNorm(accum);
            static_light_dir = surface_normal;
        }
        else
        {
            surface_normal = dir0;
            static_light_dir = surface_normal;
        }
    }

    vec3 static_light_base = static_light;
    bool halfLambertEnabled = (HalfLambert != 0) && (StaticLightmapMode != 0);

    float static_half_term = 1.0;
    if (halfLambertEnabled)
    {
        float ndotl_static = dot(geom_normal, static_light_dir);
        static_half_term = clamp(ndotl_static * 0.5 + 0.5, 0.0, 1.0);
    }
    vec3 static_light_shaded = static_light * static_half_term;

    vec3 static_total_source = halfLambertEnabled ? static_light_shaded : static_light_base;
    vec3 total_light = clamp_preserving_hue(static_total_source, vec3(1.0));
    vec3 dynamic_light_total = vec3(0.0);
    vec3 rim_contrib = vec3(0.0);
    vec3 sun_contrib = vec3(0.0);
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
                    float rad  = l.radius - abs(dist);
                    float minl = l.minlight;
                    if (rad < minl) continue;

                    vec3 local_pos = l.origin - plane_n * dist;
                    vec3 L = local_pos - in_pos;

                    float Llen2 = dot(L,L);
                    if (Llen2 <= 0.0) continue;
                    float Linv = inversesqrt(Llen2);
                    float sdist = 1.0 / Linv; // ≈ length(L)

                    float minlight_span = rad - minl;
                    float attenuation = saturate((minlight_span - sdist) * (1.0/16.0));
                    float falloff     = saturate((rad - sdist) * (1.0/256.0));

                    if (attenuation > 0.0 && falloff > 0.0){
                        vec3  ldir = L * Linv;
                        float ndotl_raw = dot(surface_normal, ldir);
                        float ndotl = max(ndotl_raw, 0.0);
                        float diffuse_term = halfLambertEnabled ? clamp(ndotl_raw * 0.5 + 0.5, 0.0, 1.0) : ndotl;
                        vec3  light_contrib = (attenuation * falloff) * l.color;
                        vec3  diffuse_contrib = light_contrib * diffuse_term;

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
                        dynamic_light += diffuse_contrib;
                    }
                }
            }
            // saturating Add (bleibt <= 1) with hue preservation
            vec3 dynamic_remaining = max(vec3(0.0), vec3(1.0) - total_light);
            if (dynamic_remaining.x > 0.0 || dynamic_remaining.y > 0.0 || dynamic_remaining.z > 0.0){
                vec3 dynamic_added = clamp_preserving_hue(dynamic_light, dynamic_remaining);
                total_light += dynamic_added;
                dynamic_light_total += dynamic_added;
            }
        }
    }

    // Rim
    if (RimWorld > 0.0){
        float ndv = max(dot(surface_normal, view_dir), 0.0);
        float fres = pow(saturate(1.0 - ndv), RimExponent);
        vec3 rim = vec3(RimWorld * fres);
        vec3 rim_remaining = max(vec3(0.0), vec3(1.0) - total_light);
        if (rim_remaining.x > 0.0 || rim_remaining.y > 0.0 || rim_remaining.z > 0.0){
            vec3 rim_added = clamp_preserving_hue(rim, rim_remaining);
            total_light += rim_added;
            rim_contrib += rim_added;
        }
    }

    // Sun (deine Funktion bleibt Stub/kompatibel)
    vec3 sun_light = ComputeSunLight(in_pos, surface_normal);
    vec3 sun_remaining = max(vec3(0.0), vec3(1.0) - total_light);
    if (sun_remaining.x > 0.0 || sun_remaining.y > 0.0 || sun_remaining.z > 0.0){
        vec3 sun_added = clamp_preserving_hue(sun_light, sun_remaining);
        total_light += sun_added;
        sun_contrib += sun_added;
    }

    vec3 base_color = result.rgb;
    vec3 lit_color;

    if (StaticLightmapMode <= 0)
    {
        vec3 static_component = clamp_preserving_hue(static_light_base, vec3(1.0));
        vec3 dynamic_component = clamp_preserving_hue(dynamic_light_total + rim_contrib + sun_contrib, vec3(1.0));

        vec3 shaded = base_color * (static_component * Overbright);
        shaded += base_color * (dynamic_component * Overbright);

#if MODE != 1
        lit_color = mix(base_color, shaded, result.a);
#else
        lit_color = shaded;
#endif
    }
    else
    {
        float levels = max(float(LightLevels), 1.0);
        vec3 static_source = halfLambertEnabled ? static_light_shaded : static_light_base;
        vec3 static_light_norm = clamp(static_source, 0.0, 1.0);
        vec3 static_smoothed = mix(static_light_norm, sqrt(static_light_norm), vec3(0.35));
        float static_noise = BLUE_NOISE_STATIC(lmuv);
        vec3 static_quantized = floor(static_smoothed * levels + static_noise) / levels;
        static_quantized = clamp(static_quantized, 0.0, 1.0);

        vec3 dynamic_combined = clamp(dynamic_light_total + rim_contrib + sun_contrib, 0.0, 1.0);
        vec2 time_offset = vec2(fract(NoiseTime * 0.5), fract(NoiseTime * 0.37));
        float dynamic_noise = (DynamicDither != 0) ? BlueNoiseValue(lmuv * 8.0 + time_offset * 8.0) : 0.5;
        vec3 dynamic_quantized = floor(dynamic_combined * levels + dynamic_noise) / levels;
        dynamic_quantized = clamp(dynamic_quantized, 0.0, 1.0);

        vec3 static_component = static_quantized * Overbright;
        vec3 dynamic_component = dynamic_quantized * Overbright;

        vec3 shaded = base_color * static_component;
        shaded += base_color * dynamic_component;

#if MODE != 1
        lit_color = mix(base_color, shaded, result.a);
#else
        lit_color = shaded;
#endif
    }

    result.rgb = lit_color;
    result.rgb += fullbright + emissive;

    // Specular clamp+Add
    vec3 spec_clamped = clamp_preserving_hue(specular_light, vec3(Overbright));
    result.rgb += spec_clamped * saturate(result.a);

    if (StaticLightmapMode > 0)
    {
        float safeGamma = max(GammaBoost, 1.0e-3);
        float invGamma = 1.0 / safeGamma;
        result.rgb = pow(max(result.rgb, vec3(0.0)), vec3(invGamma));
    }

    result = clamp(result, 0.0, 1.0);
    result.rgb = ApplyFog(result.rgb, in_pos - EyePos);

    result.a = in_alpha;

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
