layout(binding=0) uniform sampler2D BloomTexture;

layout(location=0) uniform vec4 CombineParams; // x: weight
layout(location=1) uniform vec4 ViewportParams; // xy: 1.0/viewportSize (texel size in viewport space)

layout(location=0) out vec4 outColor;

void main()
{
        // BUG FIX: gl_FragCoord is in viewport/render-target space.
        // Dividing by textureSize(BloomTexture) is wrong when the bloom RT
        // is a different resolution than the viewport (e.g. half/quarter res).
        // Use the viewport texel size uniform instead so UVs always stay in [0,1].
        vec2 uv = gl_FragCoord.xy * ViewportParams.xy;
        float weight = max(CombineParams.x, 0.0);
        vec3 color = texture(BloomTexture, uv).rgb * weight;
        outColor = vec4(color, 1.0);
}
