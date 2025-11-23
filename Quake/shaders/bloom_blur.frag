layout(binding=0) uniform sampler2D BloomTexture;

layout(location=0) uniform vec4 BlurParams; // xy: texel size, zw: direction

layout(location=0) out vec4 outColor;

void main()
{
        vec2 texelSize = BlurParams.xy;
        vec2 direction = BlurParams.zw;
        vec2 uv = (gl_FragCoord.xy + 0.5) * texelSize;
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
