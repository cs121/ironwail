
// ------------------------------
// DEBUG / SAFETY HELPERS
// ------------------------------
vec3 dbg_nan_guard(vec3 v) {
    if (any(isnan(v)) || any(isinf(v))) return vec3(1.0, 0.0, 1.0); // magenta = broken
    return v;
}

float dbg_nan_guard_f(float v) {
    if (isnan(v) || isinf(v)) return 0.0;
    return v;
}

#define DBG_EARLY_OUT_COLOR(c) { OutColor = vec4(c, 1.0); return; }

#include "frame_uniforms.glsl"

layout(binding=0) uniform sampler2D FogCurrent;
layout(binding=1) uniform sampler2D FogHistory;
layout(binding=2) uniform sampler2D SceneDepth;

layout(location=0) uniform float FogTemporalAlpha;
layout(location=1) uniform float FogTemporalDepthReject;
layout(location=2) uniform int FogDebugMode;
layout(location=3) uniform mat4 FogInvViewProj;
layout(location=4) uniform vec4 FogViewportParams; // xy: size, zw: inv size
layout(location=5) uniform vec2 FogDepthScale;
layout(location=6) uniform int FogHistoryValid;

layout(location=0) out vec4 OutColor;

float DepthToNdcZ(float depth)
{
#if REVERSED_Z
	return depth;
#else
	return depth * 2.0 - 1.0;
#endif
}

bool Reproject(vec2 uv, float depthNdc, out vec2 prevUv, out float prevDepthNdc)
{
	vec4 clip = vec4(uv * 2.0 - 1.0, depthNdc, 1.0);
	vec4 world = FogInvViewProj * clip;
	if (abs(world.w) < 1e-6)
		return false;
	vec4 prevClip = PrevViewProj * vec4(world.xyz / world.w, 1.0);
	if (abs(prevClip.w) < 1e-6)
		return false;
	vec3 prevNdc = prevClip.xyz / prevClip.w;
	prevUv = prevNdc.xy * 0.5 + 0.5;
	prevDepthNdc = prevNdc.z;
	return true;
}

void main()
{
    // DBG_FOG_MODES (inside main)
    // 1: pass reached (red)
    // 2: depth fetch path (green)
    // 3: volume path marker (blue)
    if (FogDebugMode == 1) DBG_EARLY_OUT_COLOR(vec3(1,0,0));
    

	vec2 invViewport = FogViewportParams.zw;
	vec2 uv = gl_FragCoord.xy * invViewport;
	vec4 current = texture(FogCurrent, uv);

	if (FogHistoryValid == 0 || PrevFrameValid == 0u || FogTemporalAlpha <= 0.0 || FogDebugMode == 5 || FogDebugMode == 8)
	{
		OutColor = current;
		return;
	}

	ivec2 depthCoord = ivec2(gl_FragCoord.xy * FogDepthScale);
	float depth = texelFetch(SceneDepth, depthCoord, 0).r;
	float depthNdc = DepthToNdcZ(depth);

	vec2 prevUv;
	float prevDepthNdc;
	bool valid = Reproject(uv, depthNdc, prevUv, prevDepthNdc);
	valid = valid && all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)));

	vec4 history = vec4(0.0);
	if (valid)
	{
		ivec2 prevCoord = ivec2(prevUv * FogViewportParams.xy);
		prevCoord = clamp(prevCoord, ivec2(0), ivec2(FogViewportParams.xy) - ivec2(1));
		float depthPrev = texelFetch(SceneDepth, ivec2(vec2(prevCoord) * FogDepthScale), 0).r;
		float depthPrevNdc = DepthToNdcZ(depthPrev);
		float depthReject = max(FogTemporalDepthReject, 0.0);
		if (abs(depthPrevNdc - prevDepthNdc) > depthReject)
			valid = false;
	}

	if (valid)
		history = texture(FogHistory, prevUv);

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
			vec2 offset = vec2(i, j) * invViewport;
			vec3 tap = texture(FogCurrent, uv + offset).rgb;
			minColor = min(minColor, tap);
			maxColor = max(maxColor, tap);
		}
	}
	history.rgb = clamp(history.rgb, minColor, maxColor);

	float motionPx = length((prevUv - uv) * FogViewportParams.xy);
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
	OutColor = vec4(blended, blendedAlpha);
}
