layout(binding=0) uniform sampler2D DlightTexture;
layout(location=0) uniform float DlightScale;

layout(location=0) out vec4 outColor;

void main()
{
        vec2 texSize = vec2(textureSize(DlightTexture, 0));
        vec2 uv = gl_FragCoord.xy / texSize;
        vec3 color = texture(DlightTexture, uv).rgb * DlightScale;
        // BP FIX #6: world_dlight.frag schreibt alpha=0 (additiver Beitrag).
        // dlight_composite.frag schrieb alpha=1 was inkonsistent ist und
        // nachgelagerte Alpha-Reads (OIT, Wasseralpha) korrumpiert.
        // Alpha=0 beibehalten da dieser Pass rein additiv composited.
        outColor = vec4(color, 0.0);
}
