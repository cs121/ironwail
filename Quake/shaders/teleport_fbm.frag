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

float hash1(float n)
{
	return fract(sin(n) * 43758.5453123);
}

vec2 hash2(float n)
{
	return vec2(hash1(n), hash1(n + 17.371));
}

mat2 rot2(float a)
{
	float c = cos(a);
	float s = sin(a);
	return mat2(c, -s, s, c);
}

void main()
{
	vec2 uv = in_uv * 2.0 - 1.0;
	float r2 = dot(uv, uv);
	vec2 swirl_uv;
	vec3 color = vec3(0.0);
	float alpha;

	swirl_uv = rot2(0.12 * sin(Time * 0.45)) * uv;
	swirl_uv += 0.025 * vec2(
		sin(uv.y * 8.0 + Time * 0.9),
		cos(uv.x * 7.0 - Time * 0.8)
	);

	for (int i = 0; i < 12; ++i)
	{
		float fi = float(i);
		vec2 seed = hash2(fi * 11.173);
		float orbit = Time * (0.23 + 0.09 * seed.x) + 6.2831853 * seed.y;
		float drift = Time * (0.41 + 0.15 * hash1(fi * 5.91));
		float radius = 0.10 + 0.58 * hash1(fi * 9.17);
		float depth = 0.55 + 0.45 * hash1(fi * 13.7);
		float size = mix(0.030, 0.085, hash1(fi * 3.27)) / depth;
		float streak = mix(0.9, 1.6, hash1(fi * 15.41));
		vec2 center = vec2(cos(orbit), sin(orbit)) * radius;
		vec2 drift_dir = normalize(vec2(seed.x - 0.5, seed.y - 0.5) + vec2(0.0001, 0.0));
		vec2 d;
		float core;
		float halo;
		float flicker;
		float particle;

		center += drift_dir * sin(drift + fi) * (0.08 / depth);
		center += vec2(
			sin(Time * (0.55 + 0.07 * fi) + fi * 1.91),
			cos(Time * (0.48 + 0.05 * fi) + fi * 2.37)
		) * (0.035 / depth);

		d = swirl_uv - center;
		d.x *= streak;
		core = exp(-dot(d, d) / max(size * size, 1e-4));
		halo = exp(-dot(d, d) / max((size * 2.8) * (size * 2.8), 1e-4));
		flicker = 0.70 + 0.30 * sin(Time * (2.2 + seed.x * 2.0) + fi * 4.1);
		particle = (core + 0.35 * halo) * depth * flicker;
		color += vec3(particle);
	}

	color *= 0.82;
	color *= 1.0 - smoothstep(0.55, 1.20, r2);
	color += vec3(0.02) * (1.0 - smoothstep(0.0, 0.75, r2));
	color = min(color, vec3(1.0));

	{
		float fog = exp2(-abs(Fog.w) * dot(in_pos - EyePos, in_pos - EyePos));
		fog = clamp(fog, 0.0, 1.0);
		alpha = clamp(max(max(color.r, color.g), color.b) * 1.1, 0.0, 1.0);
		alpha *= in_alpha * fog;
		color = mix(Fog.rgb, color, fog);
	}

	OUT_COLOR = vec4(color, alpha);

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
