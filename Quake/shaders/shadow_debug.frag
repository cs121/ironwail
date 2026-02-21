// shadow_debug.frag — hardened
// Fixes:
//   1. Depth-Visualisierung mit linearem Mapping: Roher depth-Wert aus dem
//      Shadow-Map ist nicht-linear (perspektivische Projektion). Die direkte
//      Ausgabe als Graustufe erzeugt ein fast vollständig weißes oder schwarzes
//      Bild — schlecht für Debugging.
//      FIX: Optional REVERSED_Z-korrekte Visualisierung + manueller LinearDepth-
//      Pfad wenn ShadowParams.x/y als near/far verfügbar sind.
//      Als Fallback: pow()-basiertes Gamma-Enhance für bessere Sichtbarkeit.
//   3. Kein Sampler-Layout-Qualifier-Set: binding=0 war vorhanden — korrekt.
//      Aber ohne expliziten Präzisions-/Format-Qualifier auf dem Sampler könnte
//      auf ES-Profilen ein Fehler entstehen. Für Desktop GL ist es OK.


layout(binding = 0) uniform sampler2D ShadowMap;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_fragcolor;

// Optionale Uniforms für linearisierte Tiefendarstellung.
// Wenn die Engine diese nicht bindet, werden die Defaults (near=1, far=4096)
// genutzt — Ergebnis ist dann eine Näherung, aber immer sichtbar.
layout(location = 1) uniform float u_near = 1.0;
layout(location = 2) uniform float u_far  = 4096.0;

// Linearisiert einen [0,1]-Depth-Wert zurück in view-space Distanz [near,far],
// dann normalisiert auf [0,1] für die Visualisierung.
float LinearizeDepth(float d)
{
#if REVERSED_Z
    // REVERSED_Z: nah=1, fern=0. Formel analog zu Standard, aber d invertieren.
    float d_std = 1.0 - d; // in Standard-Konvention umrechnen
    return (2.0 * u_near * u_far) /
           (u_far + u_near - d_std * (u_far - u_near));
#else
    return (2.0 * u_near * u_far) /
           (u_far + u_near - (d * 2.0 - 1.0) * (u_far - u_near));
#endif
}

void main()
{
    float depth = texture(ShadowMap, v_uv).r;

#ifdef SHADOW_DEBUG_LINEAR
    // Linearisierte, normalisierte Tiefe für bessere Visualisierung
    float linear_d = LinearizeDepth(depth);
    float vis = clamp(linear_d / u_far, 0.0, 1.0);
    out_fragcolor = vec4(vec3(vis), 1.0);
#else
    // Direktes Mapping: für ortho-Projektionen (Sun-Shadow) ist das OK.
    // Für Perspective: gamma-Anpassung macht Details sichtbarer.
    float vis = pow(depth, 0.25); // Gamma 0.25 hebt dunkle Bereiche an
    out_fragcolor = vec4(vec3(vis), 1.0);
#endif
}
