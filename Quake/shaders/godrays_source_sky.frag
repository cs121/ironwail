layout(binding=0) uniform sampler2D DepthTexture;
layout(location=0) uniform vec4 SkyParams; // x: depth cutoff, y: sky intensity, z: reversed z flag, w: sky threshold
layout(location=1) uniform vec4 SkyTint; // rgb: sky tint
layout(location=2) uniform vec4 SkyMaskParams; // x: mask knee, yzw: unused

layout(location=0) out vec4 outColor;

float BrightPartMask(vec3 color, float threshold, float knee)
{
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	float k = (knee > 0.0) ? knee : max(threshold * 0.5, 1e-5);
	float mask = smoothstep(threshold - k, threshold + k, luma);
	return mask;
}

float SkyDepthMask(float depth)
{
	/*
	 * Be tolerant about depth convention: some pipelines can expose regular or
	 * reversed depth depending on runtime capabilities/state.  Accept either
	 * sky extreme near the far plane so source debug remains
	 * visible instead of going fully black on convention mismatches.
	 */
	const float edgeEpsilon = 0.003;
	float cpuCutoff = clamp(SkyParams.x, 0.0, 1.0);
	float reversedLike = step(depth, min(cpuCutoff + edgeEpsilon, 1.0));
	float regularLike = step(max(cpuCutoff - edgeEpsilon, 0.0), depth);
	float nearZero = step(depth, edgeEpsilon);
	float nearOne = step(1.0 - edgeEpsilon, depth);

	if (SkyParams.z > 0.5)
		return clamp(max(reversedLike, nearOne), 0.0, 1.0);
	return clamp(max(regularLike, nearZero), 0.0, 1.0);
}

void main()
{
	ivec2 coord = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(DepthTexture, coord, 0).r;
	float mask = SkyDepthMask(depth) * BrightPartMask(SkyTint.rgb, SkyParams.w, SkyMaskParams.x);
	vec3 color = SkyTint.rgb * SkyParams.y * mask;
	outColor = vec4(color, mask);
}
