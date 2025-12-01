#if BINDLESS
        #extension GL_ARB_bindless_texture : require
#else
        layout(binding=0) uniform sampler2D Tex;
#endif
layout(binding=2) uniform sampler2D LMTex;
layout(binding=3) uniform sampler2D LMTexDir;
#include "frame_uniforms.glsl"

layout(std430, binding=0) restrict readonly buffer LightBuffer
{
        vec2    LightStyles[64];
};

float GetLightStyle(int index)
{
        float result;
        if (index < 64)
                result = mix(LightStyles[index].x, LightStyles[index].y, LightmapParams.w);
        else
                result = 1.0;
        return result;
}

vec4 SampleLightmap(vec2 uv)
{
        vec4 lm = texture(LMTex, uv);
        return vec4(lm.rgb * 2., lm.a);
}

vec3 SampleLightmapDir(vec2 uv)
{
        vec3 dir = texture(LMTexDir, uv).xyz * 2.0 - 1.0;
        return dir;
}

layout(location=0) flat in uint in_flags;
layout(location=1) in vec3 in_pos;
layout(location=2) in vec3 in_normal;
layout(location=3) in vec2 in_uv;
layout(location=4) in vec2 in_lmuv;
layout(location=5) flat in vec4 in_styles;
layout(location=6) flat in float in_lmofs;
#if BINDLESS
        layout(location=7) flat in uvec4 in_samplers0;
#endif

layout(location=0) out vec4 out_albedo;
layout(location=1) out vec4 out_normal_spec;

void main()
{
#if BINDLESS
        sampler2D Tex = sampler2D(in_samplers0.xy);
#endif
        vec4 result = texture(Tex, in_uv);

        bool linear_lightmaps = LightmapParams.x > 0.5;
        if (!linear_lightmaps)
                result.rgb = pow(result.rgb, vec3(2.2));

        vec2 lmuv = in_lmuv;
        vec4 lm0 = SampleLightmap(lmuv);
        vec3 static_light;
        if (in_styles.y < 0.)
                static_light = in_styles.x * lm0.xyz;
        else
        {
                vec4 lm1 = SampleLightmap(vec2(lmuv.x + in_lmofs, lmuv.y));
                if (in_styles.z < 0.)
                {
                        static_light =
                                in_styles.x * lm0.xyz +
                                in_styles.y * lm1.xyz;
                }
                else
                {
                        vec4 lm2 = SampleLightmap(vec2(lmuv.x + in_lmofs * 2., lmuv.y));
                        static_light = vec3
                        (
                                dot(in_styles, lm0),
                                dot(in_styles, lm1),
                                dot(in_styles, lm2)
                        );
                }
        }

        if ((in_flags & 4u) != 0u)
                static_light = vec3(1.0);

        if (LightmapParams.z > 0.5)
        {
                vec3 dir = SampleLightmapDir(lmuv);
                float ndl = max(dot(dir, normalize(in_normal)), 0.0);
                static_light *= ndl;
        }

        vec3 total_light = clamp(static_light, 0.0, 1.0);
        vec3 total_lightmap = clamp(total_light * Overbright, 0.0, Overbright);
        result.rgb *= total_lightmap;

        if (!linear_lightmaps)
                result.rgb = pow(result.rgb, vec3(1.0 / 2.2));

        out_albedo = vec4(result.rgb, result.a);
        vec3 nrm = normalize(in_normal);
        if (!gl_FrontFacing)
                nrm = -nrm;
        out_normal_spec = vec4(nrm, 32.0);
}
