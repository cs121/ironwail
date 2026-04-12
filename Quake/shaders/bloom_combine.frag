layout(binding=0) uniform sampler2D BloomTexture;

layout(location=0) uniform vec4 CombineParams; // x: weight

layout(location=0) out vec4 outColor;

void main()
{
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(BloomTexture, 0));
        float weight = max(CombineParams.x, 0.0);
        vec3 color = texture(BloomTexture, uv).rgb * weight;
        outColor = vec4(color, 1.0);
}
