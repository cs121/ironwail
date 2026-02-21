layout(binding=0) uniform sampler2D ScreenTexture;

layout(location=13) uniform vec4 FilmGrainParams0; // x: amount, y: size, z: speed, w: luma weight
layout(location=14) uniform vec4 FilmGrainParams1; // x: blend, y: color, z: debug, w: seed
layout(location=15) uniform vec4 FilmGrainParams2; // x: frame, yzw: unused

float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

float InterleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 FilmGrainNoise(vec2 coord, float time, float seed, float colored)
{
	vec2 base = coord + vec2(seed, seed * 0.37);
	float n0 = tri(InterleavedGradientNoise(base + vec2(time * 11.1, time * 7.3)));
	float n1 = tri(InterleavedGradientNoise(base + vec2(17.7 + time * 5.2, 3.1 + time * 9.2)));
	float n2 = tri(InterleavedGradientNoise(base + vec2(8.3 + time * 6.7, 12.9 + time * 4.1)));
	vec3 colorNoise = (vec3(n0, n1, n2) + n0) * 0.5;
	return mix(vec3(n0), colorNoise, colored);
}

layout(location=0) out vec4 out_fragcolor;

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec4 color = texelFetch(ScreenTexture, pixel, 0);

	float grainAmount = clamp(FilmGrainParams0.x, 0.0, 1.0);
	float grainDebug = FilmGrainParams1.z;
	if (grainAmount > 0.0 || grainDebug > 0.5)
	{
		float grainSize = max(FilmGrainParams0.y, 0.01);
		float grainSpeed = max(FilmGrainParams0.z, 0.0);
		float lumaWeight = clamp(FilmGrainParams0.w, 0.0, 1.0);
		float blend = clamp(FilmGrainParams1.x, 0.0, 1.0);
		float colored = clamp(FilmGrainParams1.y, 0.0, 1.0);
		float seed = FilmGrainParams1.w;
		float frame = FilmGrainParams2.x;
		float time = frame * grainSpeed;
		vec2 grainCoord = (gl_FragCoord.xy + vec2(0.25, 0.75)) / grainSize;
		vec3 grain = FilmGrainNoise(grainCoord, time, seed, colored);

		float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
		float shadow = clamp(1.0 - luma, 0.0, 1.0);
		float lumaFactor = mix(1.0, shadow, lumaWeight);
		float response = mix(1.0, smoothstep(0.0, 1.0, shadow), 0.5);
		float amount = grainAmount * lumaFactor * response;

		if (grainDebug > 0.5)
		{
			color.rgb = vec3(0.5) + grain * 0.5;
		}
		else
		{
			vec3 add = color.rgb + grain * amount;
			vec3 soft = color.rgb + (color.rgb - color.rgb * color.rgb) * grain * amount;
			color.rgb = clamp(mix(add, soft, blend), vec3(0.0), vec3(1.0));
		}
	}

	out_fragcolor = vec4(color.rgb, 1.0);
}
