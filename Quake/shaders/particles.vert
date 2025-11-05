#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p)
{
	float fog = exp2(-Fog.w * dot(p, p));
	fog = clamp(fog, 0.0, 1.0);
	return mix(Fog.rgb, clr, fog);
}


layout(location=0) in vec3 in_pos;
layout(location=1) in vec4 in_color;
layout(location=2) in vec4 in_params;

layout(location=0) out vec2 out_uv;
layout(location=1) out vec4 out_color;
layout(location=2) out vec3 out_pos;
layout(location=3) out vec4 out_params;
layout(location=4) out vec2 out_corner;

layout(location=0) uniform vec3 Params;
#define ProjScale	Params.xy
#define UVScale Params.z

layout(location=1) uniform vec4 AtlasInfo;

void main()
{
	// figure the current corner: (-1, -1), (-1, 1), (1, -1) or (1, 1)
	uvec2 flipsign = uvec2(gl_VertexID, gl_VertexID >> 1) << 31;
	vec2 corner = uintBitsToFloat(floatBitsToUint(-1.0) ^ flipsign);

	// project the center of the particle
	gl_Position = ViewProj * vec4(in_pos, 1.0);

	// hack a scale up to keep particles from disappearing
	float depthscale = max(1.0 + gl_Position.w * 0.004, 1.08);

	// perform the billboarding
	gl_Position.xy += ProjScale * uintBitsToFloat(floatBitsToUint(vec2(depthscale)) ^ flipsign);

	float textureIndex = in_params.y;
	float tileCount = AtlasInfo.x > 0.0 ? 1.0 / AtlasInfo.x : 1.0;
	textureIndex = clamp(textureIndex, 0.0, max(tileCount - 1.0, 0.0));

	vec2 uv01 = corner * 0.5 + 0.5;
	uv01 = (uv01 - 0.5) * UVScale + 0.5;
	vec2 margin = vec2(AtlasInfo.z, AtlasInfo.w);
	uv01 = clamp(uv01, margin, vec2(1.0) - margin);
	vec2 uv = vec2(textureIndex * AtlasInfo.x, 0.0) + uv01 * vec2(AtlasInfo.x, AtlasInfo.y);

	out_pos = in_pos - EyePos; // FIXME: use corner position
	out_uv = uv;
	out_color = in_color;
	out_params = vec4(in_params.x * (1.0 / 255.0), UVScale, 0.0, 0.0);
	out_corner = corner;
#if OIT
	out_color.a *= 0.9;
#endif
}
