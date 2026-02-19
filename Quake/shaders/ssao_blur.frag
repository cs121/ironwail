layout(binding=0) uniform sampler2D SSAOTexture;
layout(binding=1) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4  u_screenParams;  // xy: inv screen size, zw: screen size
layout(location=1) uniform vec4  u_aoParams;      // xy: inv AO size, zw: AO size
layout(location=2) uniform vec4  u_params0;       // xy: blur direction, zw: unused
layout(location=3) uniform vec4  u_depthParams;   // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=4) uniform vec4  u_params1;       // x: sigma, y: radius, z: depth threshold scale, w: bilateral toggle
layout(location=5) uniform vec4  u_viewRect;      // xy: view min, zw: view max
layout(location=6) uniform int   u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc
layout(location=7) uniform int   u_yFlip;         // 0: none, 1: flip Y
layout(location=8) uniform mat4  u_invProj;

#include "depth_common.glsl"

layout(location=0) out vec4 outColor;

vec2 ApplyYFlip(vec2 uv)
{
        if (u_yFlip != 0)
                uv.y = 1.0 - uv.y;
        return uv;
}

vec2 ScreenInvSize() { return u_screenParams.xy; }
vec2 ScreenSize()    { return u_screenParams.zw; }
vec2 AoInvSize()     { return u_aoParams.xy; }
vec2 AoSize()        { return u_aoParams.zw; }

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
        vec2 aoSize = max(AoSize(), vec2(1.0));
        vec2 screenPixel = floor(aoPixel * ScreenSize() / aoSize);
        return ClampScreenPixel(screenPixel);
}

vec2 ScreenUvFromAoPixel(vec2 aoPixel)
{
        vec2 screenPixel = aoPixel * AoToScreenScale() + vec2(0.5);
        return ApplyYFlip(screenPixel * ScreenInvSize());
}

vec2 ScreenUvFromAoUv(vec2 uv)
{
        vec2 aoPixel = floor(uv * AoSize());
        return ScreenUvFromAoPixel(aoPixel);
}

ivec2 ScreenPixelFromUv(vec2 uv)
{
        vec2 unflipped = UnflipUv(uv);
        vec2 screenPixel = floor(unflipped * ScreenSize());
        return ClampScreenPixel(screenPixel);
}

// Returns view-space depth (+X forward). Scalar-only to avoid full vec3 reconstruct.
float ViewZFromDepth(vec2 uv, float depth01, bool reversedZ)
{
        float ndcDepth = DepthRawToNdcDebug(depth01, reversedZ ? 1.0 : 0.0, u_reversedZMode);
        vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4 view = u_invProj * clip;
        float w = view.w;
        if (abs(w) < 1e-6)
                return 1e30;
        return view.x / w;
}

bool IsInvalidFloat(float v)
{
        return !(v > -1e20 && v < 1e20);
}

float Gaussian(float x, float sigma)
{
        float denom = max(2.0 * sigma * sigma, 1e-6);
        return exp(-x * x / denom);
}

void main()
{
        vec2 aoPixel = floor(gl_FragCoord.xy);
        vec2 uv      = AoUvFromPixel(aoPixel);

        if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
        {
                outColor = vec4(1.0);
                return;
        }

        ivec2 screenPixel    = ScreenPixelFromAoPixelNearest(aoPixel);
        vec2  screenUv       = ScreenUvFromPixel(screenPixel);
        float centerDepthRaw = texelFetch(DepthTexture, screenPixel, 0).r;

        if (DepthIsSkyDepth(centerDepthRaw, u_depthParams.z, u_depthParams.w))
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

        float sigma  = max(u_params1.x, 0.01);
        // FIX (perf): Clamp radius to the actual loop bound (4) so the loop can be
        // statically bounded and the compiler won't emit dead iterations.
        int   radius = clamp(int(u_params1.y + 0.5), 0, 4);
        bool  useBilateral    = (u_params1.w > 0.5);
        float depthThreshold  = max(u_params1.z, 0.0) * max(centerDepth, 1e-4);

        vec2  direction    = u_params0.xy;
        vec2  invResolution = AoInvSize();

        float total = 0.0;
        float accum = 0.0;

        // FIX (perf): Loop bounds now match clamped radius — no wasted iterations.
        // FIX (bug): Bilateral depthWeight formula corrected:
        //   OLD: smoothstep(0, 1, depthThreshold / depthDiff)   → weight→1 when diff→0 (ok),
        //        but inverted semantics and denominator instability.
        //   NEW: 1 - smoothstep(0, depthThreshold, depthDiff)   → clean, stable, correct:
        //        diff=0 → weight=1, diff≥threshold → weight=0.
        for (int i = -4; i <= 4; ++i)
        {
                if (abs(i) > radius)
                        continue;

                vec2 offset    = direction * (float(i) * invResolution);
                vec2 sampleUV  = clamp(uv + offset, u_viewRect.xy, u_viewRect.zw);

                float gaussW = Gaussian(float(i), sigma);

                float depthWeight = 1.0;
                if (useBilateral)
                {
                        vec2  sampleDepthUv  = ScreenUvFromAoUv(sampleUV);
                        float sampleDepthRaw = texelFetch(DepthTexture,
                                ScreenPixelFromUv(sampleDepthUv), 0).r;

                        if (DepthIsSkyDepth(sampleDepthRaw, u_depthParams.z, u_depthParams.w))
                                continue;

                        float sampleDepth = ViewZFromDepth(sampleDepthUv, sampleDepthRaw, reversedZ);
                        if (IsInvalidFloat(sampleDepth))
                                continue;

                        float depthDiff = abs(sampleDepth - centerDepth);
                        // Correct bilateral weight: weight→1 for similar depths, 0 for different.
                        depthWeight = 1.0 - smoothstep(0.0, max(depthThreshold, 1e-6), depthDiff);
                }

                float weight = gaussW * depthWeight;
                accum += texture(SSAOTexture, sampleUV).r * weight;
                total += weight;
        }

        float ao = (total > 0.0) ? (accum / total) : 1.0;
        outColor = vec4(ao, ao, ao, 1.0);
}
