#include "frame_uniforms.glsl"

layout(location=0) in vec4 in_color;
layout(location=1) in vec3 in_pos;

layout(location=0) out vec4 out_fragcolor;

float ScreenNoise(vec2 p)
{
        const vec2 k = vec2(12.9898, 78.233);
        return fract(sin(dot(p, k)) * 43758.5453);
}

void main()
{
        float fog = exp2(abs(Fog.w) * -dot(in_pos, in_pos));
        fog = clamp(fog, 0.0, 1.0);

        vec3 base = mix(Fog.rgb, in_color.rgb, fog);
        float alpha = in_color.a * fog;

        float noise = ScreenNoise(gl_FragCoord.xy);
        base += (noise - 0.5) * ScreenDither;

        out_fragcolor = clamp(vec4(base, alpha), 0.0, 1.0);
}
