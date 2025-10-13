layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4 LightShaftParams0; // x: intensity, y: decay, z: density, w: weight
layout(location=1) uniform vec4 LightShaftParams1; // x: near plane, y: far plane, z: reversed-Z flag, w: sample count (float)
layout(location=2) uniform vec4 LightShaftParams2; // x: light pos X, y: light pos Y, z: depth bias, w: occlusion range
layout(location=3) uniform vec4 ViewRect;          // xy: min, zw: max

layout(location=0) out vec4 outColor;

float LinearizeDepth(float rawDepth)
{
        float nearPlane = LightShaftParams1.x;
        float farPlane = LightShaftParams1.y;
        float reversed = LightShaftParams1.z;
        if (reversed > 0.5)
        {
                float denom = nearPlane + rawDepth * (farPlane - nearPlane);
                return (nearPlane * farPlane) / max(denom, 1e-6);
        }
        else
        {
                float ndc = rawDepth * 2.0 - 1.0;
                float denom = farPlane + nearPlane - ndc * (farPlane - nearPlane);
                return (2.0 * nearPlane * farPlane) / max(denom, 1e-6);
        }
}

void main()
{
        float intensity = max(LightShaftParams0.x, 0.0);
        if (intensity <= 0.0)
        {
                outColor = vec4(0.0);
                return;
        }

        vec2 texSize = vec2(textureSize(SceneTexture, 0));
        vec2 uv = (gl_FragCoord.xy + 0.5) / texSize;
        vec2 viewMin = ViewRect.xy;
        vec2 viewMax = ViewRect.zw;
        bool inView = all(greaterThanEqual(uv, viewMin)) && all(lessThanEqual(uv, viewMax));
        if (!inView)
        {
                outColor = vec4(0.0);
                return;
        }

        int samples = clamp(int(LightShaftParams1.w + 0.5), 1, 256);
        float decay = clamp(LightShaftParams0.y, 0.0, 1.0);
        float density = max(LightShaftParams0.z, 0.0);
        float weight = max(LightShaftParams0.w, 0.0);
        float depthBias = max(LightShaftParams2.z, 0.0);
        float occlusionRange = max(LightShaftParams2.w, 1e-4);
        vec2 lightPos = LightShaftParams2.xy;

        vec2 stepUV = (lightPos - uv) * (density / float(samples));
        vec2 sampleUV = uv;

        float currentDepth = LinearizeDepth(texture(DepthTexture, uv).r);
        if (!(currentDepth > 0.0))
                currentDepth = LightShaftParams1.y;

        float illuminationDecay = 1.0;
        float occlusion = 1.0;
        vec3 shafts = vec3(0.0);

        for (int i = 0; i < samples; ++i)
        {
                sampleUV += stepUV;
                if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0))))
                        break;

                float sampleDepth = LinearizeDepth(texture(DepthTexture, sampleUV).r);
                if (!(sampleDepth > 0.0))
                        sampleDepth = LightShaftParams1.y;

                float diff = currentDepth - sampleDepth - depthBias;
                float sampleOcclusion = clamp(1.0 - max(diff, 0.0) / occlusionRange, 0.0, 1.0);
                occlusion *= sampleOcclusion;

                vec3 sampleColor = texture(SceneTexture, sampleUV).rgb;
                sampleColor *= illuminationDecay * weight;
                shafts += sampleColor * occlusion;

                illuminationDecay *= decay;
        }

        outColor = vec4(shafts * intensity, 1.0);
}