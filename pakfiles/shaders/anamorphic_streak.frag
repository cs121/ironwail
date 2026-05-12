layout(binding=0) uniform sampler2D BloomTexture;

layout(location=0) uniform vec4 StreakParams;
layout(location=1) uniform vec4 StreakTint;
layout(location=2) uniform vec4 StreakSize;

layout(location=0) out vec4 outColor;

void main()
{
	vec2 srcSize = StreakSize.xy;
	vec2 dstSize = StreakSize.zw;
	vec2 texel = 1.0 / srcSize;
	vec2 uv = (gl_FragCoord.xy + 0.5) / dstSize;

	float lengthPx = max(StreakParams.x, 0.0);
	int taps = clamp(int(floor(StreakParams.y + 0.5)), 4, 32);
	float chroma = clamp(StreakParams.z, 0.0, 1.0);
	float softenY = max(StreakParams.w, 0.0);
	vec3 tint = StreakTint.xyz;
	float clampVal = max(StreakTint.w, 0.001);

	const float centerWeight = 0.16;

	vec3 sum = vec3(0.0);
	float wTotal = centerWeight;

	vec3 c = texture(BloomTexture, uv).rgb;
	sum += c * centerWeight;

	for (int i = 0; i < 32; ++i)
	{
		if (i >= taps)
			break;

		float t = (float(i) + 0.5) / float(taps);
		float distPx = lengthPx * t;
		float off = distPx * texel.x;
		float falloff = exp2(-t * 4.0);
		vec3 tapTint = mix(vec3(1.06, 1.00, 0.92), vec3(0.94, 0.99, 1.06), chroma * t);
		vec3 cp = texture(BloomTexture, uv + vec2(off, 0.0)).rgb;
		vec3 cn = texture(BloomTexture, uv - vec2(off, 0.0)).rgb;
		sum += (cp + cn) * falloff * tapTint;
		wTotal += 2.0 * falloff;
	}

	if (softenY > 0.001)
	{
		float yoff = softenY * texel.y;
		vec3 sy1 = texture(BloomTexture, uv + vec2(0.0, yoff)).rgb;
		vec3 sy2 = texture(BloomTexture, uv - vec2(0.0, yoff)).rgb;
		float sw = softenY * 0.04;
		sum += (sy1 + sy2) * sw;
		wTotal += sw * 2.0;
	}

	vec3 result = sum / max(wTotal, 0.001);
	result *= tint;
	result = min(result, vec3(clampVal));

	outColor = vec4(result, 1.0);
}
