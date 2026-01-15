layout(binding=0) uniform sampler2D ScreenTexture;

layout(location=13) uniform vec4 FilmGrainParams0; // x: amount, y: size, z: speed, w: luma weight
layout(location=14) uniform vec4 FilmGrainParams1; // x: blend, y: color, z: debug, w: seed
layout(location=15) uniform vec4 FilmGrainParams2; // x: frame, yzw: unused

// Film-grain focused noise (avoids directional / "tape" patterns)
// Uses integer hashing (stable, fast) and a small Gaussian-ish sum.
// Note: true nondeterminism isn't possible in pure GLSL; vary FilmGrainParams2.x ("frame") and/or FilmGrainParams1.w ("seed") per frame for randomness.

uint Hash32(uint x)
{
	// mix-like hash (PCG-ish)
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float U01(uint x)
{
	// [0,1)
	return float(x) * (1.0 / 4294967296.0);
}

float Hash12(uvec2 p, uint seed)
{
	uint x = p.x * 0x9e3779b9u + seed;
	uint y = p.y * 0x85ebca6bu + seed * 0xc2b2ae35u;
	return U01(Hash32(x ^ y));
}

float GrainGaussian(uvec2 p, uint seed)
{
	// Sum of uniforms -> approx. Gaussian in [-1, 1]
	float a = Hash12(p, seed);
	float b = Hash12(p ^ uvec2(0x68bc21ebu, 0x02e5be93u), seed + 0x9e3779b9u);
	float c = Hash12(p ^ uvec2(0x1b56c4e9u, 0x7f4a7c15u), seed + 0x85ebca6bu);
	float g = (a + b + c) - 1.5;        // ~[-1.5, +1.5]
	return clamp(g * (2.0 / 1.5), -1.0, 1.0);
}

vec3 FilmGrainNoise(vec2 coord, float frame, float seedF, float colored)
{
	// coord is already scaled by grainSize. Use pixel-ish integer space to avoid repeating patterns.
	uvec2 ip = uvec2(floor(coord));

	// Frame + seed: decorrelate over time (gives "alive" film grain, not sliding tape noise)
	uint seed = floatBitsToUint(seedF) ^ uint(frame) * 0x9e3779b9u;

	// Base monochrome grain (dominant in film)
	float g0 = GrainGaussian(ip, seed);

	// Slight clumping (very subtle): modulate grain amplitude with a lower-frequency field.
	// This helps avoid the "TV static" look and reads more like film stock.
	uvec2 lp = uvec2(floor(coord * 0.18)); // lower freq -> larger clumps (more analog)
	float clump = mix(0.70, 1.35, Hash12(lp, seed + 0x1234567u)); // stronger clumping

	// Optional chroma speckle (keep weaker than luma)
	float chromaAmt = clamp(colored, 0.0, 1.0) * 0.55; // Kodak-ish chroma grain
	float g1 = GrainGaussian(ip ^ uvec2(0xA3C59AC3u, 0x3F0A6D2Du), seed + 0x51ed2705u);
	float g2 = GrainGaussian(ip ^ uvec2(0xE91E63A5u, 0xBB67AE85u), seed + 0x243f6a88u);

	vec3 mono = vec3(g0 * clump);
	vec3 chroma = vec3(g0, g1, g2) * 0.5 + g0 * 0.5; // correlated but not identical
	return mix(mono, chroma, chromaAmt);
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
		// Subtle size wobble for analog feel (keep tiny to avoid shimmer)
		grainSize *= mix(0.97, 1.03, U01(Hash32(floatBitsToUint(seed) ^ uint(frame) * 0x51ed2705u)));
		float grainSpeed = max(FilmGrainParams0.z, 0.0);
		float lumaWeight = clamp(FilmGrainParams0.w, 0.0, 1.0);
		float blend = clamp(FilmGrainParams1.x, 0.0, 1.0);
		float colored = clamp(FilmGrainParams1.y, 0.0, 1.0);
		float seed = FilmGrainParams1.w;
		float frame = FilmGrainParams2.x;
		float time = frame * grainSpeed;
		uvec2 pix = uvec2(pixel);
		uint jseed = floatBitsToUint(seed) ^ uint(frame) * 0x9e3779b9u;
		vec2 jitter = vec2(U01(Hash32(jseed ^ pix.x)), U01(Hash32((jseed + 0x68bc21ebu) ^ pix.y)));
		// Subpixel jitter per-frame helps avoid fixed-pattern noise.
		vec2 grainCoord = (gl_FragCoord.xy + jitter) / grainSize;
		vec3 grain = FilmGrainNoise(grainCoord, time, seed, colored);

		float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
		float shadow = clamp(1.0 - luma, 0.0, 1.0);
		float lumaFactor = mix(1.0, shadow, lumaWeight);
		float mid = 1.0 - abs(luma * 2.0 - 1.0); // 0 at blacks/whites, 1 at mids
		float response = mix(0.6, 1.35, smoothstep(0.15, 0.95, mid));
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
