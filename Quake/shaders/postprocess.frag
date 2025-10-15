// ============================================================================
// POST-PROCESSING SHADER - Optimized & Extended
// Features: Motion Blur, Depth of Field, HDR, Tonemapping, Palettization
// ============================================================================

// ============================================================================
// TEXTURE BINDINGS
// ============================================================================
layout(binding=0) uniform sampler2D GammaTexture;
layout(binding=1) uniform usampler3D PaletteLUT;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D BloomTexture;
layout(binding=4) uniform sampler2D VelocityTexture;

layout(std430, binding=0) restrict readonly buffer PaletteBuffer
{
	uint Palette[256];
};

// ============================================================================
// UNIFORMS (Original compatibility maintained)
// ============================================================================
layout(location=0) uniform vec4 Params;          // x: gamma, y: contrast, z: scale, w: dither
layout(location=1) uniform vec4 DoFParams0;      // x: enabled, y: focus distance, z: focus range, w: max blur radius
layout(location=2) uniform vec4 DoFParams1;      // x: near plane, y: far plane, z: reversed-Z flag
layout(location=3) uniform vec4 ViewRect;        // xy: view min (normalized), zw: view max (normalized)
layout(location=4) uniform vec4 DepthParams;     // xy: inverse view scale, zw: unused
layout(location=5) uniform vec3 HDRParams;       // x: bloom intensity, y: exposure, z: tonemap enabled
layout(location=6) uniform vec4 MotionParams0;   // x: enabled, y: shutter strength, z: min velocity, w: depth threshold
layout(location=7) uniform vec4 MotionParams1;   // x: max blur radius, y: max samples, zw: reserved

// New optional uniforms for advanced features (backwards compatible - defaults to 0)
layout(location=8) uniform vec4 AdaptiveParams;  // x: GPU load factor [0-1], y: LOD bias, zw: reserved
layout(location=9) uniform vec4 DoFParams2;      // x: bilateral sigma spatial, y: bilateral sigma range, zw: reserved

layout(location=0) out vec4 out_fragcolor;

// ============================================================================
// CONSTANTS
// ============================================================================
const int MOTION_MAX_SAMPLES = 64;
const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;
const float EPSILON = 1e-6;
const float INV_255 = 1.0 / 255.0;

// LOD constants
const float LOD_DISTANCE_NEAR = 10.0;   // Full quality distance
const float LOD_DISTANCE_FAR = 100.0;   // Minimum quality distance
const float LOD_MIN_QUALITY = 0.25;     // Minimum 25% samples

// Bilateral filtering constants
const float BILATERAL_SIGMA_SPATIAL_DEFAULT = 2.0;
const float BILATERAL_SIGMA_RANGE_DEFAULT = 0.1;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Fast RGB unpacking from packed uint
uvec3 UnpackRGB8(uint c)
{
	return uvec3(c, c >> 8, c >> 16) & 255u;
}

