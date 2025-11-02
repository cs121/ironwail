#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p)
{
        float fog = exp2(-Fog.w * dot(p, p));
        fog = clamp(fog, 0.0, 1.0);
        return mix(Fog.rgb, clr, fog);
}

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

layout(binding=0) uniform sampler2D Tex;

layout(location=0) in vec2 in_uv;
layout(location=1) in vec3 in_pos;
layout(location=2) in vec4 in_color;
layout(location=3) in vec3 in_normal;
layout(location=4) in float in_spec;

layout(location=0) out vec4 out_fragcolor;
layout(location=1) out vec4 out_velocity;

void main()
{
        vec4 texel = texture(Tex, in_uv);
        vec4 result = texel * in_color;
        if (result.a < 0.01)
                discard;
        if (in_spec > 0.0)
        {
                float normal_len2 = dot(in_normal, in_normal);
                float view_len2 = dot(in_pos, in_pos);
                if (normal_len2 > 0.0 && view_len2 > 0.0)
                {
                        vec3 normal = in_normal * inversesqrt(normal_len2);
                        vec3 view_dir = -in_pos * inversesqrt(view_len2);
                        vec3 light_dir = normalize(vec3(0.25, 0.45, 1.0));
                        vec3 half_vec = light_dir + view_dir;
                        float half_len2 = dot(half_vec, half_vec);
                        if (half_len2 > 0.0)
                        {
                                half_vec *= inversesqrt(half_len2);
                                float ndoth = max(dot(normal, half_vec), 0.0);
                                float spec_term = pow(ndoth, 24.0) * in_spec * result.a;
                                vec3 spec_color = mix(result.rgb, vec3(1.0), 0.7);
                                result.rgb = clamp(result.rgb + spec_term * spec_color, 0.0, 1.0);
                        }
                }
        }
        result.rgb = ApplyFog(result.rgb, in_pos);
        out_fragcolor = result;
        out_velocity = vec4(0.0);
#if DITHER
        if (Fog.w > 0.)
        {
                out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
                out_fragcolor.rgb += SCREEN_SPACE_NOISE() * ScreenDither;
                out_fragcolor.rgb *= out_fragcolor.rgb;
        }
#else
        out_fragcolor.rgb += SUPPRESS_BANDING() * ScreenDither;
#endif
}
