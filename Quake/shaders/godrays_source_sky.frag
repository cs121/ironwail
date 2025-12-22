layout(binding=0) uniform sampler2D DepthTexture;
layout(location=0) uniform vec4 SkyParams; // x: depth cutoff, y: sky intensity, z: reversed z flag

layout(location=0) out vec4 outColor;

void main()
{
	ivec2 coord = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(DepthTexture, coord, 0).r;
	bool isSky = (SkyParams.z > 0.5) ? (depth <= SkyParams.x) : (depth >= SkyParams.x);
	float sky = isSky ? SkyParams.y : 0.0;
	outColor = vec4(sky, 0.0, 0.0, 1.0);
}
