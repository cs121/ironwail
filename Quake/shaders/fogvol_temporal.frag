#include "frame_uniforms.glsl"

layout(binding=0) uniform sampler2D FogCurrent;
layout(binding=1) uniform sampler2D FogHistory;
layout(binding=2) uniform sampler2D SceneDepth;

layout(location=0) uniform float FogTemporalAlpha;
layout(location=1) uniform float FogTemporalDepthReject;
layout(location=2) uniform int FogDebugMode;
layout(location=3) uniform mat4 FogInvViewProj;
layout(location=4) uniform vec4 FogViewportParams; // xy: screen size, zw: inv screen size
layout(location=5) uniform vec2 FogDepthScale;
layout(location=6) uniform int FogHistoryValid;
layout(location=7) uniform vec4 FogViewParams; // xy: view origin in screen px, zw: inv view size

layout(location=0) out vec4 OutColor;

float DepthToNdcZ(float depth)
{
#if REVERSED_Z
	return depth;
#else
	return depth * 2.0 - 1.0;
#endif
}

bool Reproject(vec2 viewUv, float depthNdc, out vec2 prevViewUv, out float prevDepthNdc)
{
	vec4 clip = vec4(viewUv * 2.0 - 1.0, depthNdc, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
		return false;
	vec4 prevClip = PrevViewProj * vec4(world.xyz / world.w, 1.0);
	if (abs(prevClip.w) < 1e-6)
		return false;
	vec3 prevNdc = prevClip.xyz / prevClip.w;
	prevViewUv = prevNdc.xy * 0.5 + 0.5;
	prevDepthNdc = prevNdc.z;
	return true;
}

void main()
{
	vec2 screenPos = gl_FragCoord.xy * FogDepthScale;
	vec2 invScreen = FogViewportParams.zw;
	vec2 screenUv = screenPos * invScreen;
	vec2 viewUv = (screenPos - FogViewParams.xy) * FogViewParams.zw;
	vec2 viewSize = 1.0 / max(FogViewParams.zw, vec2(1e-6));
	vec4 current = texture(FogCurrent, screenUv);
	if (any(isnan(current)) || any(isinf(current)))
	{
		OutColor = vec4(1.0, 0.0, 1.0, 1.0);
		return;
	}

	if (FogHistoryValid == 0 || PrevFrameValid == 0u || FogTemporalAlpha <= 0.0 || FogDebugMode == 5 || FogDebugMode == 8)
	{
		OutColor = current;
		return;
	}

	ivec2 depthCoord = ivec2(screenPos);
	float depth = texelFetch(SceneDepth, depthCoord, 0).r;
	float depthNdc = DepthToNdcZ(depth);

	vec2 prevViewUv;
	float prevDepthNdc;
	bool valid = Reproject(viewUv, depthNdc, prevViewUv, prevDepthNdc);
	valid = valid && all(greaterThanEqual(prevViewUv, vec2(0.0))) && all(lessThanEqual(prevViewUv, vec2(1.0)));

	vec4 history = vec4(0.0);
	vec2 prevScreenUv = vec2(0.0);
	if (valid)
	{
		prevScreenUv = (prevViewUv * viewSize + FogViewParams.xy) * invScreen;
		ivec2 prevCoord = ivec2(prevScreenUv * FogViewportParams.xy);
		prevCoord = clamp(prevCoord, ivec2(0), ivec2(FogViewportParams.xy) - ivec2(1));
		float depthPrev = texelFetch(SceneDepth, prevCoord, 0).r;
		float depthPrevNdc = DepthToNdcZ(depthPrev);
		float depthReject = max(FogTemporalDepthReject, 0.0);
		if (abs(depthPrevNdc - prevDepthNdc) > depthReject)
			valid = false;
	}

	if (valid)
	{
		history = texture(FogHistory, prevScreenUv);
		if (any(isnan(history)) || any(isinf(history)))
			valid = false;
	}

	if (FogDebugMode == 6)
	{
		OutColor = vec4(vec3(valid ? 1.0 : 0.0), 1.0);
		return;
	}

	vec3 minColor = current.rgb;
	vec3 maxColor = current.rgb;
	for (int j = -1; j <= 1; ++j)
	{
		for (int i = -1; i <= 1; ++i)
		{
			vec2 offset = vec2(i, j) * invScreen;
			vec3 tap = texture(FogCurrent, screenUv + offset).rgb;
			minColor = min(minColor, tap);
			maxColor = max(maxColor, tap);
		}
	}
	history.rgb = clamp(history.rgb, minColor, maxColor);

	float motionPx = length((prevScreenUv - screenUv) * FogViewportParams.xy);
	float motionFactor = clamp(1.0 - motionPx * 0.1, 0.0, 1.0);
	float alpha = (valid ? FogTemporalAlpha : 0.0) * motionFactor;
	alpha = clamp(alpha, 0.0, 1.0);

	if (FogDebugMode == 7)
	{
		OutColor = vec4(vec3(alpha), 1.0);
		return;
	}

	vec3 blended = mix(current.rgb, history.rgb, alpha);
	float blendedAlpha = mix(current.a, history.a, alpha);
	if (FogDebugMode == 12)
	{
		bvec4 bad = bvec4(any(isnan(vec4(blended, blendedAlpha))) || any(isinf(vec4(blended, blendedAlpha))), false, false, false);
		OutColor = bad.x ? vec4(1.0, 1.0, 0.0, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	if (any(isnan(vec4(blended, blendedAlpha))) || any(isinf(vec4(blended, blendedAlpha))))
	{
		OutColor = current;
		return;
	}
	OutColor = vec4(blended, blendedAlpha);
}
