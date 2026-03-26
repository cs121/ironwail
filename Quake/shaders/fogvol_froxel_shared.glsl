const float FOGFROXEL_NEAR_EPS = 1e-4;
const float FOGFROXEL_OUTSIDE_ACCEPT = 0.75;
const float FOGFROXEL_EDGE_FADE_WIDTH = 0.35;

vec3 FogFroxel_ViewPosFromUVW(vec3 uvw, vec4 params0, vec4 params1)
{
	float nearClip = max(params0.x, FOGFROXEL_NEAR_EPS);
	float logFarNear = max(params1.x, FOGFROXEL_NEAR_EPS);
	// depth is the linear eye-space distance along the forward axis.
	// Standard OpenGL view space: forward = -Z, so viewPos.z = -depth.
	float depth = nearClip * exp(uvw.z * logFarNear);
	float halfW = max(FOGFROXEL_NEAR_EPS, depth * params0.z);
	float halfH = max(FOGFROXEL_NEAR_EPS, depth * params0.w);
	return vec3(
		(uvw.x - 0.5) * (2.0 * halfW),
		(uvw.y - 0.5) * (2.0 * halfH),
		-depth);
}

void FogFroxel_UVWFromViewPos(vec3 viewPos, vec4 params0, vec4 params1, out vec3 uvw, out float outside)
{
	float nearClip = max(params0.x, FOGFROXEL_NEAR_EPS);
	// Standard OpenGL view space: forward = -Z, depth = -viewPos.z
	float viewDepth = -viewPos.z;
	float depth = max(viewDepth, nearClip);
	float halfW = max(FOGFROXEL_NEAR_EPS, depth * params0.z);
	float halfH = max(FOGFROXEL_NEAR_EPS, depth * params0.w);
	float u = viewPos.x / (2.0 * halfW) + 0.5;
	float v = viewPos.y / (2.0 * halfH) + 0.5;
	float w = log(max(depth, nearClip) / nearClip) / max(params1.x, FOGFROXEL_NEAR_EPS);
	vec3 below;
	vec3 above;
	vec3 extent;
	float nearOutside;

	uvw = vec3(u, v, w);
	below = max(-uvw, vec3(0.0));
	above = max(uvw - vec3(1.0), vec3(0.0));
	extent = max(below, above);
	nearOutside = max((nearClip - viewDepth) / nearClip, 0.0);
	outside = max(max(extent.x, max(extent.y, extent.z)), nearOutside);
}
