#if BINDLESS
#extension GL_ARB_bindless_texture : require
#else
layout(binding=0) uniform sampler2D Tex;
#endif
layout(binding=2) uniform sampler2D LMTex; // unused, kept for binding slot consistency
layout(binding=3) uniform sampler2D LMTexDir; // unused
layout(binding=5) uniform sampler2D ShadowDlightMap;

#include "frame_uniforms.glsl"
#define SHADOW_DLIGHT 1
#include "shadow_sample.glsl"

#ifndef ALPHATEST
#define ALPHATEST 0
#endif

layout(location=0) uniform vec4 DLightConfig0; // x: scale, y: radius scale, z: falloff mode, w: exp
layout(location=1) uniform vec4 DLightConfig1; // x: core boost, y: core exp, z: soft knee, w: ndotl mix
layout(location=2) uniform vec4 DLightConfig2; // x: saturation chop

vec3 ApplyFog(vec3 clr, vec3 p)
{
        float fog = exp2(-abs(Fog.w) * dot(p, p));
        fog = clamp(fog, 0.0, 1.0);
        return mix(Fog.rgb, clr, fog);
}

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
        vec2    LightStyles[64];
        Light   Lights[];
};

layout(rg32ui, binding=0) uniform readonly uimage3D LightClusters;

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

