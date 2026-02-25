layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_fragcolor;

layout(binding=0) uniform sampler2D Tex;
layout(location=0) uniform int u_mode;           // 0 = checker, 1 = depth visualization
layout(location=1) uniform vec4 u_region_uv;     // xy=min, zw=max

void main()
{
    if (u_mode == 0)
    {
        vec2 p = floor(gl_FragCoord.xy / 32.0);
        float c = mod(p.x + p.y, 2.0);
        float v = mix(0.2, 1.0, c);
        out_fragcolor = vec4(v, v, v, 1.0);
        return;
    }

    vec2 uv = mix(u_region_uv.xy, u_region_uv.zw, clamp(v_uv, 0.0, 1.0));
    float depth = texture(Tex, uv).r;
    out_fragcolor = vec4(depth, depth, depth, 1.0);
}
