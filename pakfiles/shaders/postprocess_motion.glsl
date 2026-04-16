void AccumulateMotionSample(inout vec3 accum, inout float weight, vec2 sampleUV, vec2 sampleCoordPx, vec2 viewMin, vec2 viewMax, DepthSamplingInfo info, bool useDepth, float centerDepth, float depthThresholdRatio, float tapWeight)
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
        accum += sampleColor.rgb * tapWeight;
        weight += tapWeight;
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
        if (maxRadius <= 0.0)
                maxRadius = speed;
        float radius = clamp(speed, 0.0, maxRadius);
        /* Keep tiny camera drift from producing animated shimmer. */
        float stableMinVelocity = max(minVelocity, 1.0);
        if (!(radius > stableMinVelocity))
                return;

        vec2 direction = speed > 1e-4 ? (velocityPx / speed) : vec2(0.0);
        bool useDepth = MotionParams0.w > 0.0 && depthInfo.valid;
        float centerDepth = 0.0;
        float depthThresholdRatio = max(MotionParams0.w, 0.0);
        if (useDepth)
                centerDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);

        /* Simple deterministic 5-tap blur: center + 2 symmetric pairs. */
        vec3 accum = color.rgb * 0.50;
        float weight = 0.50;
        vec2 nearOffsetPx = direction * (radius * 0.5);
        vec2 farOffsetPx = direction * radius;
        vec2 nearOffsetUV = nearOffsetPx * invTexSize;
        vec2 farOffsetUV = farOffsetPx * invTexSize;

        AccumulateMotionSample(accum, weight, uv + nearOffsetUV, gl_FragCoord.xy + nearOffsetPx, viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio, 0.20);
        AccumulateMotionSample(accum, weight, uv - nearOffsetUV, gl_FragCoord.xy - nearOffsetPx, viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio, 0.20);
        AccumulateMotionSample(accum, weight, uv + farOffsetUV, gl_FragCoord.xy + farOffsetPx, viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio, 0.05);
        AccumulateMotionSample(accum, weight, uv - farOffsetUV, gl_FragCoord.xy - farOffsetPx, viewMin, viewMax, depthInfo, useDepth, centerDepth, depthThresholdRatio, 0.05);

        if (weight <= 0.0)
                return;

        vec3 blurred = accum / weight;
        float blurBlend = clamp((radius - stableMinVelocity) / max(maxRadius - stableMinVelocity, 1e-3), 0.0, 1.0);
        color.rgb = mix(color.rgb, blurred, blurBlend * 0.85);
}
