#ifndef POSTPROCESS_GODRAYS_GLSL
#define POSTPROCESS_GODRAYS_GLSL

const int GODRAY_MAX_SAMPLES = 128;

void ApplyGodRays(inout vec3 color, vec2 uv, bool inView, vec2 viewMin, vec2 viewMax)
{
        if (!(GodRayParams0.x > 0.5 && inView))
                return;

        float intensity = max(GodRayParams0.y, 0.0);
        if (intensity <= 1e-5)
                return;

        float decay = clamp(GodRayParams0.z, 0.0, 1.0);
        float weight = max(GodRayParams0.w, 0.0);
        float density = max(GodRayParams1.w, 0.0);
        if (density <= 0.0 || weight <= 0.0)
                return;

        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 lightUV = clamp(GodRayParams1.xy, vec2(0.0), vec2(1.0));
        lightUV = viewMin + lightUV * viewSize;

        vec2 delta = lightUV - uv;
        float deltaMag = max(abs(delta.x), abs(delta.y));
        if (deltaMag <= 1e-6)
                return;

        int samples = int(clamp(GodRayParams2.x + 0.5, 1.0, float(GODRAY_MAX_SAMPLES)));
        float threshold = clamp(GodRayParams1.z, 0.0, 1.0);
        float softness = clamp(GodRayParams2.y, 1e-4, 1.0);

        vec2 stepUV = delta / float(samples) * density;
        vec2 sampleUV = uv;
        float illuminationDecay = 1.0;
        vec3 scatterAccum = vec3(0.0);

        for (int i = 0; i < GODRAY_MAX_SAMPLES; ++i)
        {
                if (i >= samples)
                        break;

                sampleUV += stepUV;
                if (any(lessThan(sampleUV, viewMin)) || any(greaterThan(sampleUV, viewMax)))
                        break;

                vec3 sampleColor = texture(GammaTexture, sampleUV).rgb;
                float luminance = dot(sampleColor, vec3(0.2126, 0.7152, 0.0722));
                float visibility = smoothstep(threshold, min(threshold + softness, 1.0), luminance);

                scatterAccum += vec3(visibility) * illuminationDecay * weight;
                illuminationDecay *= decay;
        }

        color += scatterAccum * intensity;
}

#endif // POSTPROCESS_GODRAYS_GLSL
