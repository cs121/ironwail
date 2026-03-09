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
	/* Strict sky test: only pass pixels that match the active depth convention.
	 * This prevents non-sky bright surfaces from becoming godray sources. */
	const float edgeEpsilon = 0.0015;
	float cpuCutoff = clamp(SkyParams.x, 0.0, 1.0);
	if (SkyParams.z > 0.5)
		return step(depth, min(cpuCutoff + edgeEpsilon, 1.0));  // reversed-Z sky near 0
	return step(max(cpuCutoff - edgeEpsilon, 0.0), depth);      // regular-Z sky near 1
}

void main()
{
	ivec2 coord = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(DepthTexture, coord, 0).r;
	float mask = SkyDepthMask(depth) * BrightPartMask(SkyTint.rgb, SkyParams.w, SkyMaskParams.x);
	vec3 color = SkyTint.rgb * SkyParams.y * mask;
	outColor = vec4(color, mask);
}
