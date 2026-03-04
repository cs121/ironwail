// Quake/shaders/glsl/teleport_fbm.frag
//
// Teleporter surface shader adapted from the provided 3D fBM/noised effect.

#version 330 core

in vec2 v_texcoord;
out vec4 fragColor;

uniform float u_time;

// --- Utility -----------------------------------------------------------------

vec3 hash(vec3 p)
{
    vec3 q = vec3(
        dot(p, vec3(127.1, 311.7, 109.2)),
        dot(p, vec3(269.5, 183.3, 432.6)),
        dot(p, vec3(419.2, 371.9, 304.4))
    );
    return fract(sin(q) * 43758.5453);
}

float noised(in vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);

    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    vec3 ga = hash(i + vec3(0.0, 0.0, 0.0));
    vec3 gb = hash(i + vec3(1.0, 0.0, 0.0));
    vec3 gc = hash(i + vec3(0.0, 1.0, 0.0));
    vec3 gd = hash(i + vec3(1.0, 1.0, 0.0));
    vec3 ge = hash(i + vec3(0.0, 0.0, 1.0));
    vec3 gf = hash(i + vec3(1.0, 0.0, 1.0));
    vec3 gg = hash(i + vec3(0.0, 1.0, 1.0));
    vec3 gh = hash(i + vec3(1.0, 1.0, 1.0));

    float va = dot(ga, f - vec3(0.0, 0.0, 0.0));
    float vb = dot(gb, f - vec3(1.0, 0.0, 0.0));
    float vc = dot(gc, f - vec3(0.0, 1.0, 0.0));
    float vd = dot(gd, f - vec3(1.0, 1.0, 0.0));
    float ve = dot(ge, f - vec3(0.0, 0.0, 1.0));
    float vf = dot(gf, f - vec3(1.0, 0.0, 1.0));
    float vg = dot(gg, f - vec3(0.0, 1.0, 1.0));
    float vh = dot(gh, f - vec3(1.0, 1.0, 1.0));

    float v = va
        + u.x * (vb - va)
        + u.y * (vc - va)
        + u.z * (ve - va)
        + u.x * u.y * (va - vb - vc + vd)
        + u.y * u.z * (va - vc - ve + vg)
        + u.z * u.x * (va - vb - ve + vf)
        + u.x * u.y * u.z * (-va + vb + vc - vd + ve - vf - vg + vh);

    return v;
}

float fbm(vec3 x)
{
    float a = 1.0;
    float t = 0.0;
    for (int i = 0; i < 7; ++i)
    {
        t += a * noised(x);
        x *= 2.0;
        a *= 0.5;
    }
    return t;
}

// --- Main --------------------------------------------------------------------

void main()
{
    vec2 uv = v_texcoord * 2.0 - 1.0;

    float T = u_time;
    float a = sin(T * 0.10) + cos(T * 0.0331) * 3.14;
    float c = cos(a);
    float s = sin(a);
    mat2 R = mat2(c, s, -s, c);

    vec3 ro = vec3(0.0, 0.0, 3.0 + 2.0 * sin(T * 0.5));
    vec3 rd = normalize(vec3(uv, -2.0));

    ro.xy *= R;
    rd.xy *= R;
    ro.zx *= R;
    rd.zx *= R;

    vec3 color = vec3(0.0);
    float t = 0.15 * fract(T * 61.123 + dot(uv, uv));

    for (int i = 0; i < 20; ++i)
    {
        vec3 p = rd * t + ro;
        vec3 q = p;

        float Td = T - length(p) * 0.15;

        for (float f = 0.08; f < 8.0; f += f)
        {
            p.xy *= R;
            p = sin(p * 0.2) * 4.25;
            p += sin(Td / f * 0.6 + p.xzy / f) * f * 0.07;
            p.zx *= R;
            p /= dot(p, p) + 0.08;
        }

        float sdf = max(0.99 - length(p), abs(fbm(p) - 0.25));
        sdf = max(sdf, -3.8 + length(q));
        float dt = abs(sdf) * 0.44 + 0.0035;
        t += dt;

        vec3 k = 3.14 * sdf + vec3(0.375, 1.05, 1.4);
        vec3 cmap = 0.5 + 0.5 * cos(k * k);
        float kernel = tanh(0.003 / dt);

        color += cmap * kernel;
        color += vec3(1.0, 3.0, 2.0) * (0.0012 / dot(q, q));
    }

    color = 1.0 - exp(-1.2 * sqrt(color * color * color));
    color *= 1.0 - dot(uv, uv) * 0.2;
    float alpha = clamp(max(max(color.r, color.g), color.b), 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
