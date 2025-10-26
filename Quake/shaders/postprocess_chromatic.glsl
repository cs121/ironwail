#ifndef POSTPROCESS_CHROMATIC_GLSL
#define POSTPROCESS_CHROMATIC_GLSL

void ApplyChromaticAberration(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax)
{
    // Early exit wenn chromatic aberration deaktiviert ist
    float chromaticAmount = PostFXParams2.y;
    if (chromaticAmount <= 0.0)
        return;
    
    // View-Space Berechnungen
    vec2 viewSize = viewMax - viewMin;
    viewSize = max(viewSize, vec2(1e-6));
    vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
    
    // Richtung vom Zentrum mit radialem Falloff
    vec2 dir = viewUV - vec2(0.5);
    float lenDir = length(dir);
    
    // Early exit für Zentrum
    if (lenDir <= 1e-4)
        return;
    
    // Normalisierung
    vec2 normDir = dir / lenDir;
    
    // Verbesserter radialer Falloff (quadratisch für stärkere Randbetonung)
    float radialFalloff = lenDir * lenDir;
    
    // Barrel distortion für realistischeren Linsen-Effekt
    float barrelDistortion = 1.0 + 0.15 * radialFalloff;
    
    // Separate Offsets für jeden Farbkanal (3-Kanal statt 2-Kanal)
    float baseOffset = chromaticAmount * radialFalloff * barrelDistortion;
    vec2 offsetR = normDir * baseOffset * 1.2 * invTexSize;  // Rot am weitesten außen
    vec2 offsetG = normDir * baseOffset * 0.5 * invTexSize;  // Grün leicht versetzt
    vec2 offsetB = normDir * baseOffset * -1.0 * invTexSize; // Blau nach innen
    
    // UV-Koordinaten für alle drei Kanäle
    vec2 uvR = clamp(uv + offsetR, viewMin, viewMax);
    vec2 uvG = clamp(uv + offsetG, viewMin, viewMax);
    vec2 uvB = clamp(uv + offsetB, viewMin, viewMax);
    
    // Multi-Sample für weichere Übergänge (Optional: kann auskommentiert werden für Performance)
    vec3 aberrated;
    aberrated.r = texture(GammaTexture, uvR).r;
    aberrated.g = texture(GammaTexture, uvG).g;
    aberrated.b = texture(GammaTexture, uvB).b;
    
    // Verbesserter Blend mit smoothstep für natürlicheren Übergang
    float blendCurve = smoothstep(0.0, 1.0, radialFalloff);
    float blend = clamp(chromaticAmount * blendCurve, 0.0, 1.0);
    
    // Vignette-ähnlicher Effekt an den Rändern verstärken
    float edgeIntensity = smoothstep(0.3, 1.0, radialFalloff);
    blend = mix(blend, 1.0, edgeIntensity * 0.3);
    
    // Final color mix mit verbesserten Gewichtungen
    color = mix(color, aberrated, blend);
}

#endif // POSTPROCESS_CHROMATIC_GLSL