#ifndef POSTPROCESS_VIGNETTE_GLSL
#define POSTPROCESS_VIGNETTE_GLSL

void ApplyVignette(inout vec3 color, vec2 uv, vec2 viewMin, vec2 viewMax, vec2 texSize)
{
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
        float aspect = texSize.y > 0.0 ? texSize.x / texSize.y : 1.0;

        float vignetteStrength = clamp(PostFXParams0.x, 0.0, 1.0);
        float vignetteInner = max(PostFXParams0.y, 0.0);
        float vignetteOuter = max(PostFXParams0.z, vignetteInner + 1e-3);
        float vignetteFalloff = max(PostFXParams0.w, 1e-3);
        vec3 vignetteColor = clamp(PostFXParams1.xyz, vec3(0.0), vec3(1.0));
        int vignetteBlendMode = clamp(int(PostFXParams1.w + 0.5), 0, 2);
        float vignetteNoise = clamp(PostFXParams2.x, 0.0, 0.1);
        if (!(vignetteStrength > 0.0 && vignetteOuter > vignetteInner))
                return;

        vec2 vignetteCoord = viewUV * 2.0 - vec2(1.0);
        vignetteCoord.x *= aspect;
        float dist = length(vignetteCoord);
        float range = max(vignetteOuter - vignetteInner, 1e-3);
        float fade = clamp((dist - vignetteInner) / range, 0.0, 1.0);
        if (vignetteNoise > 0.0)
        {
                float noise = whitenoise(gl_FragCoord.xy);
                fade = clamp(fade + noise * vignetteNoise * fade, 0.0, 1.0);
        }
        float vignette = pow(fade, vignetteFalloff);
        float intensity = min(vignette * vignetteStrength, 1.0);
        if (intensity <= 0.0)
                return;

        if (vignetteBlendMode == 1)
        {
                vec3 overlayColor = vignetteColor;
                vec3 overlayDark = 2.0 * color * overlayColor;
                vec3 overlayLight = 1.0 - 2.0 * (1.0 - color) * (1.0 - overlayColor);
                vec3 overlayResult = mix(overlayDark, overlayLight, step(0.5, color));
                overlayResult = clamp(overlayResult, vec3(0.0), vec3(1.0));
                color = mix(color, overlayResult, intensity);
        }
        else if (vignetteBlendMode == 2)
        {
                color = clamp(color + vignetteColor * intensity, vec3(0.0), vec3(1.0));
        }
        else
        {
                vec3 multiplier = mix(vec3(1.0), vignetteColor, intensity);
                color *= multiplier;
        }
}

#endif // POSTPROCESS_VIGNETTE_GLSL
