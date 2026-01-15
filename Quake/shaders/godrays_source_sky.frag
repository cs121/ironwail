layout(binding=0) uniform sampler2D DepthTexture;
layout(location=0) uniform vec4 SkyParams; // x: depth cutoff, y: sky intensity, z: reversed z flag, w: sky threshold
layout(location=1) uniform vec4 SkyTint; // rgb: sky tint
layout(location=2) uniform vec4 SkyMaskParams; // x: mask knee, yzw: unused

layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outDir;

float BrightPartMask(vec3 color, float threshold, float knee)
{
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	float k = (knee > 0.0) ? knee : max(threshold * 0.5, 1e-5);
	float mask = smoothstep(threshold - k, threshold + k, luma);
	return mask;
}

void main()
{
	ivec2 coord = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(DepthTexture, coord, 0).r;
	bool isSky = (SkyParams.z > 0.5) ? (depth <= SkyParams.x) : (depth >= SkyParams.x);
	float mask = 0.0;
	if (isSky)
		mask = BrightPartMask(SkyTint.rgb, SkyParams.w, SkyMaskParams.x);
	vec3 color = SkyTint.rgb * SkyParams.y * mask;
	outColor = vec4(color, mask);
	outDir = vec4(0.5, 0.5, 0.0, 1.0);
}
