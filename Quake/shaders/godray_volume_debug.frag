#include "frame_uniforms.glsl"

layout(location=0) out vec4 FragColor;

layout(location=9) uniform vec3 GodrayVolumeRayDir;
layout(location=10) uniform int GodrayVolumeDebugMode;

void main()
{
	if (GodrayVolumeDebugMode == 1)
	{
		FragColor = vec4(0.0, 1.0, 0.0, 0.5);
		return;
	}

	if (GodrayVolumeDebugMode == 2)
	{
		FragColor = vec4(normalize(GodrayVolumeRayDir) * 0.5 + 0.5, 1.0);
		return;
	}

	FragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
