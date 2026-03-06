void ApplyDepthOfField(inout vec4 color, vec2 uv, vec2 invTexSize, bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        if (!(DoFParams0.x > 0.5 && inView && depthInfo.valid && viewModelMask < 0.5 && centerOpaque))
                return;

        float linearDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
        float focusDistance = DoFParams0.y;
        float focusRange = max(DoFParams0.z, 0.0001);
        float maxBlur = max(DoFParams0.w, 0.0);
        float coc = abs(linearDepth - focusDistance);
        float blurFactor = clamp((coc - focusRange) / focusRange, 0.0, 1.0);
        // Scale blur by fog transmittance: dense fog obscures depth cues so DoF
        // should have no effect on fully fogged pixels.  FogVolTransmittance()
        // returns 1.0 when fogvol is inactive so the line is a no-op in that case.
        float blurRadius = blurFactor * maxBlur * FogVolTransmittance(uv);
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
                if (sampleColor.a < OPAQUE_ALPHA_THRESHOLD)
                        continue;
                accum += sampleColor.rgb;
                weight += 1.0;
        }
        color.rgb = accum / weight;
}
