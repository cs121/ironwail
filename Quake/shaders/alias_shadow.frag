#include "frame_uniforms.glsl"

layout(location=0) in vec4 in_color;
layout(location=1) in vec3 in_pos;

layout(location=0) out vec4 out_fragcolor;

void main()
{
        float fog = exp2(abs(Fog.w) * -dot(in_pos, in_pos));
        fog = clamp(fog, 0.0, 1.0);

        vec3 base = mix(Fog.rgb, in_color.rgb, fog);
        float alpha = 0.25 * fog;

        out_fragcolor = clamp(vec4(base, alpha), 0.0, 1.0);
}
