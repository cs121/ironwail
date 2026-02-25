#include "frame_uniforms.glsl"

layout(binding=0) uniform sampler2D AOCurr;
layout(binding=1) uniform sampler2D AOHistory;
layout(binding=2) uniform sampler2D SceneDepth;
layout(binding=3) uniform sampler2D VelocityTexture;

layout(location=0) uniform vec4 u_screen; // inv, size
layout(location=1) uniform vec4 u_ao; // inv, size
layout(location=2) uniform vec4 u_temporal; // weight, clamp, history valid, velocity valid
layout(location=3) uniform mat4 u_invViewProj;
layout(location=4) uniform int u_reversedZMode;
layout(location=5) uniform int u_debugMode;

layout(location=0) out vec4 outColor;

float DepthToNdcZ(float depth)
{
#if REVERSED_Z
	return depth;
#else
	return depth * 2.0 - 1.0;
#endif
}

vec2 ReprojectUV(vec2 uv, float depth)
{
	if (u_temporal.w > 0.5)
		return uv - texture(VelocityTexture, uv).xy;

	vec4 clip = vec4(uv * 2.0 - 1.0, DepthToNdcZ(depth), 1.0);
	vec4 world = u_invViewProj * clip;
	if (abs(world.w) < 1e-6)
		return vec2(-1.0);
	vec4 prevClip = PrevViewProj * vec4(world.xyz / world.w, 1.0);
	if (abs(prevClip.w) < 1e-6)
		return vec2(-1.0);
	return prevClip.xy / prevClip.w * 0.5 + 0.5;
}

void main()
{
	vec2 uv = gl_FragCoord.xy * u_ao.xy;
	vec2 fullUv = uv;
	float current = texture(AOCurr, uv).r;
	if (u_temporal.z <= 0.5 || PrevFrameValid == 0u)
	{
		outColor = vec4(current, current, current, 1.0);
		return;
	}

	float depth = texture(SceneDepth, fullUv).r;
	vec2 prevUv = ReprojectUV(fullUv, depth);
	bool valid = all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)));
	float history = valid ? texture(AOHistory, prevUv).r : current;

	float mn = current;
	float mx = current;
	for (int y = -1; y <= 1; ++y)
	for (int x = -1; x <= 1; ++x)
	{
		float tap = texture(AOCurr, uv + vec2(x, y) * u_ao.xy).r;
		mn = min(mn, tap);
		mx = max(mx, tap);
	}
	float expand = max(u_temporal.y, 0.0);
	mn -= expand;
	mx += expand;
	float clamped = clamp(history, mn, mx);
	float alpha = valid ? clamp(u_temporal.x, 0.0, 0.99) : 0.0;
	float outAo = mix(current, clamped, alpha);

	if (u_debugMode == 15)
		outAo = valid ? history : current;
	else if (u_debugMode == 16)
		outAo = (abs(history - clamped) > 1e-4) ? 0.0 : 1.0;

	outColor = vec4(outAo, outAo, outAo, 1.0);
}
