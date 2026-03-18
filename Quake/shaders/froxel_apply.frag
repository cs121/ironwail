layout(binding=0) uniform sampler2D SceneColorTex;
layout(binding=1) uniform sampler2D FogIntegratedTex;

layout(location=0) out vec4 outColor;

void main()
{
	vec2 uv = gl_FragCoord.xy / vec2(textureSize(SceneColorTex, 0));
	vec4 scene = texture(SceneColorTex, uv);
	// BUG FIX (Bug 2b): Vorher scene.rgb + fog.rgb — blind additiv,
	// ignoriert Transmittanz komplett → gleichmäßiger Helligkeits-Overlay.
	// froxel_integrate schreibt jetzt A = (1 - transmittance).
	// Korrekte physikalische Komposition: Beer-Lambert.
	vec4 fog = texture(FogIntegratedTex, uv);
	float transmittance = clamp(1.0 - fog.a, 0.0, 1.0);
	outColor = vec4(scene.rgb * transmittance + fog.rgb, scene.a);
}
