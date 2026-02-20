layout(binding=0) uniform sampler2D DepthTexture;
layout(binding=1) uniform sampler2D NoiseTexture;

#include "frame_uniforms.glsl"
#include "depth_common.glsl"

layout(location=0) uniform mat4 u_proj;
layout(location=1) uniform mat4 u_invProj;
layout(location=2) uniform vec4 u_params0; // x: radius, y: bias, z: power, w: min AO
layout(location=3) uniform vec4 u_screenParams; // xy: inv screen size, zw: screen size
layout(location=4) uniform vec4 u_aoParams; // xy: inv AO size, zw: AO size
layout(location=5) uniform vec4 u_noiseParams; // xy: noise scale, z: noise enabled, w: noise seed
layout(location=6) uniform vec4 u_depthParams; // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=7) uniform vec4 u_viewRect; // xy: view min, zw: view max
layout(location=8) uniform int u_samples;
layout(location=9) uniform vec4 u_debugParams; // x: debug mode, y: debug far
layout(location=10) uniform int u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc
layout(location=11) uniform int u_normalSource; // 0: neighbor, 1: derivatives
layout(location=12) uniform int u_yFlip; // 0: none, 1: flip Y
layout(location=13) uniform int u_noiseMode; // 0: off, 1: IGN, 2: texture
layout(location=14) uniform vec4 u_fogParams; // x: max distance

layout(location=0) out vec4 outColor;

const int SSAO_MAX_SAMPLES = 32;
const vec3 SSAO_KERNEL[SSAO_MAX_SAMPLES] = vec3[](
        vec3(0.5381, 0.1856, 0.4319),
        vec3(0.1379, 0.2486, 0.4430),
        vec3(0.3371, 0.5679, 0.0057),
        vec3(-0.6999, -0.0451, 0.0019),
        vec3(0.0689, -0.1598, 0.8547),
        vec3(0.0560, 0.0069, 0.1843),
        vec3(-0.0146, 0.1402, 0.0762),
        vec3(0.0100, -0.1924, 0.0344),
        vec3(-0.3577, -0.5301, 0.4358),
        vec3(-0.3169, 0.1063, 0.0158),
        vec3(0.0103, -0.5869, 0.0046),
        vec3(-0.0897, -0.4940, 0.3287),
        vec3(0.7119, -0.0154, 0.0918),
        vec3(-0.0533, 0.0596, 0.5411),
        vec3(0.0352, -0.0631, 0.5460),
        vec3(-0.4776, 0.2847, 0.0271),
        vec3(0.6281, 0.2908, 0.1163),
        vec3(0.3174, -0.1646, 0.5042),
        vec3(-0.2505, 0.4580, 0.0136),
        vec3(0.2067, -0.3752, 0.0631),
        vec3(-0.6681, -0.5057, 0.1395),
        vec3(0.1885, 0.4704, 0.1952),
        vec3(0.4441, 0.1386, 0.1782),
        vec3(-0.0791, 0.3005, 0.4190),
        vec3(-0.1153, 0.5775, 0.1735),
        vec3(0.4251, 0.0601, 0.1049),
        vec3(0.0587, -0.6517, 0.0442),
        vec3(-0.0837, 0.1302, 0.5580),
        vec3(0.1036, 0.0927, 0.1655),
        vec3(-0.0347, -0.3855, 0.3385),
        vec3(-0.3756, 0.4976, 0.0275),
        vec3(0.2397, -0.1794, 0.3984)
);

// -----------------------------------------------------------------------
// SSAO conventions:
// - View space uses +X forward (camera looks down +X).
// - Reverse-Z (clip control): NDC depth [0..1]; otherwise [-1..1].
// - All occlusion comparisons are in view space using +X depth.
// -----------------------------------------------------------------------

// FIX: Moved u_yFlip branch into a scalar ternary to avoid divergent flow.
vec2 ApplyYFlip(vec2 uv)
{
        return (u_yFlip != 0) ? vec2(uv.x, 1.0 - uv.y) : uv;
}

// Same inverse – kept symmetric with ApplyYFlip.
vec2 UnflipUv(vec2 uv)
{
        return (u_yFlip != 0) ? vec2(uv.x, 1.0 - uv.y) : uv;
}

