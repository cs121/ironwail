#if BINDLESS
	#extension GL_ARB_bindless_texture : require
#else
	layout(binding=0) uniform sampler2D Tex;
	layout(binding=1) uniform sampler2D FullbrightTex;
	layout(binding=4) uniform sampler2D EmissiveTex;
#endif

layout(location=0) uniform vec4 GodraysSourceParams0; // x: emissive intensity, y: light intensity, z: emissive threshold, w: light threshold
layout(location=1) uniform vec4 GodraysSourceParams1; // x: mask knee, yzw: unused

const uint
	CF_USE_FULLBRIGHT = 2u,
	CF_USE_EMISSIVE = 8u,
	CF_ALPHA_TEST = 16u,
	CF_GODRAYS_LIGHT = 32u,
	CF_GODRAYS_EMISSIVE = 64u;

layout(location=0) flat in uint in_flags;
layout(location=3) in vec2 in_uv;
#if BINDLESS
	layout(location=9) flat in uvec4 in_samplers0;
	layout(location=10) flat in uvec2 in_samplers1;
#endif

layout(location=0) out vec4 outColor;

float BrightPartMask(vec3 color, float threshold, float knee)
{
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	float k = (knee > 0.0) ? knee : max(threshold * 0.5, 1e-5);
	float mask = smoothstep(threshold - k, threshold + k, luma);
	return mask;
}

void main()
{
#if BINDLESS
	sampler2D Tex = sampler2D(in_samplers0.xy);
#endif
	vec4 base = texture(Tex, in_uv);

	if ((in_flags & CF_ALPHA_TEST) != 0u && base.a < 0.666)
		discard;

	vec3 fullbright = vec3(0.0);
	vec3 emissive = vec3(0.0);

#if BINDLESS
	if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
	{
		sampler2D FullbrightTex = sampler2D(in_samplers0.zw);
		fullbright = texture(FullbrightTex, in_uv).rgb;
	}
	if ((in_flags & CF_USE_EMISSIVE) != 0u)
	{
		sampler2D EmissiveTex = sampler2D(in_samplers1.xy);
		emissive = texture(EmissiveTex, in_uv).rgb;
	}
#else
	if ((in_flags & CF_USE_FULLBRIGHT) != 0u)
		fullbright = texture(FullbrightTex, in_uv).rgb;
	if ((in_flags & CF_USE_EMISSIVE) != 0u)
		emissive = texture(EmissiveTex, in_uv).rgb;
#endif

	vec3 light_color = vec3(0.0);
	vec3 emissive_color = fullbright + emissive;
	float light_strength = 0.0;
	float emissive_strength = 0.0;
	float light_mask = 0.0;
	float emissive_mask = 0.0;
	float knee = GodraysSourceParams1.x;

	if ((in_flags & CF_GODRAYS_LIGHT) != 0u)
	{
		light_color = base.rgb;
		light_strength = GodraysSourceParams0.y;
		light_mask = BrightPartMask(light_color, GodraysSourceParams0.w, knee);
	}
	if ((in_flags & CF_GODRAYS_EMISSIVE) != 0u)
	{
		emissive_strength = GodraysSourceParams0.x;
		emissive_mask = BrightPartMask(emissive_color, GodraysSourceParams0.z, knee);
	}

	vec3 color = vec3(0.0);
	float mask = clamp(light_mask + emissive_mask, 0.0, 1.0);
	if (light_strength > 0.0)
		color += light_color * light_strength * light_mask;
	if (emissive_strength > 0.0)
		color += emissive_color * emissive_strength * emissive_mask;

	outColor = vec4(color, mask);
}
