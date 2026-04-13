layout(binding=0) uniform sampler2D BloomTexture;

// xy: 1.0 / bloomRT size  (NOT 1.0 / viewport size!)
// zw: blur direction (e.g. (1,0) horizontal, (0,1) vertical)
layout(location=0) uniform vec4 BlurParams;

layout(location=0) out vec4 outColor;

void main()
{
        vec2 texelSize = BlurParams.xy;
        vec2 direction = BlurParams.zw;

        // BUG NOTE: texelSize must be 1.0 / bloomRT.size, NOT 1.0 / viewportSize.
        // This pass renders into the bloom RT, so gl_FragCoord is in bloom-RT space.
        // Using viewport texel size shifts UVs out of [0,1], causing clamp-to-edge
        // bright rectangles covering the portion where bloomRT < viewport.
        vec2 uv = gl_FragCoord.xy * texelSize;

        const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.05405405, 0.016216216);
        vec3 color = texture(BloomTexture, uv).rgb * weights[0];
        for (int i = 1; i < 5; ++i)
        {
                vec2 offset = direction * texelSize * float(i);
                color += texture(BloomTexture, uv + offset).rgb * weights[i];
                color += texture(BloomTexture, uv - offset).rgb * weights[i];
        }
        outColor = vec4(color, 1.0);
}
