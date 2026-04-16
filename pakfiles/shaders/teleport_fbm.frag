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

void main()
{
	vec2 uv = in_uv * 2.0 - 1.0;
	float r = length(uv);

	/* Keep portal present as a dark disk; only fade at the very rim. */
	float mask = 1.0 - smoothstep(0.90, 1.06, r);
	vec3 particle_color = vec3(0.0);

	/* Inward moving particles layered over a black background. */
	for (int i = 0; i < 48; ++i)
	{
		float fi = float(i);
		vec2 seed = hash2(fi * 13.17);
		float lane = hash1(fi * 9.73);
		float speed = mix(0.35, 1.30, hash1(fi * 7.91));
		float swirl = Time * (0.60 + lane * 0.90);
		float angle = seed.x * 6.2831853 + swirl;
		float travel = fract(seed.y + Time * speed);
		float radius = mix(0.95, 0.06, travel);
		float pulse = 0.80 + 0.20 * sin(Time * (2.2 + lane * 2.0) + fi * 3.1);
		float size = mix(0.016, 0.048, lane) * mix(0.60, 1.25, radius);
		vec2 center = vec2(cos(angle), sin(angle)) * radius;
		vec2 d = uv - center;
		float particle = exp(-dot(d, d) / max(size * size, 1e-4)) * pulse;
		vec3 tint = mix(vec3(0.35, 0.85, 1.00), vec3(1.00, 1.00, 1.00), lane);
		particle_color += tint * particle;
	}

	particle_color *= 1.7;
	particle_color *= mask;

	{
		float fog = exp2(-abs(Fog.w) * dot(in_pos - EyePos, in_pos - EyePos));
		float alpha;
		float emissive_visibility;
		vec3 base_color;
		vec3 color;
		fog = clamp(fog, 0.0, 1.0);
		alpha = clamp(in_alpha * mask, 0.0, 1.0);
		base_color = vec3(0.0);
		color = mix(Fog.rgb, base_color, fog);
		emissive_visibility = 0.35 + 0.65 * fog;
		color += particle_color * emissive_visibility;
		color = min(color, vec3(1.0));
		OUT_COLOR = vec4(color, alpha);
	}

#if !OIT
	{
		vec2 velocity = ComputeVelocity(in_curr_clip, in_prev_clip);
		vec2 velocity_out = vec2(0.0);
		if (OUT_COLOR.a >= 0.999)
			velocity_out = velocity * OUT_COLOR.a;
		out_velocity = vec4(velocity_out, 0.0, 2.0);
	}
#endif
}
