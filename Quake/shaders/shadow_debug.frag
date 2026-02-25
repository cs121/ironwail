layout(binding=0) uniform sampler2D ShadowDebugTex;

layout(location=0) out vec4 out_fragcolor;

void main()
{
	vec2 uv = gl_FragCoord.xy / vec2(textureSize(ShadowDebugTex, 0));
	float depth = texture(ShadowDebugTex, uv).r;
	out_fragcolor = vec4(depth, depth, depth, 1.0);
}
