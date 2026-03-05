layout(location=4) uniform vec4 ShadowLightPosFar; // xyz: light pos, w: far plane
layout(location=8) uniform int ShadowMode;          // 0=sun, 1=dlight cube

layout(location=0) in vec3 in_world_pos;

void main()
{
	if (ShadowMode != 0)
	{
		float farPlane = max(ShadowLightPosFar.w, 1e-5);
		float dist = length(in_world_pos - ShadowLightPosFar.xyz);
		gl_FragDepth = clamp(dist / farPlane, 0.0, 1.0);
	}
}
