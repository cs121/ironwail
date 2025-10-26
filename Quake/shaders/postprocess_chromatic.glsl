#ifndef POSTPROCESS_CHROMATIC_GLSL
#define POSTPROCESS_CHROMATIC_GLSL

void ApplyChromaticAberration(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax)
{
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 viewUV = clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
        float chromaticAmount = max(PostFXParams2.y, 0.0);
        if (chromaticAmount <= 0.0)
                return;

        vec2 dir = viewUV - vec2(0.5);
        float lenDir = length(dir);
        if (lenDir <= 1e-4)
                return;

        vec2 normDir = dir / lenDir;
        vec2 offsetPx = normDir * (chromaticAmount * lenDir);
        vec2 offsetUV = offsetPx * invTexSize;
        vec2 uvR = clamp(uv + offsetUV, viewMin, viewMax);
        vec2 uvB = clamp(uv - offsetUV, viewMin, viewMax);
        vec3 aberrated = color;
        aberrated.r = texture(GammaTexture, uvR).r;
        aberrated.b = texture(GammaTexture, uvB).b;
        float blend = clamp(chromaticAmount * lenDir, 0.0, 1.0);
        color = mix(color, aberrated, blend);
}

#endif // POSTPROCESS_CHROMATIC_GLSL
