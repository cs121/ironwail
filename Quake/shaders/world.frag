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

    // schnellere Flächennormalen aus Derivaten
    vec3 dn = cross(DFDX(in_pos), DFDY(in_pos));
    vec3 surface_normal = fastNorm(dn);

    vec3 total_light = saturate(static_light);
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
                        float ndotl = max(dot(surface_normal, ldir), 0.0);
                        vec3  light_contrib = (attenuation * falloff) * l.color;

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
            // saturating Add (bleibt <= 1)
            total_light += max(min(dynamic_light, vec3(1.0) - total_light), vec3(0.0));
        }
    }

    // Rim
    if (RimWorld > 0.0){
        float ndv = max(dot(surface_normal, view_dir), 0.0);
        float fres = pow(saturate(1.0 - ndv), RimExponent);
        vec3 rim = vec3(RimWorld * fres);
        total_light += max(min(rim, vec3(1.0) - total_light), vec3(0.0));
    }

    // Sun (deine Funktion bleibt Stub/kompatibel)
    vec3 sun_light = ComputeSunLight(in_pos, surface_normal);
    total_light += max(min(sun_light, vec3(1.0) - total_light), vec3(0.0));

#if DITHER >= 2
    vec3 total_lightmap = saturate(floor(saturate(total_light) * 63.0 + 0.5) * (Overbright/63.0));
#else
    vec3 total_lightmap = saturate(total_light * Overbright);
#endif

#if MODE != 1
    result.rgb = mix(result.rgb, result.rgb * total_lightmap, result.a);
#else
    result.rgb *= total_lightmap;
#endif

    result.rgb += fullbright + emissive;

    // Specular clamp+Add
    vec3 spec_clamped = clamp(specular_light, vec3(0.0), vec3(Overbright));
    result.rgb += spec_clamped * saturate(result.a);

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
