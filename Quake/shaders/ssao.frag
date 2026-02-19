layout(binding=0) uniform sampler2D DepthTexture;
layout(binding=1) uniform sampler2D NoiseTexture;

#include "frame_uniforms.glsl"
#include "depth_common.glsl"

layout(location=0)  uniform mat4  u_proj;
layout(location=1)  uniform mat4  u_invProj;
layout(location=2)  uniform vec4  u_params0;      // x: radius, y: bias, z: power, w: min AO
layout(location=3)  uniform vec4  u_screenParams;  // xy: inv screen size, zw: screen size
layout(location=4)  uniform vec4  u_aoParams;      // xy: inv AO size, zw: AO size
layout(location=5)  uniform vec4  u_noiseParams;   // xy: noise scale, z: noise enabled, w: noise seed
layout(location=6)  uniform vec4  u_depthParams;   // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=7)  uniform vec4  u_viewRect;      // xy: view min, zw: view max
layout(location=8)  uniform int   u_samples;
layout(location=9)  uniform vec4  u_debugParams;   // x: debug mode, y: debug far
layout(location=10) uniform int   u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc
layout(location=11) uniform int   u_normalSource;  // 0: neighbor, 1: derivatives
layout(location=12) uniform int   u_yFlip;         // 0: none, 1: flip Y
layout(location=13) uniform int   u_noiseMode;     // 0: off, 1: IGN, 2: texture
layout(location=14) uniform vec4  u_fogParams;     // x: max distance

layout(location=0) out vec4 outColor;

const int SSAO_MAX_SAMPLES = 32;
const vec3 SSAO_KERNEL[SSAO_MAX_SAMPLES] = vec3[](
        vec3( 0.5381,  0.1856,  0.4319),
        vec3( 0.1379,  0.2486,  0.4430),
        vec3( 0.3371,  0.5679,  0.0057),
        vec3(-0.6999, -0.0451,  0.0019),
        vec3( 0.0689, -0.1598,  0.8547),
        vec3( 0.0560,  0.0069,  0.1843),
        vec3(-0.0146,  0.1402,  0.0762),
        vec3( 0.0100, -0.1924,  0.0344),
        vec3(-0.3577, -0.5301,  0.4358),
        vec3(-0.3169,  0.1063,  0.0158),
        vec3( 0.0103, -0.5869,  0.0046),
        vec3(-0.0897, -0.4940,  0.3287),
        vec3( 0.7119, -0.0154,  0.0918),
        vec3(-0.0533,  0.0596,  0.5411),
        vec3( 0.0352, -0.0631,  0.5460),
        vec3(-0.4776,  0.2847,  0.0271),
        vec3( 0.6281,  0.2908,  0.1163),
        vec3( 0.3174, -0.1646,  0.5042),
        vec3(-0.2505,  0.4580,  0.0136),
        vec3( 0.2067, -0.3752,  0.0631),
        vec3(-0.6681, -0.5057,  0.1395),
        vec3( 0.1885,  0.4704,  0.1952),
        vec3( 0.4441,  0.1386,  0.1782),
        vec3(-0.0791,  0.3005,  0.4190),
        vec3(-0.1153,  0.5775,  0.1735),
        vec3( 0.4251,  0.0601,  0.1049),
        vec3( 0.0587, -0.6517,  0.0442),
        vec3(-0.0837,  0.1302,  0.5580),
        vec3( 0.1036,  0.0927,  0.1655),
        vec3(-0.0347, -0.3855,  0.3385),
        vec3(-0.3756,  0.4976,  0.0275),
        vec3( 0.2397, -0.1794,  0.3984)
);

// SSAO conventions:
// - View space uses +X forward (camera looks down +X).
// - When reverse-Z (clip control) is enabled, NDC depth is [0..1]; otherwise [-1..1].
// - All comparisons are done in view space using +X depth.

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

vec2 AoPixelCoord()
{
        return floor(gl_FragCoord.xy);
}

