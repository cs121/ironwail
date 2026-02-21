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

void ApplyMotionBlur(inout vec4 color, vec2 uv, vec2 viewMin, vec2 viewMax, vec2 texSize, vec2 invTexSize, bool inView, bool centerOpaque, bool hasVelocityTexture, vec2 velocity, DepthSamplingInfo depthInfo, float viewModelMask)
{
        if (!(MotionParams0.x > 0.5 && inView && hasVelocityTexture && viewModelMask < 0.5 && centerOpaque))
                return;

        float effectiveShutter = MotionParams0.y;
        if (effectiveShutter <= 0.0)
                return;

        vec2 velocityPx = velocity * effectiveShutter * texSize;
        float speed = length(velocityPx);
        float minVelocity = max(MotionParams0.z, 0.0);
        float maxRadius = MotionParams1.x;
        int maxSamples = int(MotionParams1.y + 0.5);
        maxSamples = clamp(maxSamples, 1, MOTION_MAX_SAMPLES);
        if (maxRadius <= 0.0)
                maxRadius = speed;
        float radius = clamp(speed, 0.0, maxRadius);
        if (!(radius > minVelocity && maxSamples > 0))
                return;

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
        if (weight > 1.0)
                color.rgb = accum / weight;
}
