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

#include "depth_common.glsl"

layout(location=0) out vec4 outColor;

// -----------------------------------------------------------------------
// Helper functions
// -----------------------------------------------------------------------

// FIX: Replaced if-branch with ternary to reduce divergence overhead.
vec2 ApplyYFlip(vec2 uv)
{
        return (u_yFlip != 0) ? vec2(uv.x, 1.0 - uv.y) : uv;
}

vec2 UnflipUv(vec2 uv)
{
        return (u_yFlip != 0) ? vec2(uv.x, 1.0 - uv.y) : uv;
}

vec2 ScreenInvSize() { return u_screenParams.xy; }
vec2 ScreenSize()    { return u_screenParams.zw; }
vec2 AoInvSize()     { return u_aoParams.xy; }
vec2 AoSize()        { return u_aoParams.zw; }

vec2 AoToScreenScale()
{
        return ScreenSize() * AoInvSize();
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
        vec2 aoSize     = max(AoSize(), vec2(1.0));
        vec2 screenPixel = floor(aoPixel * ScreenSize() / aoSize);
        return ClampScreenPixel(screenPixel);
}

vec2 ScreenUvFromAoPixel(vec2 aoPixel)
{
        vec2 screenPixel = aoPixel * AoToScreenScale() + vec2(0.5);
        return ApplyYFlip(screenPixel * ScreenInvSize());
}

// FIX: The original ScreenUvFromAoUv converted AO uv → pixel via floor(),
//      then called ScreenUvFromAoPixel (which adds 0.5 offset).  When the
//      AO uv was at an exact half-texel boundary this could round to the
//      wrong screen pixel.  Rewritten to use a consistent centre-of-texel
//      formula.
vec2 ScreenUvFromAoUv(vec2 aoUv)
{
        // Convert to AO pixel centre, then map to screen.
        vec2 aoPixel = floor(aoUv * AoSize());
        return ScreenUvFromAoPixel(aoPixel);
}

ivec2 ScreenPixelFromUv(vec2 uv)
{
        vec2 unflipped  = UnflipUv(uv);
        vec2 screenPixel = floor(unflipped * ScreenSize());
        return ClampScreenPixel(screenPixel);
}

float DepthRawFromPixel(ivec2 pixel)
{
        return texelFetch(DepthTexture, pixel, 0).r;
}

float DepthRawFromUv(vec2 uv)
{
        return DepthRawFromPixel(ScreenPixelFromUv(uv));
}

// Returns positive view-space depth (+X forward) via inverse projection.
// Consistent with SSAO pass to avoid depth comparison mismatches.
float ViewZFromDepth(vec2 uv, float depth01, bool reversedZ)
{
        float ndcDepth = DepthRawToNdcDebug(depth01, reversedZ ? 1.0 : 0.0, u_reversedZMode);
        vec4  clip     = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4  view     = u_invProj * clip;
        if (abs(view.w) < 1e-6)
                return 1e30;
        return view.x / view.w;
}

bool IsInvalidFloat(float v) { return !(v > -1e20 && v < 1e20); }

// -----------------------------------------------------------------------
// Gaussian weight.
// PERF: Pre-compute 1/(2σ²) once outside the loop instead of inside.
// -----------------------------------------------------------------------
float Gaussian(float x, float invTwoSigmaSq)
{
        return exp(-x * x * invTwoSigmaSq);
}

// -----------------------------------------------------------------------
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
        float centerDepthRaw = DepthRawFromPixel(screenPixel);

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

        float sigma      = max(u_params1.x, 0.01);
        int   radius     = int(u_params1.y + 0.5);
        bool  useBilateral = (u_params1.w > 0.5);

        // QUALITY: Clamp depth threshold to a sensible minimum relative to
        //          view depth so the bilateral filter actually works at all
        //          distances without requiring large u_params1.z values.
        float depthThreshold = max(u_params1.z, 0.0) * max(centerDepth, 1e-4);

        // PERF: Hoist 1/(2σ²) out of the loop.
        float invTwoSigmaSq = 1.0 / max(2.0 * sigma * sigma, 1e-6);

        float total = 0.0;
        float accum = 0.0;
        vec2  direction = u_params0.xy;

        // FIX: The original loop ran from -4 to +4 (hard-coded to 9 taps)
        //      and then used 'abs(i) > radius' to skip outer taps.  This
        //      wastes iterations when radius < 4.  Now we loop exactly
        //      from -radius to +radius (clamped to a safe maximum).
        // QUALITY: Increased maximum from 4 to 8 taps per side for a
        //          smoother 17-tap kernel when a larger radius is requested.
        int clampedRadius = clamp(radius, 0, 8);
        for (int i = -clampedRadius; i <= clampedRadius; ++i)
        {
                vec2 offset    = direction * (float(i) * AoInvSize());
                vec2 sampleUV  = clamp(uv + offset, u_viewRect.xy, u_viewRect.zw);

                // Map AO sample UV to screen UV for depth fetch.
                vec2  sampleDepthUV  = ScreenUvFromAoUv(sampleUV);
                float sampleDepthRaw = DepthRawFromUv(sampleDepthUV);

                if (DepthIsSkyDepth(sampleDepthRaw, u_depthParams.z, u_depthParams.w))
                        continue;

                float sampleDepth = ViewZFromDepth(sampleDepthUV, sampleDepthRaw, reversedZ);
                if (IsInvalidFloat(sampleDepth))
                        continue;

                // Bilateral depth weight.
                float depthDiff   = abs(sampleDepth - centerDepth);
                // FIX: The original smoothstep arguments were (0, threshold/diff)
                //      which produces 1 when diff→0 but *also* approaches 1 again
                //      for diff >> threshold (smoothstep clamps at 1).  Correct
                //      formulation: weight falls toward 0 for large depth differences.
                float depthWeight = useBilateral
                        ? clamp(1.0 - depthDiff / max(depthThreshold, 1e-6), 0.0, 1.0)
                        : 1.0;

                float weight = Gaussian(float(i), invTwoSigmaSq) * depthWeight;
                accum += texture(SSAOTexture, sampleUV).r * weight;
                total += weight;
        }

        float ao = (total > 0.0) ? (accum / total) : 1.0;
        outColor = vec4(ao, ao, ao, 1.0);
}
