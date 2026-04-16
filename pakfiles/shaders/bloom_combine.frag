layout(binding=0) uniform sampler2D BloomTexture;

layout(location=0) uniform vec4 CombineParams;
layout(location=1) uniform vec4 SourceSize;

layout(location=0) out vec4 outColor;

void main()
{
	float weight = max(CombineParams.x, 0.0);
	vec2 srcSize = SourceSize.xy;
	vec2 uv = gl_FragCoord.xy / srcSize;

	vec3 color = texture(BloomTexture, uv).rgb * weight;

	outColor = vec4(color, 1.0);
}
