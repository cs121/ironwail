layout(binding=0) uniform sampler2D SceneColorTex;
layout(binding=1) uniform sampler2D FogIntegratedTex;

layout(location=0) out vec4 outColor;

void main()
{
	vec2 uv = gl_FragCoord.xy / vec2(textureSize(SceneColorTex, 0));
	vec4 scene = texture(SceneColorTex, uv);
	vec3 fog = texture(FogIntegratedTex, uv).rgb;
	outColor = vec4(scene.rgb + fog, scene.a);
}