float ComputeSpecEnergy(vec3 surface_normal, vec3 light_dir, vec3 view_dir, float ndotl, float quality, float energy)
{
        vec3 half_vec = normalize(light_dir + view_dir);
        float ndoth = max(dot(surface_normal, half_vec), 0.0);
        float spec_power = mix(8.0, 24.0, quality);
        float spec = pow(ndoth, spec_power) * ndotl;
        return spec * energy * (0.35 * quality);
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

        vec3 dynamic_light = vec3(0.0);
        vec3 specular_light = vec3(0.0);
        float quality = clamp(ClusteredLightParams.w / 3.0, 0.25, 1.0);
        vec3 view_dir = normalize(EyePos - in_pos);
        if (NumLights > 0u)
        {
                // in_depth is provided as absolute view-space Z from the vertex shader.
                // Convert that linear view depth into clustered logarithmic Z-slice space.
                ivec3 cluster_coord = ivec3(
                        int(floor(in_coord.x)),
                        int(floor(in_coord.y)),
                        int(floor(log2(max(in_depth, 1e-4)) * ZLogScale + ZLogBias))
                );

                // Clamp cluster coordinates before imageLoad to avoid undefined out-of-bounds access.
                ivec3 cluster_max = ivec3(LIGHT_TILES_X - 1, LIGHT_TILES_Y - 1, LIGHT_TILES_Z - 1);
                cluster_coord = clamp(cluster_coord, ivec3(0), cluster_max);
                uvec2 clusterdata = imageLoad(LightClusters, cluster_coord).xy;

                if ((clusterdata.x | clusterdata.y) != 0u)
                {
                        float dynamic_light_noise = 1.0 - whitenoise01(in_pos.xy) * 0.15;
                        float falloff_mode = DLightConfig0.z;
                        float falloff_exp = max(DLightConfig0.w, 0.01);
                        float core_boost = max(DLightConfig1.x, 0.0);
                        float core_exp = max(DLightConfig1.y, 0.01);
                        float knee = max(DLightConfig1.z, 0.0);
                        float ndotl_mix = clamp(DLightConfig1.w, 0.0, 1.0);

                        for (uint i = 0u, ofs = 0u; i < 2u; i++, ofs += 32u)
                        {
                                uint mask = clusterdata[i];
                                while (mask != 0u)
                                {
                                        // FIX: mask &= mask - 1u loescht das niederwertigste Bit
                                        // sicher ohne Shift-UB (kein "1u << int_j" mehr noetig).
                                        uint j = uint(findLSB(mask));
                                        mask &= mask - 1u;
                                        Light l = Lights[ofs + j];

                                        float rad = l.radius;

                                        // FIX: light_vec vom echten Lichtpunkt (nicht vom
                                        // auf die Oberflaeche projizierten), damit surface_dist
                                        // den tatsaechlichen 3D-Abstand wiedergibt.
                                        vec3 light_vec = l.origin - in_pos;
                                        float surface_dist = length(light_vec);

                                        // Planarer Abstand des Lichts von der Oberflaeche
                                        // (fuer Radius-Abzug und minlight-Pruefung).
                                        float dist = dot(l.origin - in_pos, surface_normal);
                                        rad -= abs(dist);
                                        float minlight = l.minlight;

                                        if (rad <= 0.0 || rad < minlight)
                                                continue;

                                        minlight = rad - minlight;
                                        float attenuation = clamp((minlight - surface_dist) / 16.0, 0.0, 1.0);
                                        float normalized_dist = surface_dist / rad;
                                        float x = clamp(1.0 - clamp(normalized_dist, 0.0, 1.0), 0.0, 1.0);
                                        float falloff = ComputeFalloff(x, falloff_mode, falloff_exp);
                                        float intensity = attenuation * falloff;
                                        float core = 1.0 + core_boost * pow(max(x, 0.0), core_exp);
                                        float core_intensity = intensity * core;
                                        float shaped = (knee > 0.0) ? (core_intensity / (core_intensity + knee)) : core_intensity;
                                        float ndotl = 1.0;
                                        vec3 light_dir = vec3(0.0, 0.0, 1.0);
                                        if (surface_dist > 0.0)
                                        {
                                                light_dir = light_vec / surface_dist;
                                                float ndotl_raw = max(dot(surface_normal, light_dir), 0.0);
                                                ndotl = mix(1.0, ndotl_raw, ndotl_mix);
                                        }

                                        // FIX: shadow_range = l.radius statt fest 1.0,
                                        // damit ShadowVisibilityDlight korrekt skaliert.
                                        float shadow_range = l.radius;
                                        float shadow_term = ShadowVisibilityDlight(in_pos, surface_normal, l.origin, ofs + j, shadow_range);
                                        vec3 light_contrib = shaped * ndotl * shadow_term * l.color * dynamic_light_noise;
                                        dynamic_light += light_contrib;
                                        if (surface_dist > 0.0)
                                        {
                                                // FIX: energy aus geometrischer Intensitaet (shaped * ndotl * shadow_term),
                                                // nicht aus der farbgewichteten light_contrib, um Verzerrung durch
                                                // Lichtfarbe im Spekularbeitrag zu vermeiden.
                                                float energy = min(1.0, shaped * ndotl * shadow_term);
                                                float spec = ComputeSpecEnergy(surface_normal, light_dir, view_dir, ndotl, quality, energy);
                                                specular_light += l.color * spec * shadow_term;
                                        }
                                }
                        }
                }
        }

        float satchop = clamp(DLightConfig2.x, 0.0, 1.0);
        if (satchop > 0.0)
        {
                float luma = dot(dynamic_light, vec3(0.299, 0.587, 0.114));
                dynamic_light = mix(dynamic_light, vec3(luma), satchop);
        }

        vec3 contrib = clamp(dynamic_light * DLightConfig0.x * Overbright, 0.0, Overbright);
        vec3 spec_budget = max(vec3(0.0), vec3(Overbright) - contrib);
        vec3 spec_contrib = min(max(specular_light, vec3(0.0)), spec_budget);
        vec3 color = albedo * contrib + spec_contrib;

        color = ApplyFog(color, in_pos - EyePos);

        // FIX: alpha (texel.a * in_alpha) korrekt in out_fragcolor schreiben,
        // statt fest 0.0 -- sonst geht jede Transparenz (z.B. Wasser) verloren.
        out_fragcolor = vec4(color, alpha);
        out_velocity = vec4(0.0, 0.0, 0.0, 1.0);
}
