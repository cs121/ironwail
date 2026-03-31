#include "frame_uniforms.glsl"

layout(location=0) flat in uint in_flags;
layout(location=1) flat in float in_alpha;
layout(location=2) in vec2 in_uv;
layout(location=3) in vec3 in_pos;
layout(location=6) noperspective in vec4 in_curr_clip;
layout(location=7) noperspective in vec4 in_prev_clip;

#define OUT_COLOR out_fragcolor
#if OIT
	vec4 OUT_COLOR;
	layout(location=0) out vec4 out_accum;
	layout(location=1) out float out_reveal;

	vec3 GammaToLinear(vec3 v)
	{
		return v;
	}

	void main_body();

	void main()
	{
		main_body();
		OUT_COLOR = clamp(OUT_COLOR, 0.0, 1.0);
		vec4 color = vec4(GammaToLinear(OUT_COLOR.rgb), OUT_COLOR.a);
		float z = 1.0 / gl_FragCoord.w;
		float weight = clamp(color.a * color.a * 0.03 / (1e-5 + pow(z / 1e7, 1.0)), 1e-2, 3e3);
		out_accum = vec4(color.rgb, color.a * weight);
		out_accum.rgb *= out_accum.a;
		out_reveal = color.a;
	}

	#define main main_body
#else
	layout(location=0) out vec4 OUT_COLOR;
	layout(location=1) out vec4 out_velocity;
#endif

vec2 ComputeVelocity(vec4 curr_clip, vec4 prev_clip)
{
	const float EPS = 1e-6;
	float inv_curr_w = abs(curr_clip.w) > EPS ? 1.0 / curr_clip.w : 0.0;
	float inv_prev_w = abs(prev_clip.w) > EPS ? 1.0 / prev_clip.w : 0.0;
	vec2 curr_ndc = curr_clip.xy * inv_curr_w;
	vec2 prev_ndc = prev_clip.xy * inv_prev_w;
	return (curr_ndc - prev_ndc) * 0.5;
}

vec3 hash(vec3 p)
{
	vec3 q = vec3(
		dot(p, vec3(127.1, 311.7, 109.2)),
		dot(p, vec3(269.5, 183.3, 432.6)),
		dot(p, vec3(419.2, 371.9, 304.4))
	);
	return fract(sin(q) * 43758.5453);
}

float noised(vec3 x)
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

	return va
		+ u.x * (vb - va)
		+ u.y * (vc - va)
		+ u.z * (ve - va)
		+ u.x * u.y * (va - vb - vc + vd)
		+ u.y * u.z * (va - vc - ve + vg)
		+ u.z * u.x * (va - vb - ve + vf)
		+ u.x * u.y * u.z * (-va + vb + vc - vd + ve - vf - vg + vh);
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

void main()
{
	vec2 uv = in_uv * 2.0 - 1.0;
	float r2 = dot(uv, uv);
	float a = sin(Time * 0.10) + cos(Time * 0.0331) * 3.14;
	float c = cos(a);
	float s = sin(a);
	mat2 R = mat2(c, s, -s, c);
	vec2 warp_uv = uv;
	vec3 color = vec3(0.0);
	vec3 dots = vec3(0.0);
	vec3 final_color;
	float t;
	float alpha;

	warp_uv += vec2(-uv.y, uv.x) * (0.12 * sin(10.0 * length(uv) - Time * 2.2));
	warp_uv += 0.055 * vec2(
		sin(uv.y * 12.0 + Time * 1.8),
		cos(uv.x * 11.0 - Time * 1.6)
	);

	{
		vec3 ro = vec3(0.0, 0.0, 3.0 + 2.0 * sin(Time * 0.5));
		vec3 rd = normalize(vec3(warp_uv, -2.0));

		ro.xy *= R;
		rd.xy *= R;
		ro.zx *= R;
		rd.zx *= R;

		t = 0.15 * fract(Time * 61.123 + r2);

		for (int i = 0; i < 20; ++i)
		{
			vec3 p = rd * t + ro;
			vec3 q = p;
			float Td = Time - length(p) * 0.15;

			for (float f = 0.08; f < 8.0; f += f)
			{
				p.xy *= R;
				p = sin(p * 0.2) * 4.25;
				p += sin(Td / f * 0.6 + p.xzy / f) * f * 0.07;
				p.zx *= R;
				p /= dot(p, p) + 0.08;
			}

			{
				float sdf = max(0.99 - length(p), abs(fbm(p) - 0.25));
				float dt;
				vec3 k;
				vec3 cmap;
				float kernel;

				sdf = max(sdf, -3.8 + length(q));
				dt = abs(sdf) * 0.44 + 0.0035;
				t += dt;

				k = 3.14 * sdf + vec3(0.375, 1.05, 1.4);
				cmap = 0.5 + 0.5 * cos(k * k);
				kernel = tanh(0.003 / dt);

				color += cmap * kernel;
				color += vec3(1.0, 3.0, 2.0) * (0.0012 / dot(q, q));
			}
		}
	}

	for (int i = 0; i < 10; ++i)
	{
		float fi = float(i);
		float ang = fi * 1.723 + sin(Time * 0.32 + fi * 0.71) * 0.55;
		float rad = 0.12 + 0.34 * fract(sin(fi * 19.37) * 43758.5453);
		vec2 center = vec2(cos(ang), sin(ang)) * rad;
		float d;
		float blob;
		float flicker;
		vec3 tint;

		center += 0.075 * vec2(
			sin(Time * (1.2 + 0.07 * fi) + fi * 2.1),
			cos(Time * (1.5 + 0.05 * fi) + fi * 1.3)
		);

		d = length(warp_uv - center);
		blob = exp(-28.0 * d * d);
		flicker = 0.65 + 0.35 * sin(Time * (3.0 + fi * 0.35) + fi * 4.13);
		tint = mix(vec3(0.85, 0.9, 1.0), vec3(1.0, 1.0, 1.0), fract(fi * 0.37));
		dots += tint * blob * flicker;
	}

	final_color = 1.0 - exp(-1.15 * sqrt(max(color, 0.0)));
	final_color = mix(vec3(dot(final_color, vec3(0.333))), final_color, 0.25);
	final_color *= vec3(0.45, 0.48, 0.5);
	final_color += dots * 0.9;
	final_color *= 1.0 - r2 * 0.23;
	final_color *= 0.58;
	final_color += vec3(0.01, 0.012, 0.015) * (1.0 - smoothstep(0.0, 1.0, r2));

	{
		float fog = exp2(-abs(Fog.w) * dot(in_pos - EyePos, in_pos - EyePos));
		fog = clamp(fog, 0.0, 1.0);
		alpha = clamp(max(max(final_color.r, final_color.g), final_color.b) * 1.15, 0.0, 1.0);
		alpha *= in_alpha * fog;
		final_color = mix(Fog.rgb, final_color, fog);
	}

	OUT_COLOR = vec4(final_color, alpha);

#if !OIT
	{
		vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
		vec2 velocity_out = vec2(0.0);
		if (alpha >= 0.999)
			velocity_out = velocity * alpha;
		out_velocity = vec4(velocity_out, 0.0, 2.0);
	}
#endif
}
