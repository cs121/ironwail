#include "frame_uniforms.glsl"

#define MAX_FOGVOLUMES 64

struct FogVolume
{
	vec4 mins;
	vec4 maxs;
	vec4 color_density;
	vec4 noise_params;
	vec4 velocity;
	vec4 misc;
};

layout(std140, binding=2) uniform FogVolumeUBO
{
	FogVolume FogVolumes[MAX_FOGVOLUMES];
};

layout(location=0) uniform int FogSteps;
layout(location=1) uniform int FogNoiseEnabled;
layout(location=2) uniform int FogDebugMode;
layout(location=3) uniform int FogVolumeIndex;

layout(location=0) out vec4 FragColor;

void main()
{
	FogVolume volume = FogVolumes[FogVolumeIndex];
	float density = max(volume.color_density.a, 0.0);
	float step_factor = clamp(float(FogSteps) / 128.0, 0.0, 1.0);
	float alpha = clamp(density * 0.25, 0.0, 0.6) * mix(0.8, 1.0, step_factor);
	vec3 color = volume.color_density.rgb;

	if (FogDebugMode == 3)
	{
		color = vec3(density);
		alpha = 1.0;
	}
	else if (FogDebugMode == 4)
	{
		float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + Time) * 43758.5453);
		color = vec3(n);
		alpha = 1.0;
	}
	else if (FogNoiseEnabled == 0)
	{
		alpha *= 0.9;
	}

	FragColor = vec4(color, alpha);
}
