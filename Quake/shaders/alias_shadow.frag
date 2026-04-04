// BUG FIX: location=4 kollidierte mit ShadowViewProj im jeweiligen Vert-Shader.
// alias_shadow.vert hat ShadowViewProj auf location=8 (mat4, belegt 8-11).
// ShadowLightPosFar → 12, ShadowMode → 13 um den mat4-Block zu umgehen.
layout(location=12) uniform vec4 ShadowLightPosFar; // xyz: light pos, w: far plane
layout(location=13) uniform int ShadowMode;          // 0=sun, 1=dlight cube

layout(location=0) in vec3 in_world_pos;

void main()
{
	if (ShadowMode != 0)
	{
		// Cubemap dlight shadow: lineare Distanz normalisiert auf [0,1].
		// Kein Reversed-Z — cubemap depth ist immer linear.
		float farPlane = max(ShadowLightPosFar.w, 1e-5);
		float dist = length(in_world_pos - ShadowLightPosFar.xyz);
		gl_FragDepth = clamp(dist / farPlane, 0.0, 1.0);
	}
	else
	{
		// Sun shadow: explizit schreiben um Treiber-Ambiguitaet zu vermeiden.
		// ShadowViewProj muss Reversed-Z bereits eingebaut haben (near/far vertauscht),
		// dann ist gl_FragCoord.z direkt konsistent mit dem Vergleich in SampleSunShadow.
		gl_FragDepth = gl_FragCoord.z;
	}
}
