// shadow_depth.frag — depth-only shadow pass
// This fragment shader is intentionally empty: for a depth-only render pass,
// the hardware writes gl_FragCoord.z to the depth buffer automatically.
// No colour attachment is used (glDrawBuffer(GL_NONE)).
//
// NOTE: Do NOT add layout(early_fragment_tests) in; here.
// Reasons:
//   1. early_fragment_tests is only beneficial when the fragment shader
//      WRITES to gl_FragDepth — an empty shader never does this, so the
//      driver already applies hardware early-Z automatically.
//   2. Some drivers (particularly AMD/Mesa on reversed-Z framebuffers) have
//      subtle bugs with early_fragment_tests + depth-only FBOs that can cause
//      depth values to be written incorrectly or skipped.
//   3. The qualifier changes the shader hash, forcing a re-link on every map
//      load (prog ID changes from session to session) with no rendering benefit.

void main()
{
    // Intentionally empty.
    // Depth is written from gl_FragCoord.z by the hardware.
}
