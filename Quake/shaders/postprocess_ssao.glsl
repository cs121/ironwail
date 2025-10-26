#ifndef POSTPROCESS_SSAO_GLSL
#define POSTPROCESS_SSAO_GLSL

const int SSAO_MAX_SAMPLES = 64;
const float SSAO_EPSILON = 1e-6;

vec3 GetHemisphereSample(int index, int totalSamples)
{
        float i = float(index);
        float n = max(float(totalSamples), 1.0);
        float angle = 6.28318530718 * fract(i * 0.61803398875);
        float radius = sqrt((i + 0.5) / n);
        float z = sqrt(max(0.0, 1.0 - radius * radius));
        vec3 sample = vec3(cos(angle) * radius, sin(angle) * radius, z);
        float scale = (i + 0.5) / n;
        scale = mix(0.1, 1.0, scale * scale);
        return sample * scale;
}

vec2 NormalizeViewUV(vec2 uv, vec2 viewMin, vec2 viewMax)
{
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        return clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
}

vec2 DenormalizeViewUV(vec2 viewUV, vec2 viewMin, vec2 viewMax)
{
        return viewMin + viewUV * (viewMax - viewMin);
}

bool ReconstructViewPosition(vec2 fragPx, vec2 uv, vec2 viewMin, vec2 viewMax,
        DepthSamplingInfo info, out vec3 position, out float linearDepth)
{
        float rawDepth;
        if (!SampleDepth(fragPx, info, rawDepth, linearDepth))
                return false;

        vec2 viewUV = NormalizeViewUV(uv, viewMin, viewMax);
        float ndcZ = (DoFParams1.z > 0.5) ? rawDepth : rawDepth * 2.0 - 1.0;
        vec4 clipPos = vec4(viewUV * 2.0 - 1.0, ndcZ, 1.0);
        vec4 viewPos4 = InverseProjectionMatrix * clipPos;
        if (abs(viewPos4.w) < SSAO_EPSILON)
                return false;

        position = viewPos4.xyz / viewPos4.w;
        if (viewPos4.w <= 0.0)
                return false;
        return true;
}

vec3 ReconstructNormal(vec2 fragPx, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax,
        DepthSamplingInfo info, vec3 centerPos)
{
        vec2 offsets[4] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
        vec3 samples[4] = vec3[](centerPos, centerPos, centerPos, centerPos);
        for (int i = 0; i < 4; ++i)
        {
                vec2 sampleFrag = fragPx + offsets[i];
                vec2 sampleUV = uv + offsets[i] * invTexSize;
                vec3 samplePos;
                float sampleDepth;
                if (ReconstructViewPosition(sampleFrag, sampleUV, viewMin, viewMax, info, samplePos, sampleDepth))
                        samples[i] = samplePos;
        }

        vec3 dx = samples[1] - samples[0];
        vec3 dy = samples[3] - samples[2];
        vec3 normal = normalize(cross(dx, dy));
        if (!all(isfinite(normal)) || length(normal) < SSAO_EPSILON)
                normal = vec3(0.0, 0.0, -1.0);
        else if (dot(normal, vec3(0.0, 0.0, -1.0)) < 0.0)
                normal = -normal;
        return normal;
}

vec3 GetNoiseVector(vec2 fragCoord)
{
        float seed = whitenoise01(fragCoord * 0.25);
        float angle = seed * 6.28318530718;
        vec2 rand = vec2(cos(angle), sin(angle));
        return normalize(vec3(rand, 0.0));
}

void ApplySSAO(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax,
        bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        if (!(SSAOParams0.x > 0.5 && inView && depthInfo.valid && centerOpaque && viewModelMask < 0.5))
                return;

        float radius = max(SSAOParams0.y, 0.0) * 0.01;
        float bias = max(SSAOParams0.z, 0.0);
        float magnitude = max(SSAOParams0.w, 1.0);
        int sampleCount = clamp(int(SSAOParams1.x + 0.5), 1, SSAO_MAX_SAMPLES);
        float contrast = max(SSAOParams1.y, 1.0);

        if (!(radius > 0.0 && sampleCount > 0))
                return;

        vec2 fragCoord = gl_FragCoord.xy;
        vec3 centerPos;
        float centerDepth;
        if (!ReconstructViewPosition(fragCoord, uv, viewMin, viewMax, depthInfo, centerPos, centerDepth))
                return;

        vec3 normal = ReconstructNormal(fragCoord, uv, invTexSize, viewMin, viewMax, depthInfo, centerPos);
        vec3 randomVec = GetNoiseVector(fragCoord);
        float ndotRandom = dot(randomVec, normal);
        if (abs(ndotRandom) > 0.999)
                randomVec = normalize(vec3(normal.y, normal.z, normal.x));
        vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
        vec3 bitangent = normalize(cross(normal, tangent));
        mat3 tbn = mat3(tangent, bitangent, normal);

        float occlusionAccum = 0.0;

        for (int i = 0; i < SSAO_MAX_SAMPLES; ++i)
        {
                if (i >= sampleCount)
                        break;

                vec3 sampleVec = GetHemisphereSample(i, sampleCount);
                vec3 samplePos = centerPos + tbn * sampleVec * radius;

                vec4 clip = ProjectionMatrix * vec4(samplePos, 1.0);
                if (clip.w <= SSAO_EPSILON)
                        continue;
                vec3 ndc = clip.xyz / clip.w;
                vec2 viewUV = ndc.xy * 0.5 + 0.5;
                if (any(lessThan(viewUV, vec2(0.0))) || any(greaterThan(viewUV, vec2(1.0))))
                        continue;

                vec2 sampleUV = DenormalizeViewUV(viewUV, viewMin, viewMax);
                vec2 sampleFrag = sampleUV / invTexSize;

                vec3 fetchedPos;
                float fetchedDepth;
                if (!ReconstructViewPosition(sampleFrag, sampleUV, viewMin, viewMax, depthInfo, fetchedPos, fetchedDepth))
                        continue;

                vec3 dir = fetchedPos - centerPos;
                float dist = length(dir);
                if (dist < SSAO_EPSILON)
                        continue;
                float ndotDir = dot(normal, dir / dist);
                if (ndotDir <= 0.0)
                        continue;

                float expectedDepth = -samplePos.z;
                float actualDepth = fetchedDepth;
                float depthDelta = expectedDepth - actualDepth - bias;
                if (depthDelta <= 0.0)
                        continue;

                float rangeCheck = smoothstep(0.0, 1.0, radius / (abs(centerDepth - actualDepth) + 1e-4));
                occlusionAccum += rangeCheck;
        }

        float occlusion = 1.0 - (occlusionAccum / float(sampleCount));
        occlusion = pow(clamp(occlusion, 0.0, 1.0), magnitude);
        occlusion = contrast * (occlusion - 0.5) + 0.5;
        occlusion = clamp(occlusion, 0.0, 1.0);

        color *= occlusion;
}

#endif // POSTPROCESS_SSAO_GLSL
