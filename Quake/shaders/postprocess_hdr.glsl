#ifndef POSTPROCESS_HDR_GLSL
#define POSTPROCESS_HDR_GLSL

vec4 ApplyHDRAndFilmGrain(vec3 hdrColor, vec2 uv, float contrast, float gamma)
{
        float bloomIntensity = HDRParams.x;
        vec3 bloomColor = vec3(0.0);
        if (bloomIntensity > 0.0)
                bloomColor = texture(BloomTexture, uv).rgb * bloomIntensity;

        float exposure = max(HDRParams.y, 0.0);
        float tonemapMode = HDRParams.z;
        vec3 combined = (hdrColor + bloomColor) * exposure * contrast;
        combined = max(combined, vec3(0.0));

        vec3 mapped;
        if (tonemapMode > 0.5)
        {
                if (tonemapMode < 1.5)
                        mapped = UchimuraTonemap(combined);
                else
                        mapped = LottesTonemap(combined);
        }
        else
        {
                mapped = clamp(combined, 0.0, 1.0);
        }
        mapped = clamp(mapped, 0.0, 1.0);

        float filmGrainIntensity = max(FilmGrainParams.x, 0.0);
        if (filmGrainIntensity > 0.0)
        {
                float grainSize = max(FilmGrainParams.y, 1.0);
                float filmGrainStrength = max(FilmGrainParams.z, 0.0);
                vec2 grainOffset = FilmGrainOffset.xy;
                vec2 grainCoord = (gl_FragCoord.xy + grainOffset) / grainSize;
                float grain = tri(whitenoise01(grainCoord));
                float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
                float response = mix(1.0, clamp(1.0 - luma, 0.0, 1.0), 0.5);
                mapped = clamp(mapped + grain * (filmGrainIntensity * filmGrainStrength * response), 0.0, 1.0);
        }

        return vec4(pow(mapped, vec3(gamma)), 1.0);
}

#endif // POSTPROCESS_HDR_GLSL
