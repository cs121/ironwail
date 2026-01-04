layout(binding=0) uniform sampler2D GammaTexture;
layout(binding=1) uniform usampler3D PaletteLUT;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D BloomTexture;
layout(binding=4) uniform sampler2D VelocityTexture;
layout(binding=5) uniform sampler2D GodraysTexture;
layout(binding=6) uniform sampler2D GodraysMaskTexture;
layout(binding=7) uniform sampler2D GodraysSourceTexture;
layout(binding=8) uniform sampler2D SSAOTexture;
layout(binding=9) uniform sampler2DArray PostFXLUT;
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

float InterleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 FilmGrainNoise(vec2 coord, float time, float seed, float colored)
{
	vec2 base = coord + vec2(seed, seed * 0.37);
	float n0 = tri(InterleavedGradientNoise(base + vec2(time * 11.1, time * 7.3)));
	float n1 = tri(InterleavedGradientNoise(base + vec2(17.7 + time * 5.2, 3.1 + time * 9.2)));
	float n2 = tri(InterleavedGradientNoise(base + vec2(8.3 + time * 6.7, 12.9 + time * 4.1)));
	vec3 colorNoise = (vec3(n0, n1, n2) + n0) * 0.5;
	return mix(vec3(n0), colorNoise, colored);
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
layout(location=10) uniform vec4 PostFXParams2; // x: vignette noise amount, y: screen-space darken strength, z: screen-space darken depth range, w: unused
layout(location=11) uniform vec4 TeleportParams; // x: teleport fade, y: blur radius (pixels)
layout(location=12) uniform float u_saturation;
layout(location=13) uniform vec4 FilmGrainParams0; // x: amount, y: size, z: speed, w: luma weight
layout(location=14) uniform vec4 FilmGrainParams1; // x: blend, y: color, z: debug, w: seed
layout(location=15) uniform vec4 FilmGrainParams2; // x: frame, yzw: unused
layout(location=16) uniform vec4 GodraysParams; // x: enabled, y: debug, z: debug source mode, w: unused
layout(location=17) uniform vec4 SSAOParams; // x: intensity, y: debug mode, z: upscale nearest, w: fog damp strength
layout(location=18) uniform vec4 SSAOBlurParams; // x: blur sigma, y: blur radius, z: depth threshold scale, w: fog damp power
layout(location=19) uniform vec4 ColorSpaceParams; // x: debug mode, y: unused, z: output sRGB conversion, w: reserved
layout(location=20) uniform float u_midtone;
layout(location=21) uniform vec4 PostFXParams3; // x: exposure add (stops), y: bloom boost, z: emissive boost, w: damage tint
layout(location=22) uniform vec4 PostFXParams4; // x: lut strength, y: underwater grade strength, z: underwater fog strength, w: vignette softness
layout(location=23) uniform vec4 PostFXLUTParams; // x: lut size, y: lut id, z: unused, w: unused
layout(location=24) uniform vec4 PostFXFogColor; // rgb: fog color, w: unused
layout(location=25) uniform vec4 DamageDVParams0; // x: trauma, y: strength, z: max offset px, w: frequency
layout(location=26) uniform vec4 DamageDVParams1; // x: time, y: quality, z: debug, w: unused

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

float FogTransmittanceFromDepth(float depth, float fogStrength)
{
        float fogFactor = clamp(depth / 2048.0, 0.0, 1.0);
        fogFactor = clamp(fogFactor * fogStrength, 0.0, 1.0);
        return 1.0 - fogFactor;
}

float SampleSSAO(vec2 uv, DepthSamplingInfo info, float centerDepth, bool useDepth)
{
        vec2 ssaoSize = vec2(textureSize(SSAOTexture, 0));
        vec2 colorSize = vec2(textureSize(GammaTexture, 0));
        if (all(lessThan(abs(ssaoSize - colorSize), vec2(0.5))))
                return texture(SSAOTexture, uv).r;

        bool upscaleNearest = SSAOParams.z > 0.5;
        if (upscaleNearest)
        {
                vec2 ssaoCoord = uv * ssaoSize;
                ivec2 ssaoPixel = ivec2(clamp(floor(ssaoCoord), vec2(0.0), ssaoSize - vec2(1.0)));
                return texelFetch(SSAOTexture, ssaoPixel, 0).r;
        }

        vec2 ssaoCoord = uv * ssaoSize - vec2(0.5);
        vec2 base = floor(ssaoCoord);
        vec2 frac = ssaoCoord - base;
        vec2 ssaoToScreen = colorSize / max(ssaoSize, vec2(1.0));
        float accum = 0.0;
        float total = 0.0;
        float depthThreshold = 0.02 * max(centerDepth, 1e-4);

        for (int y = 0; y <= 1; ++y)
        {
                for (int x = 0; x <= 1; ++x)
                {
                        vec2 offset = vec2(float(x), float(y));
                        vec2 aoTexel = clamp(base + offset, vec2(0.0), ssaoSize - vec2(1.0));
                        vec2 aoUV = (aoTexel + 0.5) / ssaoSize;
                        float weight = (x == 0 ? (1.0 - frac.x) : frac.x) * (y == 0 ? (1.0 - frac.y) : frac.y);
                        if (useDepth && info.valid)
                        {
                                vec2 sampleScreenPx = aoTexel * ssaoToScreen + vec2(0.5);
                                float sampleDepth = SampleLinearDepth(sampleScreenPx, info);
                                float depthDiff = abs(sampleDepth - centerDepth);
                                float depthWeight = smoothstep(0.0, 1.0, depthThreshold / max(depthDiff, 1e-4));
                                weight *= depthWeight;
                        }
                        accum += texture(SSAOTexture, aoUV).r * weight;
                        total += weight;
                }
        }

        if (total > 0.0)
                return accum / total;
        return texture(SSAOTexture, uv).r;
}

float Gaussian2(float r2, float sigma)
{
        float denom = max(2.0 * sigma * sigma, 1e-6);
        return exp(-r2 / denom);
}

vec3 ApplyPostContrast(vec3 color, float contrast)
{
        if (abs(contrast - 1.0) < 1e-4)
                return color;
        vec3 t = color * (vec3(1.0) - color);
        return clamp(color + t * ((contrast - 1.0) * 2.0), 0.0, 1.0);
}

vec3 ApplySaturation(vec3 color, float saturation)
{
        float luma = dot(color, vec3(0.299, 0.587, 0.114));
        return mix(vec3(luma), color, saturation);
}

vec3 ApplyPostFXLUT(vec3 color, float lutSize, int lutId)
{
        if (lutSize <= 1.0)
                return color;
        vec3 clamped = clamp(color, vec3(0.0), vec3(1.0));
        float size = max(lutSize, 2.0);
        float sliceSize = 1.0 / size;
        float slicePixelSize = sliceSize / size;
        float sliceInnerSize = slicePixelSize * (size - 1.0);
        vec3 scaled = clamped * (size - 1.0);
        float slice = scaled.b;
        float slice0 = floor(slice);
        float slice1 = min(slice0 + 1.0, size - 1.0);
        float lerp = slice - slice0;
        vec2 uv0 = vec2(slice0 * sliceSize + scaled.r * sliceInnerSize + slicePixelSize * 0.5,
                        scaled.g * sliceInnerSize + slicePixelSize * 0.5);
        vec2 uv1 = vec2(slice1 * sliceSize + scaled.r * sliceInnerSize + slicePixelSize * 0.5,
                        scaled.g * sliceInnerSize + slicePixelSize * 0.5);
        vec3 sample0 = texture(PostFXLUT, vec3(uv0, float(lutId))).rgb;
        vec3 sample1 = texture(PostFXLUT, vec3(uv1, float(lutId))).rgb;
        return mix(sample0, sample1, lerp);
}

vec3 LinearToSRGB(vec3 color)
{
        vec3 cutoff = step(vec3(0.0031308), color);
        vec3 lower = color * 12.92;
        vec3 higher = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
        return mix(lower, higher, cutoff);
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
        float postContrast = Params.y;
        float scale = Params.z;
        float dither = Params.w;
        int debugMode = int(floor(ColorSpaceParams.x + 0.5));
        float outputSrgb = ColorSpaceParams.z;
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

        if (GodraysParams.z > 0.5)
        {
                vec4 source = texture(GodraysSourceTexture, uv);
                if (GodraysParams.z < 1.5)
                        out_fragcolor = vec4(source.rgb, 1.0);
                else
                        out_fragcolor = vec4(vec3(source.a), 1.0);
                return;
        }
        if (GodraysParams.y > 0.5)
        {
                vec3 maskColor = texture(GodraysMaskTexture, uv).rgb;
                vec3 shaftsColor = vec3(0.0);
                if (GodraysParams.x > 0.5)
                        shaftsColor = texture(GodraysTexture, uv).rgb;
                out_fragcolor = vec4(max(maskColor, shaftsColor), 1.0);
                return;
        }

        bool hasVelocityTexture = MotionParams1.z > 0.5;
        vec2 velocity = vec2(0.0);
        float viewModelMask = 0.0;
        int materialMask = 0;
        if (hasVelocityTexture && inView)
        {
                vec2 velocityUV = clamp((uv - viewMin) * invScale, vec2(0.0), viewSize * invScale);
                vec4 velocitySample = texture(VelocityTexture, velocityUV);
                velocity = velocitySample.xy;
                viewModelMask = velocitySample.z;
                materialMask = int(floor(velocitySample.w + 0.5));
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
                float trauma = clamp(DamageDVParams0.x, 0.0, 1.0);
                float strength = max(DamageDVParams0.y, 0.0);
                float maxPx = max(DamageDVParams0.z, 0.0);
                float freq = max(DamageDVParams0.w, 0.0);
                float quality = DamageDVParams1.y;
                float debug = DamageDVParams1.z;
                if (trauma > 1e-3 && strength > 0.0 && maxPx > 0.0 && quality > 0.5)
                {
                        float t = DamageDVParams1.x;
                        float a = trauma * strength;
                        float osc = sin(t * freq) * 0.75 + sin(t * freq * 0.37 + 1.7) * 0.25;
                        float jitter = (InterleavedGradientNoise(vec2(t * 0.123, t * 0.917)) - 0.5) * 0.6;
                        float px = maxPx * a * (0.65 + 0.35 * abs(osc));
                        vec2 baseDir = vec2(cos(t * 1.7), sin(t * 1.31));
                        vec2 dir = baseDir + vec2(jitter, -jitter);
                        float dirLen = length(dir);
                        dir = (dirLen > 1e-4) ? (dir / dirLen) : vec2(1.0, 0.0);
                        vec2 offset1 = dir * px * invTexSize;
                        vec2 offset2 = vec2(-dir.y, dir.x) * (px * 0.75) * invTexSize;
                        vec3 base = color.rgb;
                        vec3 ghost1 = texture(GammaTexture, clamp(uv + offset1, viewMin, viewMax)).rgb;
                        vec3 ghost2 = texture(GammaTexture, clamp(uv - offset2, viewMin, viewMax)).rgb;
                        float w1 = 0.28 * a;
                        float w2 = 0.22 * a;
                        vec3 dvColor = base * (1.0 - (w1 + w2)) + ghost1 * w1 + ghost2 * w2;
                        if (quality > 1.5)
                        {
                                vec2 offset3 = (offset1 + offset2) * 0.6;
                                vec3 ghost3 = texture(GammaTexture, clamp(uv + offset3, viewMin, viewMax)).rgb;
                                float w3 = 0.18 * a;
                                dvColor = dvColor * (1.0 - w3) + ghost3 * w3;
                                vec3 smear = texture(GammaTexture, clamp(uv + offset1 * 0.5, viewMin, viewMax)).rgb;
                                dvColor = mix(dvColor, smear, 0.12 * a);
                        }
                        color.rgb = (debug > 0.5) ? vec3(a) : dvColor;
                }
        }

        if (inView)
        {
                vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
                float aspect = texSize.y > 0.0 ? texSize.x / texSize.y : 1.0;

                float screenDarkenStrength = clamp(PostFXParams2.y, 0.0, 1.0);
                float screenDarkenDepth = max(PostFXParams2.z, 1e-4);
                if (screenDarkenStrength > 0.0 && depthInfo.valid)
                {
                        float linearDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
                        float depthRange = max(DoFParams1.y - DoFParams1.x, 1e-6);
                        float normalizedDepth = clamp((linearDepth - DoFParams1.x) / depthRange, 0.0, 1.0);
                        float screenAO = smoothstep(0.0, screenDarkenDepth, 1.0 - normalizedDepth);
                        color.rgb *= mix(vec3(1.0), vec3(screenAO), screenDarkenStrength);
                }

                float vignetteStrength = clamp(PostFXParams0.x, 0.0, 1.0);
                float vignetteInner = max(PostFXParams0.y, 0.0);
                float vignetteOuter = max(PostFXParams0.z, vignetteInner + 1e-3);
                float vignetteFalloff = max(PostFXParams0.w, 1e-3);
                float vignetteSoftness = clamp(PostFXParams4.w, 0.0, 1.0);
                vignetteFalloff = mix(vignetteFalloff, 1.0, vignetteSoftness);
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

                float teleportFade = clamp(TeleportParams.x, 0.0, 1.0);
                float teleportBlur = max(TeleportParams.y, 0.0);
                if (teleportFade > 0.0)
                {
                        if (teleportBlur > 0.0)
                        {
                                vec2 blurOffset = teleportBlur * invTexSize;
                                vec3 blurAccum = color.rgb;
                                blurAccum += texture(GammaTexture, clamp(uv + vec2(blurOffset.x, 0.0), viewMin, viewMax)).rgb;
                                blurAccum += texture(GammaTexture, clamp(uv - vec2(blurOffset.x, 0.0), viewMin, viewMax)).rgb;
                                blurAccum += texture(GammaTexture, clamp(uv + vec2(0.0, blurOffset.y), viewMin, viewMax)).rgb;
                                blurAccum += texture(GammaTexture, clamp(uv - vec2(0.0, blurOffset.y), viewMin, viewMax)).rgb;
                                color.rgb = blurAccum * 0.2;
                        }
                        color.rgb += vec3(0.5, 0.3, 1.0) * teleportFade;
                }
                }

                float ssaoIntensity = SSAOParams.x;
                int ssaoDebugMode = int(floor(SSAOParams.y + 0.5));
                bool ssaoInView = inView;
                bool ssaoAllowed = (materialMask & 2) == 0 && viewModelMask < 0.5;
                if ((ssaoDebugMode > 0 || ssaoIntensity > 0.0) && ssaoInView)
                {
                        if (!ssaoAllowed)
                        {
                                if (ssaoDebugMode > 0)
                                        color.rgb = (ssaoDebugMode == 12) ? vec3(0.0) : vec3(1.0);
                        }
                        else
                        {
                                float ssaoCenterDepth = 0.0;
                                bool ssaoUseDepth = depthInfo.valid;
                                if (ssaoUseDepth)
                                        ssaoCenterDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
                                bool useDepthUpscale = ssaoUseDepth && (ssaoDebugMode <= 0 || ssaoDebugMode <= 3 || ssaoDebugMode == 10);
                                float ao = SampleSSAO(uv, depthInfo, ssaoCenterDepth, useDepthUpscale);
                                float ssaoFogStrength = clamp(SSAOParams.w, 0.0, 1.0);
                                float ssaoFogPower = max(SSAOBlurParams.w, 0.01);
                                float fogStrength = clamp(PostFXParams4.z, 0.0, 1.0);
                                float fogTransmittance = 1.0;
                                if (ssaoUseDepth && fogStrength > 0.0)
                                        fogTransmittance = FogTransmittanceFromDepth(ssaoCenterDepth, fogStrength);
                                float aoFogMask = pow(clamp(fogTransmittance, 0.0, 1.0), ssaoFogPower);
                                float aoFogged = mix(1.0, ao, aoFogMask);
                                float aoDamped = mix(ao, aoFogged, ssaoFogStrength);
                                if (ssaoDebugMode == 12)
                                {
                                        float mask = centerOpaque ? (1.0 - ao) : 0.0;
                                        color.rgb = vec3(mask);
                                }
                                else if (ssaoDebugMode == 13)
                                {
                                        color.rgb = vec3(fogTransmittance);
                                }
                                else if (ssaoDebugMode == 14)
                                {
                                        color.rgb = vec3(aoDamped);
                                }
                                else if (ssaoDebugMode == 11)
                                {
                                        vec2 ssaoSize = vec2(textureSize(SSAOTexture, 0));
                                        vec2 colorSize = vec2(textureSize(GammaTexture, 0));
                                        vec2 ssaoCoord = uv * ssaoSize;
                                        ivec2 ssaoPixel = ivec2(clamp(floor(ssaoCoord), vec2(0.0), ssaoSize - vec2(1.0)));
                                        float rawAo = texelFetch(SSAOTexture, ssaoPixel, 0).r;
                                        float sigma = max(SSAOBlurParams.x, 0.01);
                                        int radius = int(SSAOBlurParams.y + 0.5);
                                        float depthThreshold = SSAOBlurParams.z * max(ssaoCenterDepth, 1e-4);
                                        vec2 ssaoToScreen = colorSize / max(ssaoSize, vec2(1.0));
                                        float accum = 0.0;
                                        float total = 0.0;
                                        float depthWeightSum = 0.0;
                                        float sampleCount = 0.0;

                                        for (int y = -4; y <= 4; ++y)
                                        {
                                                if (abs(y) > radius)
                                                        continue;
                                                for (int x = -4; x <= 4; ++x)
                                                {
                                                        if (abs(x) > radius)
                                                                continue;
                                                        vec2 offset = vec2(float(x), float(y));
                                                        vec2 sampleTexel = clamp(vec2(ssaoPixel) + offset, vec2(0.0), ssaoSize - vec2(1.0));
                                                        ivec2 samplePixel = ivec2(sampleTexel);
                                                        float sampleAo = texelFetch(SSAOTexture, samplePixel, 0).r;
                                                        float depthWeight = 1.0;
                                                        if (ssaoUseDepth)
                                                        {
                                                                vec2 sampleScreenPx = sampleTexel * ssaoToScreen + vec2(0.5);
                                                                float sampleDepth = SampleLinearDepth(sampleScreenPx, depthInfo);
                                                                float depthDiff = abs(sampleDepth - ssaoCenterDepth);
                                                                depthWeight = smoothstep(0.0, 1.0, depthThreshold / max(depthDiff, 1e-4));
                                                        }
                                                        float weight = Gaussian2(dot(offset, offset), sigma) * depthWeight;
                                                        accum += sampleAo * weight;
                                                        total += weight;
                                                        depthWeightSum += depthWeight;
                                                        sampleCount += 1.0;
                                                }
                                        }
                                        float blurred = (total > 0.0) ? (accum / total) : rawAo;
                                        float avgDepthWeight = (sampleCount > 0.0) ? (depthWeightSum / sampleCount) : 1.0;
                                        color.rgb = vec3(rawAo, blurred, avgDepthWeight);
                                }
                                else if (ssaoDebugMode > 0)
                                {
                                        color.rgb = vec3(ao);
                                }
                                else if (centerOpaque)
                                {
                                        color.rgb *= mix(1.0, aoDamped, ssaoIntensity);
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
        float bloomIntensity = max(HDRParams.x + PostFXParams3.y, 0.0);
        vec3 bloomColor = vec3(0.0);
        if (bloomIntensity > 0.0)
        {
                bloomColor = texture(BloomTexture, uv).rgb * bloomIntensity;
        }
        bloomColor *= 1.0 + max(PostFXParams3.z, 0.0);
        vec3 godraysColor = vec3(0.0);
        if (GodraysParams.x > 0.5)
                godraysColor = texture(GodraysTexture, uv).rgb;
        float exposure = max(HDRParams.y, 0.0);
        float exposureAdd = clamp(PostFXParams3.x, -2.0, 2.0);
        exposure *= exp2(exposureAdd);
        float tonemapMode = HDRParams.z;
        vec3 combined = (hdrColor + bloomColor + godraysColor) * exposure;
        combined = max(combined, vec3(0.0));
        float fogStrength = clamp(PostFXParams4.z, 0.0, 1.0);
        if (fogStrength > 0.0 && depthInfo.valid)
        {
                float depth = SampleLinearDepth(vec2(pixel) + vec2(0.5), depthInfo);
                float fogFactor = clamp(depth / 2048.0, 0.0, 1.0);
                fogFactor = clamp(fogFactor * fogStrength, 0.0, 1.0);
                combined = mix(combined, PostFXFogColor.rgb, fogFactor);
        }
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
        if (debugMode == 2)
        {
                out_fragcolor = vec4(mapped, 1.0);
                return;
        }
        if (debugMode == 4)
        {
                float maxHdr = max(max(combined.r, combined.g), combined.b);
                out_fragcolor = vec4(maxHdr > 1.0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0), 1.0);
                return;
        }
        float midtone = max(u_midtone, 0.1);
        if (abs(midtone - 1.0) > 1e-4)
                mapped = pow(mapped, vec3(1.0 / midtone));
        mapped = ApplyPostContrast(mapped, postContrast);
        mapped = ApplySaturation(mapped, u_saturation);
        {
                float lutStrength = clamp(PostFXParams4.x, 0.0, 1.0);
                float lutSize = PostFXLUTParams.x;
                int lutId = int(PostFXLUTParams.y + 0.5);
                if (lutStrength > 0.0 && lutSize > 1.0 && lutId > 0)
                {
                        vec3 lutColor = ApplyPostFXLUT(mapped, lutSize, lutId);
                        mapped = mix(mapped, lutColor, lutStrength);
                }
        }
        {
                float damageTint = clamp(PostFXParams3.w, 0.0, 1.0);
                if (damageTint > 0.0)
                {
                        vec3 tint = vec3(1.0, 0.15, 0.15);
                        mapped = mix(mapped, mapped * tint, damageTint);
                }
        }
        if (outputSrgb > 0.5)
                mapped = LinearToSRGB(mapped);
        out_fragcolor = vec4(mapped, 1.0);
#endif // PALETTIZE

	float grainAmount = clamp(FilmGrainParams0.x, 0.0, 1.0);
	float grainDebug = FilmGrainParams1.z;
	if (grainAmount > 0.0 || grainDebug > 0.5)
	{
		float grainSize = max(FilmGrainParams0.y, 0.01);
		float grainSpeed = max(FilmGrainParams0.z, 0.0);
		float lumaWeight = clamp(FilmGrainParams0.w, 0.0, 1.0);
		float blend = clamp(FilmGrainParams1.x, 0.0, 1.0);
		float colored = clamp(FilmGrainParams1.y, 0.0, 1.0);
		float seed = FilmGrainParams1.w;
		float frame = FilmGrainParams2.x;
		float time = frame * grainSpeed;
		vec2 grainCoord = (gl_FragCoord.xy + vec2(0.25, 0.75)) / grainSize;
		vec3 grain = FilmGrainNoise(grainCoord, time, seed, colored);

		float luma = dot(out_fragcolor.rgb, vec3(0.299, 0.587, 0.114));
		float shadow = clamp(1.0 - luma, 0.0, 1.0);
		float lumaFactor = mix(1.0, shadow, lumaWeight);
		float response = mix(1.0, smoothstep(0.0, 1.0, shadow), 0.5);
		float amount = grainAmount * lumaFactor * response;

		if (grainDebug > 0.5)
		{
			out_fragcolor.rgb = vec3(0.5) + grain * 0.5;
		}
		else
		{
			vec3 add = out_fragcolor.rgb + grain * amount;
			vec3 soft = out_fragcolor.rgb + (out_fragcolor.rgb - out_fragcolor.rgb * out_fragcolor.rgb) * grain * amount;
			out_fragcolor.rgb = clamp(mix(add, soft, blend), vec3(0.0), vec3(1.0));
		}
	}
}
