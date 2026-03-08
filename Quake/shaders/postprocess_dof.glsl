void ApplyDepthOfField(inout vec4 color, vec2 uv, vec2 invTexSize, bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        // Do not gate DoF on color alpha: fogvol/medium passes may repurpose alpha
        // while depth remains valid for DoF classification.
        if (!(DoFParams0.x > 0.5 && inView && depthInfo.valid && viewModelMask < 0.5))
                return;

        float linearDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
        float focusDistance = DoFParams0.y;
        float focusRange = max(DoFParams0.z, 0.0001);
        float maxBlur = max(DoFParams0.w, 0.0);
        float coc = abs(linearDepth - focusDistance);
        float blurFactor = clamp((coc - focusRange) / focusRange, 0.0, 1.0);
        float blurRadius = blurFactor * maxBlur;
        if (blurRadius <= 0.0001)
                return;

        const vec2 kernel[8] = vec2[](
                vec2(1.0, 0.0),
                vec2(-1.0, 0.0),
                vec2(0.0, 1.0),
                vec2(0.0, -1.0),
                vec2(0.70710678, 0.70710678),
                vec2(-0.70710678, 0.70710678),
                vec2(0.70710678, -0.70710678),
                vec2(-0.70710678, -0.70710678)
        );
        float noise = SCREEN_SPACE_NOISE();
        float angle = noise * 6.28318530718;
        float sine = sin(angle);
        float cosine = cos(angle);
        mat2 rotation = mat2(cosine, -sine, sine, cosine);
        vec3 accum = color.rgb;
        float weight = 1.0;
        for (int i = 0; i < 8; ++i)
        {
                vec2 offset = rotation * kernel[i] * blurRadius * invTexSize;
                vec4 sampleColor = texture(GammaTexture, uv + offset);
                accum += sampleColor.rgb;
                weight += 1.0;
        }
        color.rgb = accum / weight;
}