vec2 AoUvFromPixel(vec2 aoPixel)
{
        return ApplyYFlip((aoPixel + 0.5) * AoInvSize());
}

vec2 UnflipUv(vec2 uv)
{
        if (u_yFlip != 0)
                uv.y = 1.0 - uv.y;
        return uv;
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
        // Snap AO pixels to integer depth texels to avoid half-res UV drift/banding.
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

vec3 ReconstructViewPos(vec2 uv, float depth)
{
        float ndcDepth = DepthRawToNdcDebug(depth, u_depthParams.z, u_reversedZMode);
        vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4 view = u_invProj * clip;
        float w = view.w;
        if (abs(w) < 1e-6)
                return vec3(1e30);
        return view.xyz / w;
}

// FIX (perf): Only reconstruct viewPos.x (view-space depth) without full vec3.
// Avoids a redundant full matrix multiply when only depth is needed in the AO loop.
float ReconstructViewDepthOnly(vec2 uv, float depth)
{
        float ndcDepth = DepthRawToNdcDebug(depth, u_depthParams.z, u_reversedZMode);
        vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4 view = u_invProj * clip;
        float w = view.w;
        if (abs(w) < 1e-6)
                return 1e30;
        return view.x / w;
}

// FIX (bug): Normal-length check moved BEFORE normalize to be meaningful.
// After normalize() the length is always ~1.0 so the old check was never triggered.
vec3 ComputeNormalFromViewPos(vec3 viewPos)
{
        vec3 dx = dFdx(viewPos);
        vec3 dy = dFdy(viewPos);
        vec3 crossed = cross(dx, dy);
        if (length(crossed) < 1e-4)
                return vec3(0.0, 0.0, 1.0);
        vec3 normal = normalize(crossed);
        // Orient reconstructed normals toward the camera for stable TBN.
        if (dot(normal, -viewPos) < 0.0)
                normal = -normal;
        return normal;
}

vec3 ReconstructNormalFromDepth(vec2 uv)
{
        ivec2 centerPixel = ScreenPixelFromUv(uv);
        vec2 viewMinPx = ViewMinPx();
        vec2 viewMaxPx = ViewMaxPx();
        ivec2 rightPixel = ivec2(clamp(vec2(centerPixel + ivec2(1,  0)), viewMinPx, viewMaxPx));
        ivec2 leftPixel  = ivec2(clamp(vec2(centerPixel - ivec2(1,  0)), viewMinPx, viewMaxPx));
        ivec2 upPixel    = ivec2(clamp(vec2(centerPixel + ivec2(0,  1)), viewMinPx, viewMaxPx));
        ivec2 downPixel  = ivec2(clamp(vec2(centerPixel - ivec2(0,  1)), viewMinPx, viewMaxPx));

        float centerDepth = texelFetch(DepthTexture, centerPixel, 0).r;
        if (DepthIsSkyDepth(centerDepth, u_depthParams.z, u_depthParams.w))
                return vec3(0.0, 0.0, 1.0);

        vec2 uvCenter = ScreenUvFromPixel(centerPixel);
        vec3 p = ReconstructViewPos(uvCenter, centerDepth);

        float depthRight = texelFetch(DepthTexture, rightPixel, 0).r;
        if (DepthIsSkyDepth(depthRight, u_depthParams.z, u_depthParams.w))
                depthRight = texelFetch(DepthTexture, leftPixel, 0).r;
        ivec2 rPx = DepthIsSkyDepth(depthRight, u_depthParams.z, u_depthParams.w) ? centerPixel : rightPixel;
        vec3 pr = ReconstructViewPos(ScreenUvFromPixel(rPx), depthRight);

        float depthUp = texelFetch(DepthTexture, upPixel, 0).r;
        if (DepthIsSkyDepth(depthUp, u_depthParams.z, u_depthParams.w))
                depthUp = texelFetch(DepthTexture, downPixel, 0).r;
        ivec2 uPx = DepthIsSkyDepth(depthUp, u_depthParams.z, u_depthParams.w) ? centerPixel : upPixel;
        vec3 pu = ReconstructViewPos(ScreenUvFromPixel(uPx), depthUp);

        vec3 crossed = cross(pr - p, pu - p);
        // FIX (bug): length check before normalize (same fix as ComputeNormalFromViewPos).
        if (length(crossed) < 1e-4)
                return vec3(0.0, 0.0, 1.0);
        vec3 normal = normalize(crossed);
        if (dot(normal, -p) < 0.0)
                normal = -normal;
        return normal;
}

float RandIGN(ivec2 pixel, float seed)
{
        float f = fract(0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y) + seed);
        return fract(52.9829189 * f);
}

