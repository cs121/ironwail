#ifndef POSTPROCESS_LENS_GLSL
#define POSTPROCESS_LENS_GLSL

// Simulates optical characteristics of a ZEISS Planar 0.7/50mm lens.
// Adds gentle barrel distortion, subtle spherical aberration and edge halos.
void ApplyPlanarLens(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax)
{
        float intensity = clamp(LensParams0.x, 0.0, 1.0);
        if (intensity <= 1e-4)
                return;

        float distortionCoeff = max(LensParams0.y, 0.0);
        float haloStrength = max(LensParams0.z, 0.0);
        float tintStrength = max(LensParams0.w, 0.0);

        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
        vec2 centered = viewUV - vec2(0.5);

        float aspect = viewSize.y > 1e-6 ? viewSize.x / viewSize.y : 1.0;
        vec2 elliptical = vec2(centered.x * aspect, centered.y);
        float radius = length(elliptical);
        float radius2 = radius * radius;
        float radius4 = radius2 * radius2;

        float curvature = distortionCoeff * (radius2 + 0.5 * radius4);
        vec2 distortion = centered * curvature * intensity;
        vec2 distortedUV = clamp(uv + distortion, viewMin, viewMax);
        vec3 distortedColor = texture(GammaTexture, distortedUV).rgb;

        vec2 aberrationDir = centered * (0.6 + 0.4 * radius2);
        vec3 aberrationColor;
        aberrationColor.r = texture(GammaTexture, clamp(uv + aberrationDir * 0.75 * intensity + invTexSize * 0.5, viewMin, viewMax)).r;
        aberrationColor.g = texture(GammaTexture, clamp(uv + aberrationDir * 0.55 * intensity, viewMin, viewMax)).g;
        aberrationColor.b = texture(GammaTexture, clamp(uv - aberrationDir * 0.65 * intensity, viewMin, viewMax)).b;

        float halo = haloStrength * smoothstep(0.35, 1.05, radius);
        vec2 haloOffset = -centered * (0.25 + 0.5 * radius2);
        vec3 haloColor = texture(GammaTexture, clamp(uv + haloOffset * intensity, viewMin, viewMax)).rgb;

        vec3 result = mix(color, distortedColor, intensity * 0.6);
        result = mix(result, aberrationColor, intensity * 0.35);
        result += haloColor * halo * intensity * 0.2;

        vec3 tint = vec3(1.02, 1.0, 0.985);
        result = mix(result, result * tint, tintStrength * intensity);

        color = clamp(result, 0.0, 1.0);
}

#endif // POSTPROCESS_LENS_GLSL
