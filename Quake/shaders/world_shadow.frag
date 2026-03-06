// BUG FIX: location=4 kollidierte mit ShadowViewProj (mat4, belegt locations 4-7) im Vert-Shader.
// NVIDIA-Treiber lehnt Duplicate-Location-Fehler ab; AMD/Intel ignorierten es stillschweigend.
// ShadowLightPosFar → 8 (erste freie Slot nach mat4-Block), ShadowMode → 9.
layout(location=8) uniform vec4 ShadowLightPosFar; // xyz: light pos, w: far plane
layout(location=9) uniform int ShadowMode;          // 0=sun, 1=dlight cube

layout(location=0) in vec3 in_world_pos;

void main()
{
	if (ShadowMode != 0)
	{
		// Cubemap dlight shadow: lineare Distanz normalisiert auf [0,1].
		// Kein Reversed-Z hier — cubemap depth ist immer linear.
		float farPlane = max(ShadowLightPosFar.w, 1e-5);
		float dist = length(in_world_pos - ShadowLightPosFar.xyz);
		gl_FragDepth = clamp(dist / farPlane, 0.0, 1.0);
	}
	else
	{
		// Sun shadow (orthografisch): Standard-Tiefen aus dem Rasterizer uebernehmen.
		// BUG FIX: Bei Reversed-Z liegt gl_FragCoord.z bereits im [0,1]-Raum mit
		// near=1, far=0. SampleSunShadow() liest depth = ndc.z*0.5+0.5 und vergleicht
		// mit dem gespeicherten Wert — beide muessen dasselbe Reversed-Z-Vorzeichen haben.
		// gl_FragCoord.z ist direkt korrekt; kein Umrechnen noetig, SOFERN die
		// ShadowViewProj-Matrix schon Reversed-Z eingebaut hat (near/far getauscht).
		// Explizit schreiben um Treiber-Ambiguitaeten zu vermeiden:
#if REVERSED_Z
		gl_FragDepth = gl_FragCoord.z;   // bereits [1→0], konsistent mit SampleSunShadow
#else
		gl_FragDepth = gl_FragCoord.z;   // standard [0→1]
#endif
	}
}
