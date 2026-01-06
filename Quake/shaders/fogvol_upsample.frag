
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

#define DBG_EARLY_OUT_COLOR(c) { fragColor = vec4(c, 1.0); return; }

layout(binding=0) uniform sampler2D FogColor;
layout(binding=1) uniform sampler2D SceneDepth;

layout(location=0) uniform vec4 FogUpsampleSize; // xy: full size, zw: half size
layout(location=1) uniform float FogUpsampleK;
layout(location=2) uniform int FogUpsampleTaps;

layout(location=0) out vec4 outColor;

void main()
{
	ivec2 fullSize = ivec2(FogUpsampleSize.xy);
	ivec2 halfSize = ivec2(FogUpsampleSize.zw);
	ivec2 fullCoord = ivec2(gl_FragCoord.xy);
	ivec2 halfCoord = ivec2(gl_FragCoord.xy * 0.5);
	halfCoord = clamp(halfCoord, ivec2(0), halfSize - ivec2(1));

	float depthCenter = texelFetch(SceneDepth, fullCoord, 0).r;
	vec3 accum = vec3(0.0);
	float weightSum = 0.0;

	if (FogUpsampleTaps == 9)
	{
		for (int j = -1; j <= 1; ++j)
		{
			for (int i = -1; i <= 1; ++i)
			{
				ivec2 offset = ivec2(i, j);
				ivec2 tapCoord = clamp(halfCoord + offset, ivec2(0), halfSize - ivec2(1));
				ivec2 fullTap = (tapCoord * 2) + ivec2(1, 1);
				fullTap = clamp(fullTap, ivec2(0), fullSize - ivec2(1));
				float depthTap = texelFetch(SceneDepth, fullTap, 0).r;
				float weight = exp(-abs(depthTap - depthCenter) * FogUpsampleK);
				accum += texelFetch(FogColor, tapCoord, 0).rgb * weight;
				weightSum += weight;
			}
		}
	}
	else
	{
		for (int j = 0; j < 2; ++j)
		{
			for (int i = 0; i < 2; ++i)
			{
				ivec2 offset = ivec2(i, j);
				ivec2 tapCoord = clamp(halfCoord + offset, ivec2(0), halfSize - ivec2(1));
				ivec2 fullTap = (tapCoord * 2) + ivec2(1, 1);
				fullTap = clamp(fullTap, ivec2(0), fullSize - ivec2(1));
				float depthTap = texelFetch(SceneDepth, fullTap, 0).r;
				float weight = exp(-abs(depthTap - depthCenter) * FogUpsampleK);
				accum += texelFetch(FogColor, tapCoord, 0).rgb * weight;
				weightSum += weight;
			}
		}
	}

	vec3 color = (weightSum > 0.0) ? (accum / weightSum) : vec3(0.0);
	outColor = vec4(color, 1.0);
}
