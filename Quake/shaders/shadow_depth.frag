// shadow_depth.frag — hardened
// Original war leer (nur main{}). Das ist für reine Depth-Passes auf den meisten
// Treibern legal, aber AMD/Mesa warnen manchmal bei vollständig leerem Fragment-Shader
// ohne explizite early-z-Annotation.
//
// Fix: early_fragment_tests hint + leerer Body.


// Hint für Treiber: Fragment wird nur für Depth-Write genutzt —
// erlaubt Hardware-Early-Z ohne separaten Depth-Prepass-Pass.
layout(early_fragment_tests) in;

void main()
{
    // Intentionally empty: depth is written automatically from gl_FragDepth
    // (which defaults to gl_FragCoord.z). No colour attachment needed.
}