vec2 ScreenInvSize() { return u_screenParams.xy; }
vec2 ScreenSize()    { return u_screenParams.zw; }
vec2 AoInvSize()     { return u_aoParams.xy; }
vec2 AoSize()        { return u_aoParams.zw; }

vec2 AoPixelCoord()
{
        return floor(gl_FragCoord.xy);
}

vec2 AoUvFromPixel(vec2 aoPixel)
{
        return ApplyYFlip((aoPixel + 0.5) * AoInvSize());
}

vec2 ViewMinPx()
{
        return floor(u_viewRect.xy * ScreenSize());
}

vec2 ViewMaxPx()
{
        vec2 minPx = ViewMinPx();
        vec2 maxPx = floor(u_viewRect.zw * ScreenSize() - vec2(1.0));
        return max(minPx, maxPx);
}

ivec2 ClampScreenPixel(vec2 pixel)
{
        vec2 maxPx = ScreenSize() - vec2(1.0);
        return ivec2(clamp(pixel, vec2(0.0), maxPx));
}

ivec2 ScreenPixelFromAoPixelNearest(vec2 aoPixel)
{
        vec2 aoSize = max(AoSize(), vec2(1.0));
        // Snap to integer depth texel to avoid half-res UV drift/banding.
        vec2 screenPixel = floor(aoPixel * ScreenSize() / aoSize);
        return ClampScreenPixel(screenPixel);
}

ivec2 ScreenPixelFromUv(vec2 uv)
{
        vec2 unflipped = UnflipUv(uv);
        vec2 screenPixel = floor(unflipped * ScreenSize());
        return ClampScreenPixel(screenPixel);
}

vec2 ScreenUvFromPixel(ivec2 pixel)
{
        return ApplyYFlip((vec2(pixel) + 0.5) * ScreenInvSize());
}

// -----------------------------------------------------------------------
// Reconstruct view-space position from a raw depth sample.
// FIX: Uses u_invProj instead of manual linear reconstruction so that
//      reversed-Z / clip-control mismatches can't accumulate.
// -----------------------------------------------------------------------
vec3 ReconstructViewPos(vec2 uv, float depth)
{
        float ndcDepth = DepthRawToNdcDebug(depth, u_depthParams.z, u_reversedZMode);
        vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4 view = u_invProj * clip;
        // Guard against degenerate w (sky / near-clip artefacts).
        if (abs(view.w) < 1e-6)
                return vec3(1e30);
        return view.xyz / view.w;
}

// Returns positive view-space depth (+X forward).
// PERF: Inlined to a single ReconstructViewPos call; no redundant matrix multiply.
float ViewZFromDepth(vec2 uv, float depth01)
{
        return ReconstructViewPos(uv, depth01).x;
}

float DepthRawFromPixel(ivec2 pixel)
{
        return texelFetch(DepthTexture, pixel, 0).r;
}

float DepthRawFromUv(vec2 uv)
{
        return DepthRawFromPixel(ScreenPixelFromUv(uv));
}

// -----------------------------------------------------------------------
// Normal reconstruction via screen-space finite differences.
// FIX: after normalize(), length check on the already-normalised result
//      was redundant (always ~1). Replaced with a NaN guard on the cross
//      product inputs, and removed the redundant normalize-after-flip.
// -----------------------------------------------------------------------
vec3 ComputeNormalFromViewPos(vec3 viewPos)
{
        vec3 dx = dFdx(viewPos);
        vec3 dy = dFdy(viewPos);
        // Bail early on degenerate input (e.g. sky/far-clip fragments).
        if (dot(dx, dx) < 1e-10 || dot(dy, dy) < 1e-10)
                return vec3(0.0, 0.0, 1.0);
        vec3 normal = normalize(cross(dx, dy));
        if (dot(normal, -viewPos) < 0.0)
                normal = -normal;
        return normal;
}

