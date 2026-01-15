#include "frame_uniforms.glsl"

layout(location=0) in vec3 v_world_pos;
layout(location=0) out vec4 FragColor;

layout(location=0) uniform vec3 GodrayVolumeOrigin;
layout(location=1) uniform vec3 GodrayVolumeAxisT;
layout(location=2) uniform vec3 GodrayVolumeAxisB;
layout(location=3) uniform vec3 GodrayVolumeAxisR;
layout(location=4) uniform vec3 GodrayVolumeMins;
layout(location=5) uniform vec3 GodrayVolumeMaxs;
layout(location=6) uniform vec4 GodrayVolumeColorDensity;
layout(location=7) uniform vec4 GodrayVolumeMisc; // x: noise scale, y: noise amount, z: intensity
layout(location=8) uniform int GodrayVolumeSteps;

float Hash31(vec3 p)
{
	return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

void main()
{
	vec3 ray_origin_ws = EyePos;
	vec3 ray_dir_ws = normalize(v_world_pos - ray_origin_ws);

	vec3 rel_origin = ray_origin_ws - GodrayVolumeOrigin;
	vec3 ray_origin = vec3(
		dot(rel_origin, GodrayVolumeAxisT),
		dot(rel_origin, GodrayVolumeAxisB),
		dot(rel_origin, GodrayVolumeAxisR));
	vec3 ray_dir = vec3(
		dot(ray_dir_ws, GodrayVolumeAxisT),
		dot(ray_dir_ws, GodrayVolumeAxisB),
		dot(ray_dir_ws, GodrayVolumeAxisR));

	vec3 inv_dir = 1.0 / ray_dir;
	vec3 t0 = (GodrayVolumeMins - ray_origin) * inv_dir;
	vec3 t1 = (GodrayVolumeMaxs - ray_origin) * inv_dir;
	vec3 tmin = min(t0, t1);
	vec3 tmax = max(t0, t1);
	float t_enter = max(max(tmin.x, tmin.y), tmin.z);
	float t_exit = min(min(tmax.x, tmax.y), tmax.z);

	if (t_exit <= max(t_enter, 0.0))
		discard;

	float start_t = max(t_enter, 0.0);
	float end_t = t_exit;
	float length_t = max(end_t - start_t, 0.0001);
	float step_t = length_t / float(GodrayVolumeSteps);

	float accum = 0.0;
	float ray_length = max(GodrayVolumeMaxs.z - GodrayVolumeMins.z, 0.0001);

	for (int i = 0; i < GodrayVolumeSteps; ++i)
	{
		float t = start_t + (float(i) + 0.5) * step_t;
		vec3 local_pos = ray_origin + ray_dir * t;
		float depth = clamp((local_pos.z - GodrayVolumeMins.z) / ray_length, 0.0, 1.0);
		float plane_atten = 1.0 - depth;
		float noise = mix(1.0, Hash31(local_pos * GodrayVolumeMisc.x), GodrayVolumeMisc.y);
		float dist_atten = 1.0 / (1.0 + t * 0.25);
		accum += noise * plane_atten * dist_atten * step_t;
	}

	vec3 color = GodrayVolumeColorDensity.rgb * GodrayVolumeMisc.z;
	float density = GodrayVolumeColorDensity.a;
	FragColor = vec4(color * accum * density, 1.0);
}
