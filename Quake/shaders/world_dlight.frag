#if BINDLESS
#extension GL_ARB_bindless_texture : require
#else
layout(binding=0) uniform sampler2D Tex;
#endif
layout(binding=2) uniform sampler2D LMTex; // unused, kept for binding slot consistency
layout(binding=3) uniform sampler2D LMTexDir; // unused

#include "frame_uniforms.glsl"

#ifndef ALPHATEST
#define ALPHATEST 0
#endif

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
                out_velocity = vec4(0.0);
                return;
        }
        vec3 surface_normal = normalize(in_normal);
        if (!gl_FrontFacing)
                surface_normal = -surface_normal;

        vec3 dynamic_light = vec3(0.0);
        if (NumLights > 0u)
        {
                ivec3 cluster_coord = ivec3(
                        int(floor(in_coord.x)),
                        int(floor(in_coord.y)),
                        int(floor(log2(in_depth) * ZLogScale + ZLogBias))
                );

                uvec2 clusterdata = imageLoad(LightClusters, cluster_coord).xy;

                if ((clusterdata.x | clusterdata.y) != 0u)
                {
                        float dynamic_light_noise = 1.0 - whitenoise01(in_pos.xy) * 0.15;
                        vec4 plane = vec4(surface_normal, dot(in_pos, surface_normal));

                        for (uint i = 0u, ofs = 0u; i < 2u; i++, ofs += 32u)
                        {
                                uint mask = clusterdata[i];
                                while (mask != 0u)
                                {
                                        int j = findLSB(mask);
                                        mask ^= 1u << j;
                                        Light l = Lights[ofs + uint(j)];

                                        float rad = l.radius;
                                        float dist = dot(l.origin, plane.xyz) - plane.w;
                                        rad -= abs(dist);
                                        float minlight = l.minlight;

                                        if (rad <= 0.0 || rad < minlight)
                                                continue;

                                        vec3 local_pos = l.origin - plane.xyz * dist;
                                        minlight = rad - minlight;
                                        vec3 light_vec = local_pos - in_pos;
                                        float surface_dist = length(light_vec);
                                        float attenuation = clamp((minlight - surface_dist) / 16.0, 0.0, 1.0);
                                        float normalized_dist = surface_dist / rad;
                                        float falloff = pow(1.0 - clamp(normalized_dist, 0.0, 1.0), 1.5);
                                        if (surface_dist > 0.0)
                                        {
                                                vec3 light_dir = light_vec / surface_dist;
                                                float ndotl = max(dot(surface_normal, light_dir), 0.0);
                                                vec3 light_contrib = attenuation * falloff * l.color * dynamic_light_noise * ndotl;
                                                dynamic_light += light_contrib;
                                        }
                                }
                        }
                }
        }

        vec3 contrib = clamp(dynamic_light * Overbright, 0.0, Overbright);
        vec3 color = albedo * contrib;

        color = ApplyFog(color, in_pos - EyePos);

        out_fragcolor = vec4(color, 0.0);
        out_velocity = vec4(0.0);
}