vec3 ReconstructNormalFromDepth(vec2 uv)
{
        ivec2 centerPixel = ScreenPixelFromUv(uv);
        vec2 viewMinPx = ViewMinPx();
        vec2 viewMaxPx = ViewMaxPx();

        float centerDepth = DepthRawFromPixel(centerPixel);
        if (DepthIsSkyDepth(centerDepth, u_depthParams.z, u_depthParams.w))
                return vec3(0.0, 0.0, 1.0);

        // QUALITY: Use the better-depth neighbour on each axis to reduce edge artifacts.
        ivec2 rightPixel = ivec2(clamp(vec2(centerPixel + ivec2(1, 0)), viewMinPx, viewMaxPx));
        ivec2 leftPixel  = ivec2(clamp(vec2(centerPixel - ivec2(1, 0)), viewMinPx, viewMaxPx));
        ivec2 upPixel    = ivec2(clamp(vec2(centerPixel + ivec2(0, 1)), viewMinPx, viewMaxPx));
        ivec2 downPixel  = ivec2(clamp(vec2(centerPixel - ivec2(0, 1)), viewMinPx, viewMaxPx));

        vec2 uvCenter = ScreenUvFromPixel(centerPixel);
        vec3 p = ReconstructViewPos(uvCenter, centerDepth);

        float depthRight = DepthRawFromPixel(rightPixel);
        // Fallback to opposite neighbour when primary is sky.
        ivec2 chosenRightPixel = (DepthIsSkyDepth(depthRight, u_depthParams.z, u_depthParams.w))
                                 ? leftPixel : rightPixel;
        float chosenDepthRight = (chosenRightPixel == rightPixel) ? depthRight
                                 : DepthRawFromPixel(leftPixel);
        vec3 pr = ReconstructViewPos(ScreenUvFromPixel(chosenRightPixel), chosenDepthRight);

        float depthUp = DepthRawFromPixel(upPixel);
        ivec2 chosenUpPixel = (DepthIsSkyDepth(depthUp, u_depthParams.z, u_depthParams.w))
                              ? downPixel : upPixel;
        float chosenDepthUp = (chosenUpPixel == upPixel) ? depthUp
                              : DepthRawFromPixel(downPixel);
        vec3 pu = ReconstructViewPos(ScreenUvFromPixel(chosenUpPixel), chosenDepthUp);

        // FIX: Guard degenerate cross product before normalizing.
        vec3 d0 = pr - p;
        vec3 d1 = pu - p;
        if (dot(d0, d0) < 1e-10 || dot(d1, d1) < 1e-10)
                return vec3(0.0, 0.0, 1.0);

        vec3 normal = normalize(cross(d0, d1));
        if (dot(normal, -p) < 0.0)
                normal = -normal;
        return normal;
}

// -----------------------------------------------------------------------
// Interleaved Gradient Noise – stable per-pixel angle for kernel rotation.
// -----------------------------------------------------------------------
float RandIGN(ivec2 pixel, float seed)
{
        // FIX: Cast to float once; avoids repeated integer-to-float promotion.
        float x = float(pixel.x);
        float y = float(pixel.y);
        return fract(52.9829189 * fract(0.06711056 * x + 0.00583715 * y + seed));
}

// -----------------------------------------------------------------------
// Fog helpers (unchanged logic, no bugs found).
// -----------------------------------------------------------------------
float FogTransmittanceFromViewPos(vec3 viewPos)
{
        float density = abs(Fog.w);
        if (density <= 0.0)
                return 1.0;
        return clamp(exp2(-density * dot(viewPos, viewPos)), 0.0, 1.0);
}

float FogFactorFromViewPos(vec3 viewPos)
{
        return 1.0 - FogTransmittanceFromViewPos(viewPos);
}

// -----------------------------------------------------------------------
// Lightweight validity check: detects NaN/Inf without isinf()/isnan()
// extensions (compatibility with older GLSL targets).
// -----------------------------------------------------------------------
bool IsInvalidFloat(float v) { return !(v > -1e20 && v < 1e20); }
bool IsInvalidVec3(vec3 v)   { return IsInvalidFloat(v.x) || IsInvalidFloat(v.y) || IsInvalidFloat(v.z); }

