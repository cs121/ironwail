layout(binding=0) uniform sampler2D GammaTexture;
layout(binding=1) uniform usampler3D PaletteLUT;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D BloomTexture;
layout(binding=4) uniform sampler2D VelocityTexture;
layout(binding=8) uniform sampler2D SSAOTexture;
layout(binding=9) uniform sampler2DArray PostFXLUT;
layout(std430, binding=0) restrict readonly buffer PaletteBuffer
{
	uint Palette[256];
};
#include "frame_uniforms.glsl"

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
layout(location=10) uniform vec4 PostFXParams2; // x: vignette noise amount, yzw: unused
layout(location=11) uniform vec4 TeleportParams; // x: teleport fade, y: blur radius (pixels)
layout(location=12) uniform float u_saturation;
layout(location=17) uniform vec4 SSAOParams; // x: intensity, y: debug mode, z: upscale nearest, w: fog damp strength
layout(location=18) uniform vec4 SSAOBlurParams; // x: blur sigma, y: blur radius, z: depth threshold scale, w: fog damp power
layout(location=20) uniform float u_midtone;
layout(location=21) uniform vec4 PostFXParams3; // x: exposure add (stops), y: reserved, z: emissive boost, w: damage tint
layout(location=22) uniform vec4 PostFXParams4; // x: lut strength, y: underwater grade strength, z: underwater fog strength, w: vignette softness
layout(location=23) uniform vec4 PostFXLUTParams; // x: lut size, y: lut id, z: unused, w: unused
layout(location=24) uniform vec4 PostFXFogColor; // rgb: underwater fog color, w: scene fog density (saved before Fog_DisableGFog)
layout(location=25) uniform vec4 DamageDVParams0; // x: trauma, y: time, z: unused, w: unused
layout(location=26) uniform vec4 DamageDirParams; // xy: damage dir in view space (screen), z: dir strength, w: unused

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

