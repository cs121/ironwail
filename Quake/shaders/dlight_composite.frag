layout(binding=0) uniform sampler2D DlightTexture;
layout(location=0) uniform float DlightScale;

layout(location=0) out vec4 outColor;

void main()
{
        vec2 texSize = vec2(textureSize(DlightTexture, 0));
        vec2 uv = gl_FragCoord.xy / texSize;
        vec3 color = texture(DlightTexture, uv).rgb * DlightScale;
        outColor = vec4(color, 1.0);
}
