layout(binding=0) uniform sampler2D GammaTexture;
layout(binding=1) uniform usampler3D PaletteLUT;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D BloomTexture;
layout(binding=4) uniform sampler2D VelocityTexture;
layout(std430, binding=0) restrict readonly buffer PaletteBuffer
{
	uint Palette[256];
};

uvec3 UnpackRGB8(uint c)
{
	return uvec3(c, c >> 8, c >> 16) & 255u;
}

// ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));	// 0  0  0  0 | x3 x2 x1 x0 |  0  0  0  0 | y3 y2 y1 y0
	v = (v ^ (v << 2)) & 0x3333;				// 0  0 x3 x2 |  0  0 x1 x0 |  0  0 y3 y2 |  0  0 y1 y0
	v = (v ^ (v << 1)) & 0x5555;				// 0 x3  0 x2 |  0 x1  0 x0 |  0 y3  0 y2 |  0 y1  0 y0
	v |= v >> 7;								// 0 x3  0 x2 |  0 x1  0 x0 | x3 y3 x2 y2 | x1 y1 x0 y0
	v = bitfieldReverse(v) >> 24;				// 0  0  0  0 |  0  0  0  0 | y0 x0 y1 x1 | y2 x2 y3 x3
	return float(v) * (1.0/256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

// Hash without Sine
// https://www.shadertoy.com/view/4djSRW 
float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * .1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float whitenoise(vec2 p)
{
	return whitenoise01(p) - 0.5;
}

// Convert uniform distribution to triangle-shaped distribution
// Input in [0..1], output in [-1..1]
// Based on https://www.shadertoy.com/view/4t2SDh 
float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

vec3 UchimuraTonemap(vec3 x)
{
        const float P = 1.0;
        const float a = 1.0;
        const float m = 0.22;
        const float l = 0.4;
        const float c = 1.33;
        const float b = 0.0;

        float l0 = ((P - m) * l) / a;
        float S0 = m + l0;
        float S1 = m + a * l0;
        float C2 = (a * P) / (P - S1);
        float CP = -C2 / P;

        vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
        vec3 w2 = vec3(step(m + l0, x));
        vec3 w1 = vec3(1.0) - w0 - w2;

        vec3 T = vec3(m * pow(x / m, vec3(c)) + b);
        vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
        vec3 L = vec3(m + a * (x - m));

        return clamp(T * w0 + L * w1 + S * w2, 0.0, 1.0);
}

vec3 LottesTonemap(vec3 x)
{
        const vec3 a = vec3(1.6);
        const vec3 d = vec3(0.977);
        const vec3 hdrMax = vec3(8.0);
        const vec3 midIn = vec3(0.18);
        const vec3 midOut = vec3(0.267);

        const vec3 b = (-pow(midIn, a) + pow(hdrMax, a) * midOut)
                / ((pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut);
        const vec3 c = (pow(hdrMax, a * d) * pow(midIn, a)
                - pow(hdrMax, a) * pow(midIn, a * d) * midOut)
                / ((pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut);

        return clamp(pow(x, a) / (pow(x, a * d) * b + c), 0.0, 1.0);
}

#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

layout(location=0) uniform vec4 Params;
layout(location=1) uniform vec4 DoFParams0; // x: enabled, y: focus distance, z: focus range, w: max blur radius (pixels)
layout(location=2) uniform vec4 DoFParams1; // x: near plane, y: far plane, z: reversed-Z flag (>0.5 when reversed)
layout(location=3) uniform vec4 ViewRect;   // xy: view min (normalized), zw: view max (normalized)
layout(location=4) uniform vec4 DepthParams; // xy: inverse view scale, zw: unused
layout(location=5) uniform vec3 HDRParams; // x: bloom intensity, y: exposure, z: tonemap mode
layout(location=6) uniform vec4 MotionParams0; // x: enabled, y: shutter strength, z: min velocity (pixels), w: depth threshold ratio
layout(location=7) uniform vec4 MotionParams1; // x: max blur radius (pixels), y: max samples, z: velocity texture available, w: reserved
layout(location=8) uniform vec4 PostFXParams0; // x: vignette strength, y: inner radius, z: outer radius, w: falloff
layout(location=9) uniform vec4 PostFXParams1; // xyz: vignette color, w: blend mode
layout(location=10) uniform vec4 PostFXParams2; // x: vignette noise amount, y: chromatic aberration (pixels), zw: reserved

const int MOTION_MAX_SAMPLES = 64;
const float OPAQUE_ALPHA_THRESHOLD = 0.999;

struct DepthSamplingInfo
{
        vec2 viewMinPx;
        vec2 viewMaxPx;
        vec2 invViewScale;
        vec2 depthTexSize;
        vec2 maxDepthIdx;
        bool valid;
};

DepthSamplingInfo MakeDepthSamplingInfo()
{
        DepthSamplingInfo info;
        info.depthTexSize = vec2(textureSize(DepthTexture, 0));
        info.valid = info.depthTexSize.x > 0.0 && info.depthTexSize.y > 0.0;
        if (!info.valid)
        {
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
        vec2 depthSizePx = max(vec2(1.0), floor(viewSizePx * info.invViewScale + vec2(0.0001)));
        info.maxDepthIdx = max(depthSizePx - vec2(1.0), vec2(0.0));
        return info;
}

vec2 ComputeDepthUV(vec2 fragPx, DepthSamplingInfo info)
{
        if (!info.valid)
                return vec2(-1.0);
        vec2 viewSizePx = max(info.viewMaxPx - info.viewMinPx, vec2(0.0));
        vec2 relPx = clamp(fragPx - info.viewMinPx, vec2(0.0), max(viewSizePx - vec2(1e-4), vec2(0.0)));
        vec2 depthIdx = clamp(floor(relPx * info.invViewScale), vec2(0.0), info.maxDepthIdx);
        vec2 depthPx = info.viewMinPx + depthIdx + vec2(0.5);
        return depthPx / info.depthTexSize;
}

float SampleLinearDepth(vec2 fragPx, DepthSamplingInfo info)
{
        vec2 depthUV = ComputeDepthUV(fragPx, info);
        if (depthUV.x < 0.0 || depthUV.y < 0.0)
                return 0.0;
        float rawDepth = texture(DepthTexture, depthUV).r;
        float nearPlane = DoFParams1.x;
        float farPlane = DoFParams1.y;
        float reversed = DoFParams1.z;
        if (reversed > 0.5)
        {
                float denom = nearPlane + rawDepth * (farPlane - nearPlane);
                return (nearPlane * farPlane) / max(denom, 1e-6);
        }
        else
        {
                float ndcDepth = rawDepth * 2.0 - 1.0;
                float denom = farPlane + nearPlane - ndcDepth * (farPlane - nearPlane);
                return (2.0 * nearPlane * farPlane) / max(denom, 1e-6);
        }
}

void AccumulateMotionSample(inout vec3 accum, inout float weight, vec2 sampleUV, vec2 sampleCoordPx, vec2 viewMin, vec2 viewMax, DepthSamplingInfo info, bool useDepth, float centerDepth, float depthThresholdRatio)
{
        if (!all(greaterThanEqual(sampleUV, viewMin)) || !all(lessThanEqual(sampleUV, viewMax)))
                return;
        if (useDepth)
        {
                float sampleDepth = SampleLinearDepth(sampleCoordPx, info);
                float tolerance = depthThresholdRatio * max(centerDepth, 1e-6);
                if (abs(sampleDepth - centerDepth) > tolerance)
                        return;
        }
        vec4 sampleColor = texture(GammaTexture, sampleUV);
        if (sampleColor.a < OPAQUE_ALPHA_THRESHOLD)
                return;
        accum += sampleColor.rgb;
        weight += 1.0;
}

layout(location=0) out vec4 out_fragcolor;

void main()
{
        float gamma = Params.x;
        float contrast = Params.y;
        float scale = Params.z;
        float dither = Params.w;
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        vec4 color = texelFetch(GammaTexture, pixel, 0);
        bool centerOpaque = color.a >= OPAQUE_ALPHA_THRESHOLD;
        vec2 texSize = vec2(textureSize(GammaTexture, 0));
        vec2 invTexSize = vec2(1.0) / max(texSize, vec2(1.0));
        vec2 uv = (vec2(pixel) + 0.5) / texSize;
        vec2 viewMin = ViewRect.xy;
        vec2 viewMax = ViewRect.zw;
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 invScale = max(DepthParams.xy, vec2(1e-4));
        bool inView = all(greaterThanEqual(uv, viewMin)) && all(lessThanEqual(uv, viewMax));
        DepthSamplingInfo depthInfo = MakeDepthSamplingInfo();

        bool hasVelocityTexture = MotionParams1.z > 0.5;
        vec2 velocity = vec2(0.0);
        float viewModelMask = 0.0;
        if (hasVelocityTexture && inView)
        {
                vec2 velocityUV = clamp((uv - viewMin) * invScale, vec2(0.0), viewSize * invScale);
                vec4 velocitySample = texture(VelocityTexture, velocityUV);
                velocity = velocitySample.xy;
                viewModelMask = velocitySample.z;
        }

        if (MotionParams0.x > 0.5 && inView && hasVelocityTexture && viewModelMask < 0.5 && centerOpaque)
        {
                float effectiveShutter = MotionParams0.y;
                if (effectiveShutter > 0.0)
                {
                        vec2 velocityPx = velocity * effectiveShutter * texSize;
                        float speed = length(velocityPx);
                        float minVelocity = max(MotionParams0.z, 0.0);
                        float maxRadius = MotionParams1.x;
                        int maxSamples = int(MotionParams1.y + 0.5);
                        maxSamples = clamp(maxSamples, 1, MOTION_MAX_SAMPLES);
                        if (maxRadius <= 0.0)
                                maxRadius = speed;
                        float radius = clamp(speed, 0.0, maxRadius);
                        if (radius > minVelocity && maxSamples > 0)
                        {
                                float radiusNormDenom = max(maxRadius, 1e-3);
                                float sampleCountF = clamp(radius / radiusNormDenom, 0.0, 1.0) * float(maxSamples);
                                int sampleCount = clamp(int(floor(sampleCountF + 0.5)), 1, maxSamples);
                                vec2 direction = speed > 1e-4 ? (velocityPx / speed) : vec2(0.0);
                                bool useDepth = MotionParams0.w > 0.0 && depthInfo.valid;
                                float centerDepth = 0.0;
                                float depthThresholdRatio = max(MotionParams0.w, 0.0);
                                if (useDepth)
                                        centerDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
                                vec3 accum = color.rgb;
                                float weight = 1.0;
                                float jitter = SCREEN_SPACE_NOISE();
                                for (int i = 1; i <= MOTION_MAX_SAMPLES; ++i)
                                {
                                        if (i > sampleCount)
                                                break;
                                        float t = (float(i) - 0.5 + jitter) / float(sampleCount);
                                        t = clamp(t, 0.0, 1.0);
                                        vec2 offsetPx = direction * (t * radius);
                                        if (length(offsetPx) < 1e-6)
                                                continue;
                                        vec2 offsetUV = offsetPx * invTexSize;
                                        vec2 sampleUVPos = uv + offsetUV;
                                        vec2 sampleUVNeg = uv - offsetUV;
                                        vec2 fragCoordPos = gl_FragCoord.xy + offsetPx;
                                        vec2 fragCoordNeg = gl_FragCoord.xy - offsetPx;
                                        AccumulateMotionSample(accum, weight, sampleUVPos, fragCoordPos, viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio);
                                        AccumulateMotionSample(accum, weight, sampleUVNeg, fragCoordNeg, viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio);
                                }
                                if (weight > 0.0)
                                        color.rgb = accum / weight;
                        }
                }
        }

        if (DoFParams0.x > 0.5 && inView && depthInfo.valid && viewModelMask < 0.5 && centerOpaque)
        {
                float linearDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
                float focusDistance = DoFParams0.y;
                float focusRange = max(DoFParams0.z, 0.0001);
                float maxBlur = max(DoFParams0.w, 0.0);
                float coc = abs(linearDepth - focusDistance);
                float blurFactor = clamp((coc - focusRange) / focusRange, 0.0, 1.0);
                float blurRadius = blurFactor * maxBlur;
                if (blurRadius > 0.0001)
                {
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
                        float noise = SCREEN_SPACE_NOISE();
                        float angle = noise * 6.28318530718;
                        float sine = sin(angle);
                        float cosine = cos(angle);
                        mat2 rotation = mat2(cosine, -sine, sine, cosine);
                        vec3 accum = color.rgb;
                        float weight = 1.0;
                        for (int i = 0; i < 8; ++i)
                        {
                                vec2 offset = rotation * kernel[i] * blurRadius * invTexSize;
                                vec4 sampleColor = texture(GammaTexture, uv + offset);
                                if (sampleColor.a < OPAQUE_ALPHA_THRESHOLD)
                                        continue;
                                accum += sampleColor.rgb;
                                weight += 1.0;
                        }
                        color.rgb = accum / weight;
                }
        }

        if (inView)
        {
                vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
                float aspect = texSize.y > 0.0 ? texSize.x / texSize.y : 1.0;

                float vignetteStrength = clamp(PostFXParams0.x, 0.0, 1.0);
                float vignetteInner = max(PostFXParams0.y, 0.0);
                float vignetteOuter = max(PostFXParams0.z, vignetteInner + 1e-3);
                float vignetteFalloff = max(PostFXParams0.w, 1e-3);
                vec3 vignetteColor = clamp(PostFXParams1.xyz, vec3(0.0), vec3(1.0));
                int vignetteBlendMode = clamp(int(PostFXParams1.w + 0.5), 0, 2);
                float vignetteNoise = clamp(PostFXParams2.x, 0.0, 0.1);
                if (vignetteStrength > 0.0 && vignetteOuter > vignetteInner)
                {
                        vec2 vignetteCoord = viewUV * 2.0 - vec2(1.0);
                        vignetteCoord.x *= aspect;
                        float dist = length(vignetteCoord);
                        float range = max(vignetteOuter - vignetteInner, 1e-3);
                        float fade = clamp((dist - vignetteInner) / range, 0.0, 1.0);
                        if (vignetteNoise > 0.0)
                        {
                                float noise = whitenoise(gl_FragCoord.xy);
                                fade = clamp(fade + noise * vignetteNoise * fade, 0.0, 1.0);
                        }
                        float vignette = pow(fade, vignetteFalloff);
                        float intensity = min(vignette * vignetteStrength, 1.0);
                        if (intensity > 0.0)
                        {
                                if (vignetteBlendMode == 1)
                                {
                                        vec3 overlayColor = vignetteColor;
                                        vec3 overlayDark = 2.0 * color.rgb * overlayColor;
                                        vec3 overlayLight = 1.0 - 2.0 * (1.0 - color.rgb) * (1.0 - overlayColor);
                                        vec3 overlayResult = mix(overlayDark, overlayLight, step(0.5, color.rgb));
                                        overlayResult = clamp(overlayResult, vec3(0.0), vec3(1.0));
                                        color.rgb = mix(color.rgb, overlayResult, intensity);
                                }
                                else if (vignetteBlendMode == 2)
                                {
                                        color.rgb = clamp(color.rgb + vignetteColor * intensity, vec3(0.0), vec3(1.0));
                                }
                                else
                                {
                                        vec3 multiplier = mix(vec3(1.0), vignetteColor, intensity);
                                        color.rgb *= multiplier;
                                }
                        }
                }

                float chromaticAmount = max(PostFXParams2.y, 0.0);
                if (chromaticAmount > 0.0)
                {
                        vec2 dir = viewUV - vec2(0.5);
                        float lenDir = length(dir);
                        if (lenDir > 1e-4)
                        {
                                vec2 normDir = dir / lenDir;
                                vec2 offsetPx = normDir * (chromaticAmount * lenDir);
                                vec2 offsetUV = offsetPx * invTexSize;
                                vec2 uvR = clamp(uv + offsetUV, viewMin, viewMax);
                                vec2 uvB = clamp(uv - offsetUV, viewMin, viewMax);
                                vec3 aberrated = color.rgb;
                                aberrated.r = texture(GammaTexture, uvR).r;
                                aberrated.b = texture(GammaTexture, uvB).b;
                                float blend = clamp(chromaticAmount * lenDir, 0.0, 1.0);
                                color.rgb = mix(color.rgb, aberrated, blend);
                        }
                }
        }

        out_fragcolor = color;
#if PALETTIZE == 1
                vec2 noiseuv = floor(gl_FragCoord.xy * scale) + 0.5;
                out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
	out_fragcolor.rgb += DITHER_NOISE(noiseuv) * dither;
	out_fragcolor.rgb *= out_fragcolor.rgb;
#endif // PALETTIZE == 1
#if PALETTIZE
	ivec3 clr = ivec3(clamp(out_fragcolor.rgb, 0., 1.) * 127. + 0.5);
	uint remap = Palette[texelFetch(PaletteLUT, clr, 0).x];
	out_fragcolor.rgb = vec3(UnpackRGB8(remap)) * (1./255.);
#else
        vec3 hdrColor = out_fragcolor.rgb;
        float bloomIntensity = HDRParams.x;
        vec3 bloomColor = vec3(0.0);
        if (bloomIntensity > 0.0)
        {
                bloomColor = texture(BloomTexture, uv).rgb * bloomIntensity;
        }
        float exposure = max(HDRParams.y, 0.0);
        float tonemapMode = HDRParams.z;
        vec3 combined = (hdrColor + bloomColor) * exposure * contrast;
        combined = max(combined, vec3(0.0));
        vec3 mapped;
        if (tonemapMode > 0.5)
        {
                if (tonemapMode < 1.5)
                        mapped = UchimuraTonemap(combined);
                else
                        mapped = LottesTonemap(combined);
        }
        else
        {
                mapped = clamp(combined, 0.0, 1.0);
        }
        mapped = clamp(mapped, 0.0, 1.0);
        out_fragcolor = vec4(pow(mapped, vec3(gamma)), 1.0);
#endif // PALETTIZE
}
