layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D MaskTexture;

layout(location=0) uniform vec4 ThresholdParams;
layout(location=1) uniform vec4 DownsampleParams;

layout(location=0) out vec4 outColor;

vec3 calculateBloom(vec3 color, float threshold, float softKnee)
{
	float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
	float diff = brightness - threshold;

	if (softKnee > 1e-4) {
		float soft = clamp(brightness - threshold + softKnee, 0.0, 2.0 * softKnee);
		soft = (soft * soft) / max(4.0 * softKnee, 1e-4);
		diff = max(diff, soft);
	}
	else {
		diff = max(diff, 0.0);
	}

	float factor = (brightness > 0.0) ? (diff / brightness) : 0.0;
	return color * factor;
}

void main()
{
	float threshold = ThresholdParams.x;
	float softKnee = max(ThresholdParams.y, 0.0);
	float maskEnabled = ThresholdParams.z;

	vec2 dstSize = DownsampleParams.zw;
	vec2 uv = (gl_FragCoord.xy + 0.5) / dstSize;

	vec3 color = texture(SceneTexture, uv).rgb;

	if (maskEnabled > 0.5) {
		float rawMask = texture(MaskTexture, uv).w;
		int maskBits = int(floor(rawMask + 0.5));

		if ((maskBits & 8) != 0) {
			color = vec3(0.0);
		}
		else {
			color = calculateBloom(color, threshold, softKnee);
		}
	} else {
		color = calculateBloom(color, threshold, softKnee);
	}

	outColor = vec4(color, 1.0);
}
