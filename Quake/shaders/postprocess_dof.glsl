#ifndef POSTPROCESS_DOF_GLSL
#define POSTPROCESS_DOF_GLSL

void ApplyDepthOfField(inout vec3 color, vec2 uv, vec2 invTexSize, bool inView,
        bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        if (!(DoFParams0.x > 0.5 && inView && depthInfo.valid && viewModelMask < 0.5 && centerOpaque))
                return;
        
        float linearDepth = SampleLinearDepth(gl_FragCoord.xy, depthInfo);
        float focusDistance = DoFParams0.y;
        float focusRange = max(DoFParams0.z, 0.0001);
        float maxBlur = max(DoFParams0.w, 0.0);
        
        // Verbesserte CoC-Berechnung mit smoothem Übergang
        float coc = abs(linearDepth - focusDistance);
        float blurFactor = smoothstep(focusRange, focusRange * 2.0, coc);
        float blurRadius = blurFactor * maxBlur;
        
        if (blurRadius <= 0.0001)
                return;
        
        // Erweitertes Poisson-Disk Sampling für bessere Qualität
        const vec2 kernel[16] = vec2[](
                vec2(0.0, 0.0),
                vec2(0.54545456, 0.0),
                vec2(0.16855472, 0.5187581),
                vec2(-0.44128203, 0.3206101),
                vec2(-0.44128197, -0.3206102),
                vec2(0.1685548, -0.5187581),
                vec2(1.0, 0.0),
                vec2(0.809017, 0.58778524),
                vec2(0.30901697, 0.95105654),
                vec2(-0.30901703, 0.9510565),
                vec2(-0.80901706, 0.5877852),
                vec2(-1.0, 0.0),
                vec2(-0.80901694, -0.58778536),
                vec2(-0.30901664, -0.9510566),
                vec2(0.30901712, -0.9510565),
                vec2(0.80901694, -0.5877853)
        );
        
        // Rotation für besseres Dithering
        float noise = SCREEN_SPACE_NOISE();
        float angle = noise * 6.28318530718;
        float sine = sin(angle);
        float cosine = cos(angle);
        mat2 rotation = mat2(cosine, -sine, sine, cosine);
        
        vec3 accum = vec3(0.0);
        float weight = 0.0;
        
        // Verbessertes Sampling mit Tiefengewichtung
        for (int i = 0; i < 16; ++i)
        {
                float radius = length(kernel[i]);
                vec2 offset = rotation * kernel[i] * blurRadius * invTexSize;
                vec4 sampleColor = texture(GammaTexture, uv + offset);
                
                if (sampleColor.a < OPAQUE_ALPHA_THRESHOLD)
                        continue;
                
                // Gauß-ähnliche Gewichtung für weichere Übergänge
                float w = exp(-radius * radius * 2.0);
                accum += sampleColor.rgb * w;
                weight += w;
        }
        
        // Fallback auf Original wenn keine Samples
        if (weight > 0.0001)
                color = accum / weight;
}

#endif // POSTPROCESS_DOF_GLSL