// -----------------------------------------------------------------------
void main()
{
        vec2 aoPixel = AoPixelCoord();
        vec2 uv      = AoUvFromPixel(aoPixel);

        // PERF: Decode debug mode once; avoids repeated float→int casts.
        int debugMode = (u_debugParams.x >= 0.5) ? int(u_debugParams.x + 0.5) : -1;
        float debugFar = max(u_debugParams.y, 1e-3);

        // Out-of-viewport early exit.
        if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
        {
                outColor = vec4(1.0);
                return;
        }

        // ---------------------------------------------------------------
        // Noise / kernel rotation vector.
        // FIX: noiseEnabled guard (u_noiseParams.z) is now combined with
        //      u_noiseMode so a single branch covers both conditions.
        // ---------------------------------------------------------------
        vec2 noiseVec;
        if (u_noiseParams.z > 0.5 && u_noiseMode > 0)
        {
                if (u_noiseMode == 2)
                {
                        // Texture-based jitter.
                        float noiseSeed = u_noiseParams.w;
                        vec2 noiseUV = fract((aoPixel + vec2(noiseSeed, noiseSeed * 1.37) + 0.5)
                                            * AoInvSize() * u_noiseParams.xy);
                        // FIX: Guard zero-length noise sample to avoid normalize(vec2(0)).
                        vec2 raw = texture(NoiseTexture, noiseUV).rg * 2.0 - 1.0;
                        noiseVec = (dot(raw, raw) > 1e-8) ? normalize(raw) : vec2(1.0, 0.0);
                }
                else
                {
                        // IGN-based rotation.
                        float angle = RandIGN(ivec2(aoPixel), u_noiseParams.w) * 6.2831853;
                        noiseVec = vec2(cos(angle), sin(angle));
                }
        }
        else
        {
                noiseVec = vec2(1.0, 0.0);
        }

        if (debugMode == 8)
        {
                outColor = vec4(noiseVec * 0.5 + 0.5, 0.0, 1.0);
                return;
        }

        // ---------------------------------------------------------------
        // Sample the depth buffer at the nearest full-res screen texel.
        // ---------------------------------------------------------------
        ivec2 screenPixel = ScreenPixelFromAoPixelNearest(aoPixel);
        vec2  screenUv    = ScreenUvFromPixel(screenPixel);
        float depth       = DepthRawFromPixel(screenPixel);

        if (debugMode == 4)
        {
                outColor = vec4(vec3(depth), 1.0);
                return;
        }

        if (debugMode == 5)
        {
                if (DepthIsSkyDepth(depth, u_depthParams.z, u_depthParams.w))
                { outColor = vec4(1.0); return; }
                float viewZ = ViewZFromDepth(screenUv, depth);
                if (IsInvalidFloat(viewZ))
                { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
                float v = clamp(viewZ / debugFar, 0.0, 1.0);
                outColor = vec4(v, v, v, 1.0);
                return;
        }

        if (debugMode == 6)
        {
                if (DepthIsSkyDepth(depth, u_depthParams.z, u_depthParams.w))
                { outColor = vec4(1.0); return; }
                vec3 viewPos = ReconstructViewPos(screenUv, depth);
                if (IsInvalidVec3(viewPos))
                { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
                float v = clamp(length(viewPos) / debugFar, 0.0, 1.0);
                outColor = vec4(v, v, v, 1.0);
                return;
        }

        // Sky / far-clip: no occlusion.
        if (DepthIsSkyDepth(depth, u_depthParams.z, u_depthParams.w))
        {
                outColor = vec4(1.0);
                return;
        }

        // ---------------------------------------------------------------
        // Reconstruct view-space position and normal.
        // ---------------------------------------------------------------
        vec3 viewPos = ReconstructViewPos(screenUv, depth);
        if (IsInvalidVec3(viewPos))
        {
                outColor = (debugMode >= 0) ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(1.0);
                return;
        }

        vec3 normal = (u_normalSource != 0)
                      ? ComputeNormalFromViewPos(viewPos)
                      : ReconstructNormalFromDepth(screenUv);
        if (IsInvalidVec3(normal))
        {
                outColor = (debugMode >= 0) ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(1.0);
                return;
        }

        if (debugMode == 7)
        {
                outColor = vec4(normal * 0.5 + 0.5, 1.0);
                return;
        }

        // ---------------------------------------------------------------
        // Build TBN from noise-rotated tangent frame.
        // FIX: Project noiseVec into the tangent plane before
        //      constructing the TBN so the basis is always orthonormal
        //      even when noiseVec is nearly parallel to the normal.
        // ---------------------------------------------------------------
        vec3 tangentRaw = vec3(noiseVec, 0.0);
        tangentRaw      = tangentRaw - normal * dot(tangentRaw, normal);
        // Guard degenerate case (noiseVec ≈ normal).
        float tangentLen = dot(tangentRaw, tangentRaw);
        vec3 tangent     = (tangentLen > 1e-8) ? tangentRaw * inversesqrt(tangentLen)
                                               : vec3(0.0, 1.0, 0.0);
        vec3 bitangent   = cross(normal, tangent);
        mat3 tbn         = mat3(tangent, bitangent, normal);

        // ---------------------------------------------------------------
        // SSAO sample loop.
        // ---------------------------------------------------------------
        float radius = u_params0.x;
        float bias   = u_params0.y;
        // QUALITY: Pre-compute inverse radius for range-check normalisation.
        float invRadius = 1.0 / max(radius, 1e-6);

        float occlusion    = 0.0;
        float validSamples = 0.0;
        int   samples      = clamp(u_samples, 1, SSAO_MAX_SAMPLES);

        for (int i = 0; i < SSAO_MAX_SAMPLES; ++i)
        {
                if (i >= samples)
                        break;

                vec3 sampleVec = tbn * SSAO_KERNEL[i];
                vec3 samplePos = viewPos + sampleVec * radius;

                // Project sample into clip space.
                vec4 offset = u_proj * vec4(samplePos, 1.0);
                if (offset.w <= 1e-6)
                        continue;

                vec2 sampleUV = offset.xy / offset.w * 0.5 + 0.5;
                sampleUV = ApplyYFlip(sampleUV);
                if (!all(greaterThanEqual(sampleUV, u_viewRect.xy)) ||
                    !all(lessThanEqual(sampleUV, u_viewRect.zw)))
                        continue;

                float sampleDepth = DepthRawFromUv(sampleUV);
                if (DepthIsSkyDepth(sampleDepth, u_depthParams.z, u_depthParams.w))
                        continue;

                float sampleViewDepth = ViewZFromDepth(sampleUV, sampleDepth);
                if (IsInvalidFloat(sampleViewDepth))
                        continue;

                validSamples += 1.0;

                float depthDelta = samplePos.x - sampleViewDepth;

                // QUALITY: Normalise range-check by radius so the falloff is
                //          scale-invariant (avoids hard cutoff at large radii).
                // FIX: The original used abs(depthDelta) in the denominator
                //      which inverts the ramp for deltas > radius; clamp
                //      abs(depthDelta) to [0, radius] before dividing.
                float normDelta = clamp(abs(depthDelta) * invRadius, 0.0, 1.0);
                float rangeCheck = 1.0 - normDelta;

                if (depthDelta > bias)
                        occlusion += rangeCheck;
        }

        if (debugMode == 9)
        {
                outColor = vec4(vec3(clamp(validSamples / float(samples), 0.0, 1.0)), 1.0);
                return;
        }

        // ---------------------------------------------------------------
        // Compute final AO value.
        // FIX: Divide occlusion by validSamples (not total samples) to
        //      avoid under-estimating occlusion on surfaces near screen
        //      edges where many samples are discarded.
        // ---------------------------------------------------------------
        float ao = (validSamples > 0.0)
                   ? clamp(1.0 - occlusion / validSamples, 0.0, 1.0)
                   : 1.0;
        ao = pow(ao, u_params0.z);
        ao = max(ao, u_params0.w);

        // Distance fade: suppress AO beyond fog max distance.
        float viewZ     = viewPos.x;
        float maxDist   = max(u_fogParams.x, 1.0);
        if (viewZ > maxDist)
                ao = 1.0;

        // Blend AO toward 1 in fogged areas.
        float fogFactor   = FogFactorFromViewPos(viewPos);
        float aoFogWeight = smoothstep(0.0, 0.6, 1.0 - clamp(fogFactor, 0.0, 1.0));

        if (debugMode == 3)
        {
                outColor = vec4(vec3(clamp(fogFactor, 0.0, 1.0)), 1.0);
                return;
        }
        if (debugMode == 2)
        {
                outColor = vec4(vec3(ao * aoFogWeight), 1.0);
                return;
        }
        if (debugMode == 1)
        {
                outColor = vec4(vec3(ao), 1.0);
                return;
        }

        ao = mix(1.0, ao, aoFogWeight);
        outColor = vec4(ao, ao, ao, 1.0);
}
