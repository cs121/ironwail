layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_fragcolor;

void main()
{
    vec2 p = floor(gl_FragCoord.xy / 32.0);
    float c = mod(p.x + p.y, 2.0);
    float v = mix(0.2, 1.0, c);
    out_fragcolor = vec4(v, v, v, 1.0);
}
