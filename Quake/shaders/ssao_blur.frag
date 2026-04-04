layout(binding=0) uniform sampler2D SSAOTexture;
layout(binding=1) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4 u_screenParams; // xy: inv screen size, zw: screen size
layout(location=1) uniform vec4 u_aoParams; // xy: inv AO size, zw: AO size
layout(location=2) uniform vec4 u_params0; // xy: blur direction, zw: unused
layout(location=3) uniform vec4 u_depthParams; // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=4) uniform vec4 u_params1; // x: sigma, y: radius, z: depth threshold scale, w: bilateral toggle
layout(location=5) uniform vec4 u_viewRect; // xy: view min, zw: view max
layout(location=6) uniform int u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc
layout(location=7) uniform int u_yFlip; // 0: none, 1: flip Y
layout(location=8) uniform mat4 u_invProj;

layout(location=0) out vec4 outColor;

vec2 ApplyYFlip(vec2 uv)
{
	if (u_yFlip != 0)
		uv.y = 1.0 - uv.y;
	return uv;
}

vec2 ScreenInvSize()  { return u_screenParams.xy; }
vec2 ScreenSize()     { return u_screenParams.zw; }
vec2 AoInvSize()      { return u_aoParams.xy; }
vec2 AoSize()         { return u_aoParams.zw; }

vec2 AoToScreenScale()
{
	return ScreenSize() * AoInvSize();
}

vec2 UnflipUv(vec2 uv)
{
	if (u_yFlip != 0)
		uv.y = 1.0 - uv.y;
	return uv;
}

vec2 AoUvFromPixel(vec2 aoPixel)
{
	return ApplyYFlip((aoPixel + 0.5) * AoInvSize());
}

vec2 ScreenUvFromPixel(ivec2 pixel)
{
	return ApplyYFlip((vec2(pixel) + 0.5) * ScreenInvSize());
}

ivec2 ClampScreenPixel(vec2 pixel)
{
	vec2 maxPx = ScreenSize() - vec2(1.0);
	return ivec2(clamp(pixel, vec2(0.0), maxPx));
}

ivec2 ScreenPixelFromAoPixelNearest(vec2 aoPixel)
{
	vec2 screenSize = ScreenSize();
	vec2 aoSize     = max(AoSize(), vec2(1.0));
	vec2 screenPixel = floor(aoPixel * screenSize / aoSize);
	return ClampScreenPixel(screenPixel);
}

vec2 ScreenUvFromAoPixel(vec2 aoPixel)
{
	vec2 scale       = AoToScreenScale();
	vec2 screenPixel = aoPixel * scale + vec2(0.5);
	return ApplyYFlip(screenPixel * ScreenInvSize());
}

vec2 ScreenUvFromAoUv(vec2 uv)
{
	vec2 aoPixel = floor(uv * AoSize());
	return ScreenUvFromAoPixel(aoPixel);
}

ivec2 ScreenPixelFromUv(vec2 uv)
{
	vec2 unflipped   = UnflipUv(uv);
	vec2 screenPixel = floor(unflipped * ScreenSize());
	return ClampScreenPixel(screenPixel);
}

float DepthToNdcZ(float depth, float reversed, int mode)
{
	float raw = depth;
	if (mode == 1)
		raw = 1.0 - raw;
	if (reversed > 0.5)
	{
		if (mode == 2)
			raw = 1.0 - raw;
		return raw;
	}
	float ndc = raw * 2.0 - 1.0;
	if (mode == 2)
		ndc = -ndc;
	return ndc;
}

// Centralized SSAO depth conversion. Returns positive view-space depth (+X forward).
float ViewZFromDepth(vec2 uv, float depth01, bool reversedZ)
{
	float ndcDepth = DepthToNdcZ(depth01, reversedZ ? 1.0 : 0.0, u_reversedZMode);
	vec4  clip     = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4  view     = u_invProj * clip;
	float w        = view.w;
	if (abs(w) < 1e-6)
		return 1e30;
	return view.x / w;
}

float DepthRawFromPixel(ivec2 pixel)
{
	return texelFetch(DepthTexture, pixel, 0).r;
}

