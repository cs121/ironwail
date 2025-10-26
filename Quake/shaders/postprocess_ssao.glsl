#ifndef POSTPROCESS_SSAO_GLSL
#define POSTPROCESS_SSAO_GLSL

const int SSAO_MAX_SAMPLES = 64;

void ApplySSAO(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax,
        bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        if (!(SSAOParams0.x > 0.5 && inView && depthInfo.valid && centerOpaque && viewModelMask < 0.5))
                return;

        float radiusPx = max(SSAOParams0.y, 0.0);
        float bias = max(SSAOParams0.z, 0.0);
        float intensity = max(SSAOParams0.w, 0.0);
        int sampleCount = clamp(int(SSAOParams1.x + 0.5), 1, SSAO_MAX_SAMPLES);
        float power = max(SSAOParams1.y, 0.0);
        if (!(radiusPx > 0.5 && intensity > 0.0 && sampleCount > 0))
                return;

        float centerDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
        if (centerDepth <= 0.0)
                return;

        float goldenAngle = 2.39996322972865332;
        float rotationNoise = SCREEN_SPACE_NOISE();
        float occlusionAccum = 0.0;
        float weightAccum = 0.0;
        for (int i = 0; i < SSAO_MAX_SAMPLES; ++i)
        {
                if (i >= sampleCount)
                        break;
                float fi = float(i);
                float t = (fi + 0.5) / float(sampleCount);
                float angle = (fi + rotationNoise) * goldenAngle;
                float radius = radiusPx * t;
                vec2 offset = vec2(cos(angle), sin(angle)) * radius;
                vec2 sampleUV = uv + offset * invTexSize;
                if (!all(greaterThanEqual(sampleUV, viewMin)) || !all(lessThanEqual(sampleUV, viewMax)))
                        continue;
                float sampleDepth = SampleLinearDepth(gl_FragCoord.xy + offset, depthInfo);
                if (sampleDepth <= 0.0)
                        continue;
                float depthDelta = centerDepth - sampleDepth;
                if (depthDelta <= bias)
                        continue;
                float rangeWeight = 1.0 - t;
                float occlusionSample = clamp((depthDelta - bias) / (depthDelta + centerDepth + 1e-4), 0.0, 1.0);
                occlusionAccum += occlusionSample * rangeWeight;
                weightAccum += rangeWeight;
        }
        if (weightAccum <= 0.0)
                return;

        float occlusion = occlusionAccum / weightAccum;
        float ao = clamp(1.0 - occlusion * intensity, 0.0, 1.0);
        if (power > 0.0)
                ao = pow(ao, power);
        color *= ao;
}

#endif // POSTPROCESS_SSAO_GLSL
