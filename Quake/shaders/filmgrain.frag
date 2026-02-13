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

float Hash12(vec2 p)
{
	p = fract(p * vec2(0.1031, 0.1030));
	p += dot(p, p.yx + 33.33);
	return fract((p.x + p.y) * p.x);
}

vec3 FilmGrainNoise(vec2 fragCoord, float time, float seed, float colored, float grainSize)
{
	vec2 seedOffset = vec2(seed * 1.173, seed * 2.417);
	float t = time + Hash12(vec2(time * 0.123, seed * 1.37));

	float sizeA = max(0.25, grainSize * mix(0.55, 1.65, Hash12(floor(fragCoord * 0.03125) + seedOffset)));
	float sizeB = max(0.25, grainSize * mix(0.45, 2.05, Hash12(floor(fragCoord * 0.046875) + seedOffset.yx + 17.0)));
	float sizeC = max(0.25, grainSize * mix(0.65, 2.35, Hash12(floor(fragCoord * 0.0625) + seedOffset + 91.0)));

	vec2 coordA = (fragCoord + vec2(0.31, 0.73)) / sizeA;
	vec2 coordB = (fragCoord + vec2(0.91, 0.27)) / sizeB;
	vec2 coordC = (fragCoord + vec2(0.53, 0.49)) / sizeC;

	float n0 = tri(InterleavedGradientNoise(coordA + vec2(t * 13.11, t * 7.97) + seedOffset));
	float n1 = tri(InterleavedGradientNoise(coordB + vec2(17.7 + t * 5.23, 3.1 + t * 9.19) + seedOffset.yx));
	float n2 = tri(InterleavedGradientNoise(coordC + vec2(8.3 + t * 6.71, 12.9 + t * 4.07) + seedOffset * 0.7));

	float mono = (n0 + n1 + n2) * (1.0 / 3.0);
	vec3 colorNoise = vec3(n0, n1, n2);
	return mix(vec3(mono), colorNoise, colored);
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
		vec3 grain = FilmGrainNoise(gl_FragCoord.xy, time, seed, colored, grainSize);

		float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
		float shadow = 1.0 - clamp(luma, 0.0, 1.0);
		float lumaCurve = smoothstep(0.0, 1.0, shadow * shadow);
		float lumaFactor = mix(1.0, mix(shadow, lumaCurve, 0.65), lumaWeight);
		float response = 0.65 + 0.35 * smoothstep(0.0, 1.0, shadow);
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
