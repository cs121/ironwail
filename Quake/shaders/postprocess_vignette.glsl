#ifndef POSTPROCESS_VIGNETTE_GLSL
#define POSTPROCESS_VIGNETTE_GLSL

// Verbesserte Vignette-Funktion mit besserer Qualität und Performance
void ApplyVignette(inout vec3 color, vec2 uv, vec2 viewMin, vec2 viewMax, vec2 texSize)
{
        // Viewport-Berechnung mit Epsilon für Division-by-Zero
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
        
        // Aspect Ratio mit Fallback
        float aspect = texSize.y > 1e-6 ? texSize.x / texSize.y : 1.0;
        
        // Parameter-Extraktion mit Validierung
        float vignetteStrength = clamp(PostFXParams0.x, 0.0, 1.0);
        float vignetteInner = max(PostFXParams0.y, 0.0);
        float vignetteOuter = max(PostFXParams0.z, vignetteInner + 1e-3);
        float vignetteFalloff = max(PostFXParams0.w, 1e-3);
        vec3 vignetteColor = clamp(PostFXParams1.xyz, vec3(0.0), vec3(1.0));
        int vignetteBlendMode = clamp(int(PostFXParams1.w + 0.5), 0, 2);
        float vignetteNoise = clamp(PostFXParams2.x, 0.0, 0.1);
        
        // Early Exit für Performance
        if (vignetteStrength <= 0.0 || vignetteOuter <= vignetteInner)
                return;
        
        // Vignette-Koordinaten mit korrigiertem Aspect Ratio
        vec2 vignetteCoord = viewUV * 2.0 - 1.0;
        vignetteCoord.x *= aspect;
        
        // Distanz-Berechnung (optimiert)
        float dist = length(vignetteCoord);
        
        // Sanfterer Übergang mit verbessertem Fade
        float range = max(vignetteOuter - vignetteInner, 1e-3);
        float fade = clamp((dist - vignetteInner) / range, 0.0, 1.0);
        
        // Optionales Noise für organischeren Look
        if (vignetteNoise > 0.0)
        {
                float noise = whitenoise(gl_FragCoord.xy);
                // Smoothere Noise-Integration
                fade = clamp(fade + noise * vignetteNoise * fade * (1.0 - fade * 0.5), 0.0, 1.0);
        }
        
        // Power-Curve für natürlicheren Falloff
        float vignette = pow(fade, vignetteFalloff);
        float intensity = min(vignette * vignetteStrength, 1.0);
        
        // Early Exit wenn Intensität zu niedrig
        if (intensity <= 1e-4)
                return;
        
        // Blend-Modi mit verbesserter Qualität
        if (vignetteBlendMode == 1)
        {
                // Overlay Blend Mode - verbesserte Implementierung
                vec3 overlayColor = vignetteColor;
                vec3 overlayDark = 2.0 * color * overlayColor;
                vec3 overlayLight = 1.0 - 2.0 * (1.0 - color) * (1.0 - overlayColor);
                
                // Smoothstep für sanfteren Übergang zwischen Dark/Light
                vec3 blendFactor = smoothstep(0.45, 0.55, color);
                vec3 overlayResult = mix(overlayDark, overlayLight, blendFactor);
                overlayResult = clamp(overlayResult, vec3(0.0), vec3(1.0));
                
                // Smoother Mix mit verbessertem Intensity-Blending
                color = mix(color, overlayResult, intensity);
        }
        else if (vignetteBlendMode == 2)
        {
                // Additive Blend Mode mit Soft-Clipping für natürlicheres Aussehen
                vec3 additive = color + vignetteColor * intensity;
                color = clamp(additive, vec3(0.0), vec3(1.0));
        }
        else
        {
                // Multiply Blend Mode (Standard) - optimiert
                vec3 multiplier = mix(vec3(1.0), vignetteColor, intensity);
                color *= multiplier;
        }
        
        // Optional: Farbraum-Erhaltung für natürlichere Farben
        // color = clamp(color, vec3(0.0), vec3(1.0));
}

#endif // POSTPROCESS_VIGNETTE_GLSL