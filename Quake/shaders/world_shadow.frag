// BUG FIX: location=4 collided with ShadowViewProj (mat4 uses locations 4-7) in the vertex shader.
// ShadowLightPosFar -> 8 (first free slot after mat4), ShadowMode -> 9.
layout(location=8) uniform vec4 ShadowLightPosFar; // xyz: light pos, w: far plane
layout(location=9) uniform int ShadowMode;          // 0=sun, 1=dlight cube

layout(location=0) in vec3 in_world_pos;

void main()
{
	if (ShadowMode != 0)
	{
		// Cubemap dlight shadow: linear distance normalized to [0,1].
		float farPlane = max(ShadowLightPosFar.w, 1e-5);
		float dist = length(in_world_pos - ShadowLightPosFar.xyz);
		gl_FragDepth = clamp(dist / farPlane, 0.0, 1.0);
	}
	else
	{
		// Sun shadow: take rasterizer depth as-is. CPU shadow pass renders in
		// classic non-reversed depth space for consistent receiver comparisons.
		gl_FragDepth = gl_FragCoord.z;
	}
}
