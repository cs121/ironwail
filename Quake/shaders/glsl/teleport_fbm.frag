layout(location=12) uniform float u_time;
layout(location=2) in vec2 v_texcoord;

float colormap_red(float x)
{
	return clamp(1.5 - abs(4.0 * (x - 0.75)), 0.0, 1.0);
}

float colormap_green(float x)
{
	return clamp(1.5 - abs(4.0 * (x - 0.5)), 0.0, 1.0);
}

float colormap_blue(float x)
{
	return clamp(1.5 - abs(4.0 * (x - 0.25)), 0.0, 1.0);
}

vec3 colormap(float x)
{
	return vec3(colormap_red(x), colormap_green(x), colormap_blue(x));
}

float rand(vec2 co)
{
	return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

float noise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	float a = rand(i);
	float b = rand(i + vec2(1.0, 0.0));
	float c = rand(i + vec2(0.0, 1.0));
	float d = rand(i + vec2(1.0, 1.0));
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

float fbm(vec2 p)
{
	float value = 0.0;
	float amplitude = 0.5;
	for (int i = 0; i < 5; ++i)
	{
		value += amplitude * noise(p);
		p *= 2.0;
		amplitude *= 0.5;
	}
	return value;
}

float pattern(vec2 p)
{
	vec2 q = vec2(fbm(p + vec2(0.0, 0.0)), fbm(p + vec2(5.2, 1.3)));
	vec2 r = vec2(
		fbm(p + 4.0 * q + vec2(1.7, 9.2)),
		fbm(p + 4.0 * q + vec2(8.3, 2.8))
	);
	return clamp(fbm(p + 4.0 * r), 0.0, 1.0);
}

#if OIT
layout(location=0) out vec4 out_accum;
layout(location=1) out float out_reveal;
#else
layout(location=0) out vec4 fragColor;
layout(location=1) out vec4 out_velocity;
#endif

void main()
{
	vec2 uv = v_texcoord * 2.0;
	float t = u_time * 0.6;
	float shade = pattern(uv + t);
	vec3 color = colormap(shade).rgb;
	vec4 outColor = vec4(color, shade);

#if OIT
	float z = 1.0 / gl_FragCoord.w;
	float weight = clamp(outColor.a * outColor.a * 0.03 / (1e-5 + pow(z / 1e7, 1.0)), 1e-2, 3e3);
	out_accum = vec4(outColor.rgb, outColor.a * weight);
	out_accum.rgb *= out_accum.a;
	out_reveal = outColor.a;
#else
	fragColor = outColor;
	out_velocity = vec4(0.0);
#endif
}