// ALU-only 16x16 Bayer matrix dithering
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));
	v = (v ^ (v << 2)) & 0x3333u;
	v = (v ^ (v << 1)) & 0x5555u;
	v |= v >> 7;
	v = bitfieldReverse(v) >> 24;
	return float(v) * (1.0 / 256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

// Hash-based white noise for temporal stability
float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float whitenoise(vec2 p)
{
	return whitenoise01(p) - 0.5;
}

// Triangular distribution for improved dithering quality
float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.0;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

// Screen-space noise macros
#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy) + 0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

// ============================================================================
// TONEMAPPING
// ============================================================================

// Hable/Uncharted 2 tonemapping operator
vec3 HableTonemap(vec3 x)
{
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	const float W = 11.2;
	
	vec3 numerator = x * (A * x + C * B) + D * E;
	vec3 denominator = x * (A * x + B) + D * F;
	vec3 mapped = numerator / denominator - E / F;
	
	// Precomputed white point
	const float white = 0.98264287;
	mapped /= white;
	
	return clamp(mapped, 0.0, 1.0);
}

// ============================================================================
// DEPTH SAMPLING SYSTEM
// ============================================================================

struct DepthSamplingInfo
{
	vec2 viewMinPx;
	vec2 viewMaxPx;
	vec2 invViewScale;
	vec2 depthTexSize;
	vec2 maxDepthIdx;
	bool valid;
};

// Initialize depth sampling info once per fragment
DepthSamplingInfo MakeDepthSamplingInfo()
{
	DepthSamplingInfo info;
	info.depthTexSize = vec2(textureSize(DepthTexture, 0));
	info.valid = all(greaterThan(info.depthTexSize, vec2(0.0)));
	
	if (!info.valid)
	{
		// Invalid state - set safe defaults
		info.viewMinPx = vec2(0.0);
		info.viewMaxPx = vec2(0.0);
		info.invViewScale = vec2(1.0);
		info.maxDepthIdx = vec2(0.0);
		return info;
	}
	
	vec2 viewMin = ViewRect.xy;
	vec2 viewMax = ViewRect.zw;
	info.viewMinPx = viewMin * info.depthTexSize;
	info.viewMaxPx = viewMax * info.depthTexSize;
	vec2 viewSizePx = max(info.viewMaxPx - info.viewMinPx, vec2(0.0));
	info.invViewScale = max(DepthParams.xy, vec2(1e-4));
	vec2 depthSizePx = max(vec2(1.0), floor(viewSizePx * info.invViewScale + 0.0001));
	info.maxDepthIdx = max(depthSizePx - 1.0, vec2(0.0));
	
	return info;
}

// Compute depth texture UV from fragment pixel coordinates
vec2 ComputeDepthUV(vec2 fragPx, DepthSamplingInfo info)
{
	if (!info.valid)
		return vec2(-1.0);
	
	vec2 viewSizePx = max(info.viewMaxPx - info.viewMinPx, vec2(0.0));
	vec2 relPx = clamp(fragPx - info.viewMinPx, vec2(0.0), max(viewSizePx - 1e-4, vec2(0.0)));
	vec2 depthIdx = clamp(floor(relPx * info.invViewScale), vec2(0.0), info.maxDepthIdx);
	vec2 depthPx = info.viewMinPx + depthIdx + 0.5;
	
	return depthPx / info.depthTexSize;
}

// Sample and linearize depth buffer
float SampleLinearDepth(vec2 fragPx, DepthSamplingInfo info)
{
	vec2 depthUV = ComputeDepthUV(fragPx, info);
	if (any(lessThan(depthUV, vec2(0.0))))
		return 0.0;
	
	float rawDepth = texture(DepthTexture, depthUV).r;
	float nearPlane = DoFParams1.x;
	float farPlane = DoFParams1.y;
	bool reversed = DoFParams1.z > 0.5;
	
	// Handle reversed-Z and standard depth
	if (reversed)
	{
		float denom = nearPlane + rawDepth * (farPlane - nearPlane);
		return (nearPlane * farPlane) / max(denom, EPSILON);
	}
	else
	{
		float ndcDepth = rawDepth * 2.0 - 1.0;
		float denom = farPlane + nearPlane - ndcDepth * (farPlane - nearPlane);
		return (2.0 * nearPlane * farPlane) / max(denom, EPSILON);
	}
}

// ============================================================================
// ADAPTIVE QUALITY SYSTEM
// ============================================================================

// Compute quality factor based on GPU load and distance LOD
float ComputeAdaptiveQuality(float depth, float gpuLoad, float lodBias)
{
	// GPU load scaling: 0.0 = no load (full quality), 1.0 = high load (reduced quality)
	float loadFactor = 1.0 - gpuLoad * 0.75; // Max 75% reduction
	
	// Distance-based LOD: reduce quality for distant objects
	float lodFactor = 1.0;
	if (depth > LOD_DISTANCE_NEAR)
	{
		float t = (depth - LOD_DISTANCE_NEAR) / (LOD_DISTANCE_FAR - LOD_DISTANCE_NEAR);
		t = clamp(t + lodBias, 0.0, 1.0);
		lodFactor = mix(1.0, LOD_MIN_QUALITY, t * t); // Quadratic falloff
	}
	
	// Combined quality factor
	return clamp(loadFactor * lodFactor, LOD_MIN_QUALITY, 1.0);
}

// ============================================================================
// MOTION BLUR WITH ADAPTIVE SAMPLING
// ============================================================================

// Accumulate a single motion blur sample with depth awareness
void AccumulateMotionSample(
	inout vec3 accum, 
	inout float weight, 
	vec2 sampleUV, 
	vec2 sampleCoordPx, 
	vec2 viewMin, 
	vec2 viewMax, 
	DepthSamplingInfo info, 
	bool useDepth, 
	float centerDepth, 
	float depthThresholdRatio)
{
	// Early exit if outside view bounds
	if (!all(greaterThanEqual(sampleUV, viewMin)) || !all(lessThanEqual(sampleUV, viewMax)))
		return;
	
	// Depth-aware rejection
	if (useDepth)
	{
		float sampleDepth = SampleLinearDepth(sampleCoordPx, info);
		float tolerance = depthThresholdRatio * max(centerDepth, EPSILON);
		if (abs(sampleDepth - centerDepth) > tolerance)
			return;
	}
	
	vec3 sampleColor = texture(GammaTexture, sampleUV).rgb;
	accum += sampleColor;
	weight += 1.0;
}

// Apply motion blur with adaptive quality and distance LOD
vec3 ApplyMotionBlur(vec3 color, vec2 uv, vec2 texSize, DepthSamplingInfo depthInfo, float qualityFactor)
{
	float shutterStrength = MotionParams0.y;
	if (shutterStrength <= 0.0)
		return color;
	
	vec2 viewMin = ViewRect.xy;
	vec2 viewMax = ViewRect.zw;
	vec2 viewSize = max(viewMax - viewMin, vec2(EPSILON));
	vec2 invScale = max(DepthParams.xy, vec2(1e-4));
	vec2 invTexSize = 1.0 / max(texSize, vec2(1.0));
	
	// Sample velocity texture
	vec2 velocityUV = clamp((uv - viewMin) * invScale, vec2(0.0), viewSize * invScale);
	vec2 velocity = texture(VelocityTexture, velocityUV).xy;
	vec2 velocityPx = velocity * shutterStrength * texSize;
	float speed = length(velocityPx);
	
	// Early exit for low velocities
	float minVelocity = max(MotionParams0.z, 0.0);
	if (speed <= minVelocity)
		return color;
	
	// Compute adaptive sample count
	float maxRadius = MotionParams1.x;
	int maxSamples = clamp(int(MotionParams1.y + 0.5), 1, MOTION_MAX_SAMPLES);
	
	if (maxRadius <= 0.0)
		maxRadius = speed;
	
	float radius = clamp(speed, 0.0, maxRadius);
	
	// Apply quality factor to sample count
	float radiusNormDenom = max(maxRadius, 1e-3);
	float sampleCountF = clamp(radius / radiusNormDenom, 0.0, 1.0) * float(maxSamples) * qualityFactor;
	int sampleCount = clamp(int(floor(sampleCountF + 0.5)), 1, maxSamples);
	
	if (sampleCount <= 0)
		return color;
	
	// Compute sampling direction
	vec2 direction = speed > 1e-4 ? (velocityPx / speed) : vec2(0.0);
	
	// Setup depth testing
	bool useDepth = MotionParams0.w > 0.0 && depthInfo.valid;
	float centerDepth = 0.0;
	float depthThresholdRatio = max(MotionParams0.w, 0.0);
	
	if (useDepth)
		centerDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
	
	// Accumulate motion blur samples
	vec3 accum = color;
	float weight = 1.0;
	float jitter = SCREEN_SPACE_NOISE();
	
	// Unrolled loop for better performance
	for (int i = 1; i <= MOTION_MAX_SAMPLES; ++i)
	{
		if (i > sampleCount)
			break;
		
		float t = (float(i) - 0.5 + jitter) / float(sampleCount);
		t = clamp(t, 0.0, 1.0);
		vec2 offsetPx = direction * (t * radius);
		
		if (length(offsetPx) < EPSILON)
			continue;
		
		vec2 offsetUV = offsetPx * invTexSize;
		
		// Bidirectional sampling
		vec2 sampleUVPos = uv + offsetUV;
		vec2 sampleUVNeg = uv - offsetUV;
		vec2 fragCoordPos = gl_FragCoord.xy + offsetPx;
		vec2 fragCoordNeg = gl_FragCoord.xy - offsetPx;
		
		AccumulateMotionSample(accum, weight, sampleUVPos, fragCoordPos, 
			viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio);
		AccumulateMotionSample(accum, weight, sampleUVNeg, fragCoordNeg, 
			viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio);
	}
	
	return weight > 0.0 ? accum / weight : color;
}

// ============================================================================
// DEPTH OF FIELD WITH BILATERAL FILTERING
// ============================================================================

// Bilateral weight function: combines spatial and range (depth/color) weights
float BilateralWeight(float spatialDist, float rangeDist, float sigmaSpatial, float sigmaRange)
{
	float spatialSigma = max(sigmaSpatial, 1e-4);
	float rangeSigma = max(sigmaRange, 1e-4);
	float spatialWeight = exp(-(spatialDist * spatialDist) / (2.0 * spatialSigma * spatialSigma));
	float rangeWeight = exp(-(rangeDist * rangeDist) / (2.0 * rangeSigma * rangeSigma));
	return spatialWeight * rangeWeight;
}

// Apply depth of field with bilateral filtering for better edge preservation
vec3 ApplyDepthOfField(vec3 color, vec2 uv, vec2 invTexSize, DepthSamplingInfo depthInfo)
{
        float linearDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
        float focusDistance = DoFParams0.y;
        float focusRange = max(DoFParams0.z, 0.0001);
        float maxBlur = max(DoFParams0.w, 0.0);

        // Calculate circle of confusion
        float coc = abs(linearDepth - focusDistance);
        float blurFactor = clamp((coc - focusRange) / focusRange, 0.0, 1.0);
        float blurRadius = blurFactor * maxBlur;

        // Early exit if no blur needed
        if (blurRadius < 0.0001)
                return color;

        // Bilateral filtering parameters
        float sigmaSpatial = DoFParams2.x > 0.0 ? DoFParams2.x : BILATERAL_SIGMA_SPATIAL_DEFAULT;
        float sigmaRange = DoFParams2.y > 0.0 ? DoFParams2.y : BILATERAL_SIGMA_RANGE_DEFAULT;

        vec2 viewMin = ViewRect.xy;
        vec2 viewMax = ViewRect.zw;

        // 8-tap radial blur kernel (optimized pattern)
        const vec2 kernel[8] = vec2[](
                vec2(1.0, 0.0),
                vec2(-1.0, 0.0),
		vec2(0.0, 1.0),
		vec2(0.0, -1.0),
		vec2(0.70710678, 0.70710678),
		vec2(-0.70710678, 0.70710678),
		vec2(0.70710678, -0.70710678),
		vec2(-0.70710678, -0.70710678)
	);
	
	// Randomized rotation for temporal stability
	float noise = SCREEN_SPACE_NOISE();
	float angle = noise * TWO_PI;
	float sine = sin(angle);
	float cosine = cos(angle);
	mat2 rotation = mat2(cosine, -sine, sine, cosine);
	
	// Bilateral filtering accumulation
        vec3 accum = color;
        float weight = 1.0;
        float centerLuma = dot(color, vec3(0.299, 0.587, 0.114));
        const float COLOR_SIGMA = 0.35;

        for (int i = 0; i < 8; ++i)
        {
                vec2 offsetDir = rotation * kernel[i];
                vec2 offsetPx = offsetDir * blurRadius;
                vec2 sampleUV = uv + offsetPx * invTexSize;

                if (!all(greaterThanEqual(sampleUV, viewMin)) || !all(lessThanEqual(sampleUV, viewMax)))
                        continue;

                vec3 sampleColor = texture(GammaTexture, sampleUV).rgb;
                vec2 sampleCoord = gl_FragCoord.xy + offsetPx;

                float sampleDepth = SampleLinearDepth(sampleCoord, depthInfo);
                float sampleCoC = abs(sampleDepth - focusDistance);
                float sampleBlurFactor = clamp((sampleCoC - focusRange) / focusRange, 0.0, 1.0);

                if (sampleBlurFactor <= 0.0 && blurFactor > 0.0)
                        continue;

                float spatialDist = length(offsetPx);
                float blurDelta = sampleBlurFactor - blurFactor;
                float w = BilateralWeight(spatialDist, blurDelta, sigmaSpatial, sigmaRange);

                float sampleLuma = dot(sampleColor, vec3(0.299, 0.587, 0.114));
                float colorDiff = sampleLuma - centerLuma;
                float colorWeight = exp(-(colorDiff * colorDiff) / (2.0 * COLOR_SIGMA * COLOR_SIGMA));

                float defocus = max(sampleBlurFactor, blurFactor);
                w *= colorWeight * defocus;

                if (w <= 0.0)
                        continue;

                accum += sampleColor * w;
                weight += w;
        }

        vec3 blurred = accum / max(weight, EPSILON);
        return mix(color, blurred, blurFactor);
}

// ============================================================================
// MAIN SHADER
// ============================================================================

void main()
{
	// Unpack uniform parameters
	float gamma = Params.x;
	float contrast = Params.y;
	float scale = Params.z;
	float dither = Params.w;
	
	// Compute fragment coordinates and UVs
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec2 texSize = vec2(textureSize(GammaTexture, 0));
	vec2 invTexSize = 1.0 / max(texSize, vec2(1.0));
	vec2 uv = (vec2(pixel) + 0.5) * invTexSize;
	
	// View rectangle bounds checking
	vec2 viewMin = ViewRect.xy;
	vec2 viewMax = ViewRect.zw;
	bool inView = all(greaterThanEqual(uv, viewMin)) && all(lessThanEqual(uv, viewMax));
	
	// Fetch initial color
	vec4 color = texelFetch(GammaTexture, pixel, 0);
	
	// Initialize depth sampling system
	DepthSamplingInfo depthInfo = MakeDepthSamplingInfo();
	
	// Compute adaptive quality factor
	float gpuLoad = clamp(AdaptiveParams.x, 0.0, 1.0);
	float lodBias = AdaptiveParams.y;
	float linearDepth = depthInfo.valid ? SampleLinearDepth(gl_FragCoord.xy, depthInfo) : 0.0;
	float qualityFactor = ComputeAdaptiveQuality(linearDepth, gpuLoad, lodBias);
	
	// Apply motion blur with adaptive quality
	if (MotionParams0.x > 0.5 && inView)
	{
		color.rgb = ApplyMotionBlur(color.rgb, uv, texSize, depthInfo, qualityFactor);
	}
	
	// Apply depth of field with bilateral filtering
	if (DoFParams0.x > 0.5 && inView && depthInfo.valid)
	{
		color.rgb = ApplyDepthOfField(color.rgb, uv, invTexSize, depthInfo);
	}
	
	out_fragcolor = color;
	
	// ========================================================================
	// PALETTIZATION PATH (Original compatibility)
	// ========================================================================
#if PALETTIZE == 1
	vec2 noiseuv = floor(gl_FragCoord.xy * scale) + 0.5;
	out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
	out_fragcolor.rgb += DITHER_NOISE(noiseuv) * dither;
	out_fragcolor.rgb *= out_fragcolor.rgb;
#endif

#if PALETTIZE
	ivec3 clr = ivec3(clamp(out_fragcolor.rgb, 0.0, 1.0) * 127.0 + 0.5);
	uint remap = Palette[texelFetch(PaletteLUT, clr, 0).x];
	out_fragcolor.rgb = vec3(UnpackRGB8(remap)) * INV_255;
	
	// ========================================================================
	// HDR/TONEMAPPING PATH (Original compatibility)
	// ========================================================================
#else
	vec3 hdrColor = out_fragcolor.rgb;
	
	// Bloom addition
	float bloomIntensity = HDRParams.x;
	vec3 bloomColor = vec3(0.0);
	if (bloomIntensity > 0.0)
	{
		bloomColor = texture(BloomTexture, uv).rgb * bloomIntensity;
	}
	
	// Exposure and contrast application
	float exposure = max(HDRParams.y, 0.0);
	float tonemapEnabled = HDRParams.z;
	vec3 combined = (hdrColor + bloomColor) * exposure * contrast;
	combined = max(combined, vec3(0.0));
	
	// Tonemapping
	vec3 mapped = tonemapEnabled > 0.5 ? HableTonemap(combined) : clamp(combined, 0.0, 1.0);
	mapped = clamp(mapped, 0.0, 1.0);
	
	// Gamma correction
	out_fragcolor = vec4(pow(mapped, vec3(gamma)), 1.0);
#endif
}
