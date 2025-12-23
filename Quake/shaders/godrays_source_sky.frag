layout(binding=0) uniform sampler2D DepthTexture;
layout(location=0) uniform vec4 SkyParams; // x: depth cutoff, y: sky intensity, z: reversed z flag
layout(location=1) uniform vec4 SkyTint; // rgb: sky tint

layout(location=0) out vec4 outColor;

void main()
{
	ivec2 coord = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(DepthTexture, coord, 0).r;
	bool isSky = (SkyParams.z > 0.5) ? (depth <= SkyParams.x) : (depth >= SkyParams.x);
	float strength = isSky ? SkyParams.y : 0.0;
	outColor = vec4(SkyTint.rgb, strength);
}
