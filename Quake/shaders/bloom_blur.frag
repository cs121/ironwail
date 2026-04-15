layout(binding=0) uniform sampler2D BloomTexture;

layout(location=0) uniform vec4 BlurParams;

layout(location=0) out vec4 outColor;

void main()
{
	vec2 rtSize = BlurParams.xy;
	vec2 direction = BlurParams.zw;

	vec2 texelSize = 1.0 / rtSize;
	vec2 uv = (gl_FragCoord.xy + 0.5) * texelSize;

	const float weights[3] = float[](0.382683, 0.241897, 0.060658);

	vec3 color = texture(BloomTexture, uv).rgb * weights[0];

	for (int i = 1; i < 3; ++i) {
		vec2 offset = direction * texelSize * float(i) * 2.0;
		color += texture(BloomTexture, uv + offset).rgb * weights[i];
		color += texture(BloomTexture, uv - offset).rgb * weights[i];
	}

	outColor = vec4(color, 1.0);
}