// FIX (bug): Original used dot(viewPos, viewPos) which is distance-squared, making
// fog decay quadratically in a non-standard way. Use length() for linear distance.
float FogTransmittanceFromViewPos(vec3 viewPos)
{
        float density = abs(Fog.w);
        if (density <= 0.0)
                return 1.0;
        float dist = length(viewPos);
        float fog = exp2(-density * dist);
        return clamp(fog, 0.0, 1.0);
}

float FogFactorFromViewPos(vec3 viewPos)
{
        return 1.0 - FogTransmittanceFromViewPos(viewPos);
}

bool IsInvalidFloat(float v)
{
        return !(v > -1e20 && v < 1e20);
}

bool IsInvalidVec3(vec3 v)
{
        return IsInvalidFloat(v.x) || IsInvalidFloat(v.y) || IsInvalidFloat(v.z);
}

void main()
{
        vec2 aoPixel = AoPixelCoord();
        vec2 uv = AoUvFromPixel(aoPixel);

        int debugMode = -1;
        if (u_debugParams.x >= 0.5)
                debugMode = int(u_debugParams.x + 0.5);
        float debugFar = max(u_debugParams.y, 1e-3);

        if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
        {
                outColor = vec4(1.0);
                return;
        }

        // ---- Noise ----
        vec2 noiseVec;
        float noiseSeed    = u_noiseParams.w;
        float noiseEnabled = u_noiseParams.z;
        if (noiseEnabled > 0.5 && u_noiseMode > 0)
        {
                ivec2 noisePixel = ivec2(aoPixel);
                if (u_noiseMode == 2)
                {
                        vec2 noiseJitter = vec2(noiseSeed, noiseSeed * 1.37);
                        vec2 noiseUV = fract((aoPixel + noiseJitter + 0.5) * AoInvSize() * u_noiseParams.xy);
                        vec2 noiseSample = texture(NoiseTexture, noiseUV).rg * 2.0 - 1.0;
                        // FIX (robustness): Guard against zero-length noise sample.
                        float noiseLen = length(noiseSample);
                        noiseVec = (noiseLen > 1e-6) ? (noiseSample / noiseLen) : vec2(1.0, 0.0);
                }
                else
                {
                        float angle = RandIGN(noisePixel, noiseSeed) * 6.2831853;
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

        // ---- Depth / position ----
        ivec2 screenPixel = ScreenPixelFromAoPixelNearest(aoPixel);
        vec2  screenUv    = ScreenUvFromPixel(screenPixel);
        float depth       = texelFetch(DepthTexture, screenPixel, 0).r;

        if (debugMode == 4)
        {
                outColor = vec4(vec3(depth), 1.0);
                return;
        }
        if (debugMode == 5)
        {
                if (DepthIsSkyDepth(depth, u_depthParams.z, u_depthParams.w))
                {
                        outColor = vec4(1.0);
                        return;
                }
                float viewZ = ReconstructViewDepthOnly(screenUv, depth);
                if (IsInvalidFloat(viewZ))
                {
                        outColor = vec4(1.0, 0.0, 1.0, 1.0);
                        return;
                }
                float v = clamp(viewZ / debugFar, 0.0, 1.0);
                outColor = vec4(v, v, v, 1.0);
                return;
        }
        if (debugMode == 6)
        {
                if (DepthIsSkyDepth(depth, u_depthParams.z, u_depthParams.w))
                {
                        outColor = vec4(1.0);
                        return;
                }
                vec3 viewPos = ReconstructViewPos(screenUv, depth);
                if (IsInvalidVec3(viewPos))
                {
                        outColor = vec4(1.0, 0.0, 1.0, 1.0);
                        return;
                }
                float v = clamp(length(viewPos) / debugFar, 0.0, 1.0);
                outColor = vec4(v, v, v, 1.0);
                return;
        }
        if (DepthIsSkyDepth(depth, u_depthParams.z, u_depthParams.w))
        {
                outColor = vec4(1.0);
                return;
        }

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

        // ---- TBN ----
        // FIX (robustness): Fallback when noiseVec is nearly parallel to normal.
        vec3 noiseVec3 = vec3(noiseVec, 0.0);
        vec3 tangent   = noiseVec3 - normal * dot(noiseVec3, normal);
        float tangentLen = length(tangent);
        if (tangentLen < 1e-5)
        {
                // Pick an arbitrary perpendicular axis.
                noiseVec3 = (abs(normal.x) < 0.9) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
                tangent   = noiseVec3 - normal * dot(noiseVec3, normal);
                tangentLen = length(tangent);
        }
        tangent /= tangentLen;
        vec3 bitangent = cross(normal, tangent);
        mat3 tbn = mat3(tangent, bitangent, normal);

        // ---- AO sampling ----
        float radius = u_params0.x;
        float bias   = u_params0.y;
        float occlusion    = 0.0;
        float validSamples = 0.0;

        // FIX (perf): Use direct loop bound instead of break-based workaround.
        int samples = clamp(u_samples, 1, SSAO_MAX_SAMPLES);
        float invRadius = 1.0 / max(radius, 1e-6);

        for (int i = 0; i < samples; ++i)
        {
                vec3 sampleVec = tbn * SSAO_KERNEL[i];
                vec3 samplePos = viewPos + sampleVec * radius;

                vec4 offset = u_proj * vec4(samplePos, 1.0);
                if (offset.w <= 1e-6)
                        continue;

                vec2 sampleUV = offset.xy / offset.w * 0.5 + 0.5;
                sampleUV = ApplyYFlip(sampleUV);
                if (!all(greaterThanEqual(sampleUV, u_viewRect.xy)) || !all(lessThanEqual(sampleUV, u_viewRect.zw)))
                        continue;

                ivec2 samplePixel = ScreenPixelFromUv(sampleUV);
                float sampleDepthRaw = texelFetch(DepthTexture, samplePixel, 0).r;
                if (DepthIsSkyDepth(sampleDepthRaw, u_depthParams.z, u_depthParams.w))
                        continue;

                // FIX (perf): Use lightweight scalar reconstruction instead of full vec3.
                vec2 sampleScreenUv = ScreenUvFromPixel(samplePixel);
                float sampleViewDepth = ReconstructViewDepthOnly(sampleScreenUv, sampleDepthRaw);
                if (IsInvalidFloat(sampleViewDepth))
                        continue;

                validSamples += 1.0;
                float depthDelta = samplePos.x - sampleViewDepth;

                // FIX (correctness): Clamp rangeCheck input to [0,1] before smoothstep
                // so the falloff doesn't unexpectedly saturate for very small deltas.
                float rangeInput = clamp(radius / max(abs(depthDelta), 1e-4), 0.0, 1.0);
                float rangeCheck = smoothstep(0.0, 1.0, rangeInput);

                if (depthDelta > bias)
                        occlusion += rangeCheck;
        }

        if (debugMode == 9)
        {
                float ratio = clamp(validSamples / float(samples), 0.0, 1.0);
                outColor = vec4(vec3(ratio), 1.0);
                return;
        }

        float ao = clamp(1.0 - occlusion / float(samples), 0.0, 1.0);
        ao = pow(ao, u_params0.z);
        ao = max(ao, u_params0.w);

        float viewZ = viewPos.x;
        float maxDistance = max(u_fogParams.x, 1.0);
        if (viewZ > maxDistance)
                ao = 1.0;

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
