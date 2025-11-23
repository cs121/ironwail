layout(binding=0) uniform sampler2D SceneTexture;

layout(location=0) uniform vec4 ThresholdParams; // x: threshold
layout(location=1) uniform vec4 DownsampleParams; // xy: source size, zw: scale from target to source

layout(location=0) out vec4 outColor;

void main()
{
        float threshold = ThresholdParams.x;
        vec2 sourceSize = DownsampleParams.xy;
        vec2 scale = DownsampleParams.zw;
        vec2 base = (gl_FragCoord.xy + 0.5) * scale - 0.5;
        vec2 maxCoord = max(sourceSize - vec2(1.0), vec2(0.0));
        ivec2 baseCoord = ivec2(floor(base));
        vec3 accum = vec3(0.0);
        for (int j = 0; j < 2; ++j)
        {
                for (int i = 0; i < 2; ++i)
                {
                        vec2 sampleCoord = clamp(vec2(baseCoord) + vec2(float(i), float(j)), vec2(0.0), maxCoord);
                        accum += texelFetch(SceneTexture, ivec2(sampleCoord), 0).rgb;
                }
        }
        vec3 color = accum * 0.25;
        float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float diff = max(brightness - threshold, 0.0);
        float factor = brightness > 0.0 ? diff / brightness : 0.0;
        color *= factor;
        outColor = vec4(color, 1.0);
}
