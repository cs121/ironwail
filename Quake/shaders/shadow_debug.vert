// shadow_debug.vert — hardened
// Original war korrekt (fullscreen-triangle via gl_VertexID Trick).
// Fixes:
//   1. v_uv = pos * 0.5: pos ist in [0,2], also v_uv in [0,1]. Korrekt.
//      Bei GL_REPEAT könnte der zweite Vertex (gl_VertexID==2, pos=(0,2)) ein
//      UV von 1.0 produzieren — das ist genau auf der Grenze. OK, kein Bug.
//   2. gl_Position.z = 0.0 (NDC z=0): korrekt für Debug-Overlay (immer im
//      sichtbaren NDC-Bereich, egal ob REVERSED_Z oder nicht). Dokumentiert.


layout(location = 0) out vec2 v_uv;

void main()
{
    // Fullscreen triangle (3 Vertices, kein VBO nötig):
    //   VertexID 0: pos=(0,0) → ndc=(-1,-1), uv=(0,0)
    //   VertexID 1: pos=(2,0) → ndc=(3,-1),  uv=(1,0)
    //   VertexID 2: pos=(0,2) → ndc=(-1,3),  uv=(0,1)
    // Das überdeckt den gesamten Viewport mit einem einzigen Triangle.
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv        = pos * 0.5;

    // z=0.0: sichtbar in beiden NDC-Konventionen (Standard und REVERSED_Z),
    // da 0 immer innerhalb [-1,1] bzw. [0,1] liegt.
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