float FogTransmittanceFromGlobalFog(float depth)
{
        // FIX #1: Use PostFXFogColor.w (scene fog density saved in gl_rmain.c before
        // Fog_DisableGFog clears it) instead of Fog.w from the frame_uniforms UBO.
        // Fog_DisableGFog() runs in R_RenderView before SCR_UpdateScreen calls
        // GL_PostProcess, so Fog.w is always 0 here → fog damping had zero effect.
        // gl_rmain.c now writes r_ssao_saved_fog[3] into PostFXFogColor.w (uniform 24.w).
        float density = abs(PostFXFogColor.w); // .w = saved scene fog density
        if (density <= 0.0)
                return 1.0;
        float fog = exp2(-density * depth * depth);
        return clamp(fog, 0.0, 1.0);
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

float SanitizeScalar(float x)
{
        return (x == x) ? clamp(x, 0.0, 65504.0) : 0.0;
}

vec3 SanitizeColor(vec3 color)
{
        return vec3(
                SanitizeScalar(color.x),
                SanitizeScalar(color.y),
                SanitizeScalar(color.z));
}

#include "postprocess_motion.glsl"
#include "postprocess_dof.glsl"

layout(location=0) out vec4 out_fragcolor;

// Apply soft-emulation palette/dither after the rest of the postprocess chain so
// all operate on the unquantized intermediate color first.
vec4 ApplySoftEmulationPostFX(vec3 color, vec2 fragCoord, float scale, float dither)
{
        vec4 outColor = vec4(color, 1.0);
#if PALETTIZE == 1
        vec2 noiseuv = floor(fragCoord * scale) + 0.5;
        outColor.rgb = sqrt(outColor.rgb);
        outColor.rgb += DITHER_NOISE(noiseuv) * dither;
        outColor.rgb *= outColor.rgb;
#endif // PALETTIZE == 1
#if PALETTIZE
        ivec3 clr = ivec3(clamp(outColor.rgb, 0., 1.) * 127. + 0.5);
        uint remap = Palette[texelFetch(PaletteLUT, clr, 0).x];
        outColor.rgb = vec3(UnpackRGB8(remap)) * (1./255.);
#endif // PALETTIZE
        return outColor;
}

void main()
{
        float postContrast = Params.y;
        float scale = Params.z;
        float dither = Params.w;
        int debugMode = int(floor(ColorSpaceParams.x + 0.5));
        int lightingDebugView = int(floor(ColorSpaceParams.w + 0.5));
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
	bool inView = all(greaterThanEqual(uv, viewMin)) && all(lessThanEqual(uv, viewMax));
	DepthSamplingInfo depthInfo = MakeDepthSamplingInfo();

	if (lightingDebugView >= 1 && lightingDebugView <= 7)
	{
		vec3 debugColor = clamp(color.rgb, 0.0, 1.0);
		if (outputSrgb > 0.5)
			debugColor = LinearToSRGB(debugColor);
		out_fragcolor = ApplySoftEmulationPostFX(debugColor, gl_FragCoord.xy, scale, dither);
		return;
	}

	bool hasVelocityTexture = MotionParams1.z > 0.5;
	vec2 velocity = vec2(0.0);
	float viewModelMask = 0.0;
	int materialMask = 0;
	if (hasVelocityTexture && inView)
	{
		ivec2 velocitySize = textureSize(VelocityTexture, 0);
		vec2 velocityUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
		ivec2 velocityCoord = ivec2(velocityUV * vec2(velocitySize));
		velocityCoord = clamp(velocityCoord, ivec2(0), max(velocitySize - ivec2(1), ivec2(0)));
		vec4 velocitySample = texelFetch(VelocityTexture, velocityCoord, 0);
		velocity = velocitySample.xy;
		viewModelMask = velocitySample.z;
		materialMask = int(floor(velocitySample.w + 0.5));
        }

        ApplyMotionBlur(color, uv, viewMin, viewMax, texSize, invTexSize, inView, centerOpaque, hasVelocityTexture, velocity, depthInfo, viewModelMask);

        ApplyDepthOfField(color, uv, invTexSize, inView, centerOpaque, depthInfo, viewModelMask);

        if (inView)
        {
                float trauma = clamp(DamageDVParams0.x, 0.0, 1.0);
                float t = DamageDVParams0.y;
                const float strength = 1.9;
                const float maxPx = 24.0;
                const float freq = 9.0;
                if (trauma > 1e-3)
                {
                        vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
                        float a = clamp(trauma * strength, 0.0, 1.35);
                        float osc = sin(t * freq) * 0.75 + sin(t * freq * 0.37 + 1.7) * 0.25;
                        float jitter = (InterleavedGradientNoise(vec2(t * 0.123, t * 0.917)) - 0.5) * 0.6;
                        float px = maxPx * (0.3 + 0.7 * a) * (0.7 + 0.3 * abs(osc));
                        vec2 baseDir = vec2(cos(t * 1.7), sin(t * 1.31));
                        vec2 dvDir = baseDir + vec2(jitter, -jitter);
                        float dvDirLen = length(dvDir);
                        dvDir = (dvDirLen > 1e-4) ? (dvDir / dvDirLen) : vec2(1.0, 0.0);
                        vec2 dvPerp = vec2(-dvDir.y, dvDir.x);
                        vec2 skewScale = vec2(px * (0.45 + 0.35 * a), px * (0.20 + 0.25 * a)) * invTexSize;
                        vec2 skewBase = viewUV - vec2(0.5);
                        vec2 skew1 = vec2(skewBase.y * skewScale.x, skewBase.x * skewScale.y);
                        vec2 skew2 = vec2(skewBase.y * skewScale.x * 1.5, -skewBase.x * skewScale.y * 0.7);
                        vec2 skew3 = vec2(skewBase.y * skewScale.x * 2.0, skewBase.x * skewScale.y * 0.35);
                        vec2 offset1 = (dvDir * (px * 0.95) + dvPerp * (px * 0.30)) * invTexSize + skew1;
                        vec2 offset2 = (-dvDir * (px * 0.72) + dvPerp * (px * 0.22)) * invTexSize + skew2;
                        vec2 offset3 = (dvDir * (px * 0.46) - dvPerp * (px * 0.58)) * invTexSize + skew3;
                        vec3 base = color.rgb;
                        vec3 ghost1 = texture(GammaTexture, clamp(uv + offset1, viewMin, viewMax)).rgb;
                        vec3 ghost2 = texture(GammaTexture, clamp(uv + offset2, viewMin, viewMax)).rgb;
                        vec3 ghost3 = texture(GammaTexture, clamp(uv + offset3, viewMin, viewMax)).rgb;
                        vec3 smear = texture(GammaTexture, clamp(uv + (offset1 + offset3) * 0.5, viewMin, viewMax)).rgb;
                        float w1 = 0.40 * a;
                        float w2 = 0.32 * a;
                        float w3 = 0.24 * a;
                        float w4 = 0.18 * a;
                        float baseWeight = max(0.0, 1.0 - (w1 + w2 + w3 + w4));
                        vec3 dvColor = base * baseWeight + ghost1 * w1 + ghost2 * w2 + ghost3 * w3 + smear * w4;
                        color.rgb = mix(base, dvColor, clamp(0.85 + 0.35 * a, 0.0, 1.0));
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
                                bool useDepthUpscale = ssaoUseDepth && (ssaoDebugMode <= 0 || ssaoDebugMode >= 3 || ssaoDebugMode == 10);
                                float ao = SampleSSAO(uv, depthInfo, ssaoCenterDepth, useDepthUpscale);
                                float ssaoFogStrength = clamp(SSAOParams.w, 0.0, 1.0);
                                float ssaoFogPower = max(SSAOBlurParams.w, 0.01);
				float fogTransmittance = 1.0;
				float fogSource = 1.0;
				if (ssaoUseDepth)
				{
					float fogStrength = clamp(PostFXParams4.z, 0.0, 1.0);
					float underwaterTransmittance = 1.0;
					if (fogStrength > 0.0)
						underwaterTransmittance = FogTransmittanceFromDepth(ssaoCenterDepth, fogStrength);
					fogTransmittance = FogTransmittanceFromGlobalFog(ssaoCenterDepth) * underwaterTransmittance;
				}
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
					vec3 sourceTint = vec3(0.0);
					if (fogSource > 0.5)
						sourceTint = vec3(1.0, 0.45, 0.1); // global source
					color.rgb = mix(vec3(fogTransmittance), sourceTint, 0.75);
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
                                        // Keep SSAO intensity bounded for t>1.0.
                                        // Previous mix(1, ao, t) extrapolated when t>1 and could
                                        // over-darken lit areas; pow preserves stronger AO while
                                        // keeping the multiplier in [0,1].
                                        float aoStrength = max(ssaoIntensity, 0.0);
                                        float aoFactor = pow(clamp(aoDamped, 0.0, 1.0), aoStrength);
                                        color.rgb *= aoFactor;
                                }
                        }
                }

        } // end if (inView)

        vec3 hdrColor = color.rgb;
        float emissiveBoost = 1.0 + max(PostFXParams3.z, 0.0);
        hdrColor *= emissiveBoost;
        float bloomIntensity = max(HDRParams.x, 0.0);
        vec3 bloomColor = vec3(0.0);
        if (bloomIntensity > 0.0)
        {
                bloomColor = texture(BloomTexture, uv).rgb * bloomIntensity;
        }
	float exposure = max(HDRParams.y, 0.0);
	float exposureAdd = clamp(PostFXParams3.x, -2.0, 2.0);
	exposure *= exp2(exposureAdd);
	float tonemapMode = HDRParams.z;
	vec3 combined = hdrColor * exposure + bloomColor;
	combined = SanitizeColor(combined);
        float fogStrength = clamp(PostFXParams4.z, 0.0, 1.0);
        if (fogStrength > 0.0 && depthInfo.valid)
        {
                float depth = SampleLinearDepth(vec2(pixel) + vec2(0.5), depthInfo);
                float fogFactor = clamp(depth / 2048.0, 0.0, 1.0);
                fogFactor = clamp(fogFactor * fogStrength, 0.0, 1.0);
                combined = mix(combined, PostFXFogColor.rgb, fogFactor);
        }
        combined = clamp(combined, vec3(0.0), vec3(32.0));
        if (lightingDebugView == 8)
        {
                vec3 debugColor = clamp(log2(vec3(1.0) + combined) * 0.25, 0.0, 1.0);
                if (outputSrgb > 0.5)
                        debugColor = LinearToSRGB(debugColor);
                out_fragcolor = ApplySoftEmulationPostFX(debugColor, gl_FragCoord.xy, scale, dither);
                return;
        }
        if (lightingDebugView == 9)
        {
                float maxHdr = max(max(combined.r, combined.g), combined.b);
                float over = max(maxHdr - 1.0, 0.0);
                vec3 debugColor = vec3(0.0);
                if (over > 0.0)
                {
                        float t0 = clamp(over, 0.0, 1.0);
                        float t1 = clamp(over - 1.0, 0.0, 1.0);
                        vec3 hot = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), t0);
                        debugColor = mix(hot, vec3(1.0), t1);
                }
                if (outputSrgb > 0.5)
                        debugColor = LinearToSRGB(debugColor);
                out_fragcolor = ApplySoftEmulationPostFX(debugColor, gl_FragCoord.xy, scale, dither);
                return;
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
        mapped = SanitizeColor(mapped);
        mapped = clamp(mapped, 0.0, 1.0);
        if (debugMode == 2)
        {
                out_fragcolor = ApplySoftEmulationPostFX(mapped, gl_FragCoord.xy, scale, dither);
                return;
        }
        if (debugMode == 4)
        {
                float maxHdr = max(max(combined.r, combined.g), combined.b);
                vec3 debugColor = maxHdr > 1.0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0);
                out_fragcolor = ApplySoftEmulationPostFX(debugColor, gl_FragCoord.xy, scale, dither);
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
                        vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
                        vec2 viewPos = viewUV * 2.0 - vec2(1.0);
                        float directional = 1.0;
                        float dirStrength = clamp(DamageDirParams.z, 0.0, 1.0);
                        vec2 dir = DamageDirParams.xy;
                        float dirLen = length(dir);
                        if (dirStrength > 1e-3 && dirLen > 1e-4)
                        {
                                vec2 ndir = dir / dirLen;
                                float facing = clamp(dot(viewPos, ndir), -1.0, 1.0);
                                float hemi = facing * 0.5 + 0.5;
                                float dirFactor = mix(0.45, 1.45, hemi);
                                directional = mix(1.0, dirFactor, dirStrength);
                        }
                        vec3 blood = vec3(0.34, 0.02, 0.02);
                        vec3 tinted = mapped * vec3(0.88, 0.54, 0.5);
                        vec3 boosted = clamp(tinted + blood * 0.45, vec3(0.0), vec3(1.0));
                        mapped = mix(mapped, boosted, clamp(damageTint * directional, 0.0, 1.0));
                }
        }
        if (outputSrgb > 0.5)
                mapped = LinearToSRGB(mapped);
        out_fragcolor = ApplySoftEmulationPostFX(mapped, gl_FragCoord.xy, scale, dither);
}
