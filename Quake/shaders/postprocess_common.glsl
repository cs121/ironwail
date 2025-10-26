#ifndef POSTPROCESS_COMMON_GLSL
#define POSTPROCESS_COMMON_GLSL

uvec3 UnpackRGB8(uint c)
{
        return uvec3(c, c >> 8, c >> 16) & 255u;
}

// ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
        coord &= 15;
        coord.y ^= coord.x;
        uint v = uint(coord.y | (coord.x << 8));        // 0  0  0  0 | x3 x2 x1 x0 |  0  0  0  0 | y3 y2 y1 y0
        v = (v ^ (v << 2)) & 0x3333;                            // 0  0 x3 x2 |  0  0 x1 x0 |  0  0 y3 y2 |  0  0 y1 y0
        v = (v ^ (v << 1)) & 0x5555;                            // 0 x3  0 x2 |  0 x1  0 x0 |  0 y3  0 y2 |  0 y1  0 y0
        v |= v >> 7;                                                            // 0 x3  0 x2 |  0 x1  0 x0 | x3 y3 x2 y2 | x1 y1 x0 y0
        v = bitfieldReverse(v) >> 24;                           // 0  0  0  0 |  0  0  0  0 | y0 x0 y1 x1 | y2 x2 y3 x3
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
        vec2 viewMinPx = info.viewMinPx + vec2(0.5);
        vec2 viewMaxPx = max(info.viewMaxPx - vec2(0.5), viewMinPx);
        vec2 clampedFragPx = clamp(fragPx, viewMinPx, viewMaxPx);
        vec2 depthPx = info.viewMinPx + (clampedFragPx - info.viewMinPx - vec2(0.5)) * info.invViewScale + vec2(0.5);
        vec2 depthMinPx = info.viewMinPx + vec2(0.5);
        vec2 depthMaxPx = depthMinPx + info.maxDepthIdx;
        depthPx = clamp(depthPx, depthMinPx, max(depthMaxPx, depthMinPx));
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

#endif // POSTPROCESS_COMMON_GLSL