float DepthRawFromUv(vec2 uv)
{
	return DepthRawFromPixel(ScreenPixelFromUv(uv));
}

bool IsInvalidFloat(float v)
{
	return !(v > -1e20 && v < 1e20);
}

bool IsSkyDepth(float depth, vec4 depthParams)
{
	float reversed = depthParams.z;
	float cutoff   = depthParams.w;
	if (reversed > 0.5)
		return depth <= cutoff;
	return depth >= cutoff;
}

// PERF: Precomputed Gaussian weight table (avoids exp() in the inner loop).
// Weights for offsets -4..4 are computed at compile time using sigma from u_params1.x.
// Since sigma is a uniform and GLSL doesn't support constexpr functions, we compute
// the Gaussian inline but hoist the denominator outside the loop.
float Gaussian(float x, float invTwoSigmaSq)
{
	return exp(-x * x * invTwoSigmaSq);
}

void main()
{
	vec2 aoPixel       = floor(gl_FragCoord.xy);
	vec2 invResolution = AoInvSize();
	vec2 uv            = AoUvFromPixel(aoPixel);

	if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
	{
		outColor = vec4(1.0);
		return;
	}

	ivec2 screenPixel      = ScreenPixelFromAoPixelNearest(aoPixel);
	vec2  screenUv         = ScreenUvFromPixel(screenPixel);
	float centerDepthRaw   = DepthRawFromPixel(screenPixel);
	if (IsSkyDepth(centerDepthRaw, u_depthParams))
	{
		outColor = vec4(1.0);
		return;
	}

	bool  reversedZ  = (u_depthParams.z > 0.5);
	float centerDepth = ViewZFromDepth(screenUv, centerDepthRaw, reversedZ);
	if (IsInvalidFloat(centerDepth))
	{
		outColor = vec4(1.0);
		return;
	}

	float sigma          = max(u_params1.x, 0.01);
	int   radius         = clamp(int(u_params1.y + 0.5), 1, 4);
	// FIX: depth weight now uses standard bilateral formulation: weight = exp(-depthDiff^2 / threshold^2)
	// which gives 1 at zero diff and smoothly falls off — avoids the division-by-near-zero of the old formula.
	float depthThreshold = max(u_params1.z, 1e-4) * max(centerDepth, 1e-4);
	bool  useBilateral   = (u_params1.w > 0.5);

	// PERF: Hoist 1/(2*sigma^2) out of the loop.
	float invTwoSigmaSq  = 1.0 / max(2.0 * sigma * sigma, 1e-6);
	// PERF: Precompute bilateral denominator once.
	float invDepthThreshSq = useBilateral ? (1.0 / max(depthThreshold * depthThreshold, 1e-8)) : 0.0;

	float total = 0.0;
	float accum = 0.0;
	vec2  direction = u_params0.xy;

	// FIX + PERF: Loop runs only over [-radius..radius] — no wasted iterations.
	for (int i = -radius; i <= radius; ++i)
	{
		vec2  offset        = direction * (float(i) * invResolution);
		vec2  sampleUV      = clamp(uv + offset, u_viewRect.xy, u_viewRect.zw);
		vec2  sampleDepthUv = ScreenUvFromAoUv(sampleUV);
		float sampleDepthRaw = DepthRawFromUv(sampleDepthUv);
		if (IsSkyDepth(sampleDepthRaw, u_depthParams))
			continue;
		float sampleDepth = ViewZFromDepth(sampleDepthUv, sampleDepthRaw, reversedZ);
		if (IsInvalidFloat(sampleDepth))
			continue;

		// FIX: Standard bilateral Gaussian depth weight — numerically stable, no division by near-zero.
		float depthDiff   = sampleDepth - centerDepth;
		float depthWeight = useBilateral
		                  ? exp(-depthDiff * depthDiff * invDepthThreshSq)
		                  : 1.0;

		float weight = Gaussian(float(i), invTwoSigmaSq) * depthWeight;
		accum += texture(SSAOTexture, sampleUV).r * weight;
		total += weight;
	}

	float ao = (total > 0.0) ? (accum / total) : 1.0;
	outColor = vec4(ao, ao, ao, 1.0);
}
