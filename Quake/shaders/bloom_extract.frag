layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D MaskTexture;

layout(location=0) uniform vec4 ThresholdParams; // x: threshold, y: soft knee, z: mask enabled
layout(location=1) uniform vec4 DownsampleParams; // xy: source size, zw: scale from target to source

layout(location=0) out vec4 outColor;

float BloomThresholdFactor(float brightness, float threshold, float softKnee)
{
        float diff = max(brightness - threshold, 0.0);
        if (softKnee > 1e-4)
        {
                float soft = clamp(brightness - threshold + softKnee, 0.0, 2.0 * softKnee);
                soft = (soft * soft) / max(4.0 * softKnee, 1e-4);
                diff = max(diff, soft);
        }
        return (brightness > 0.0) ? (diff / brightness) : 0.0;
}

void main()
{
        float threshold = ThresholdParams.x;
        float softKnee = max(ThresholdParams.y, 0.0);
        float maskEnabled = ThresholdParams.z;
        vec2 sourceSize = DownsampleParams.xy;
        vec2 scale = DownsampleParams.zw;
        vec2 base = gl_FragCoord.xy * scale - 0.5;
        vec2 maxCoord = max(sourceSize - vec2(1.0), vec2(0.0));
        ivec2 baseCoord = ivec2(floor(base));
        vec3 accum = vec3(0.0);
        vec3 accum_bloom = vec3(0.0);
        vec3 accum_emissive = vec3(0.0);
        float weight = 0.0;
        float weight_bloom = 0.0;
        float weight_emissive = 0.0;
        for (int j = 0; j < 2; ++j)
        {
                for (int i = 0; i < 2; ++i)
                {
                        vec2 sampleCoord = clamp(vec2(baseCoord) + vec2(float(i), float(j)), vec2(0.0), maxCoord);
                        vec4 sampleColor = texelFetch(SceneTexture, ivec2(sampleCoord), 0);
                        float mask = 1.0;
                        if (maskEnabled > 0.5)
                        {
                                float rawMask = texelFetch(MaskTexture, ivec2(sampleCoord), 0).w;
                                int maskBits = int(floor(rawMask + 0.5));
                                bool bloomMask = (maskBits & 1) != 0;
                                bool emissiveMask = (maskBits & 4) != 0;
                                if (bloomMask)
                                {
                                        accum_bloom += sampleColor.rgb;
                                        weight_bloom += 1.0;
                                }
                                if (emissiveMask)
                                {
                                        accum_emissive += sampleColor.rgb;
                                        weight_emissive += 1.0;
                                }
                                mask = (bloomMask || emissiveMask) ? 1.0 : 0.0;
                        }
                        accum += sampleColor.rgb * mask;
                        weight += mask;
                }
        }
        vec3 color = (weight > 0.0) ? (accum / weight) : vec3(0.0);
        if (maskEnabled > 0.5)
        {
                vec3 emissiveColor = (weight_emissive > 0.0) ? (accum_emissive / weight_emissive) : vec3(0.0);
                vec3 bloomColor = (weight_bloom > 0.0) ? (accum_bloom / weight_bloom) : vec3(0.0);
                float brightness = dot(bloomColor, vec3(0.2126, 0.7152, 0.0722));
                float factor = BloomThresholdFactor(brightness, threshold, softKnee);
                bloomColor *= factor;
                color = emissiveColor + bloomColor;
        }
        else
        {
                float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
                float factor = BloomThresholdFactor(brightness, threshold, softKnee);
                color *= factor;
        }
        outColor = vec4(color, 1.0);
}
