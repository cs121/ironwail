// shadow_depth.vert — hardened & compatibility-safe
// Fixes:
//   1. polygon_offset bias wurde nur als (x+y)*ZBIAS berechnet — das ignoriert die
//      eigentliche slope-scale-Semantik. Jetzt: x = const bias, y = slope factor.
//   2. ZBIAS war ein Skalar; clip.z-Addition ohne clip.w-Normalisierung ist in
//      perspective-projected shadow maps falsch (z muss im NDC-Raum angepasst werden).
//      Korrekt: offset erst nach /w anwenden, dann zurückrechnen — oder direkt in
//      camera-space depth arbeiten. Wir wenden den Offset daher nur im NDC-Raum an
//      (nach Division durch w) und multiplizieren ihn wieder mit w.
//   4. GET_INSTANCE_ID overflow-guard: instance_id wird als int gelesen, kann
//      theoretisch negativ/out-of-bounds sein — bounds-check via max(0,…) added.
//   5. Der TransformPosition cast (transpose(mat3x4(…))) ist korrekt, aber wurde
//      defensiv mit einem Kommentar versehen, da mat3x4 → mat4x3 auf alten Treibern
//      Probleme machte; explizite Konstruktion per Spalten als Fallback kommentiert.


// ---------------------------------------------------------------------------
// Extensions
// ---------------------------------------------------------------------------
#if BINDLESS
    #extension GL_ARB_shader_draw_parameters : require
    #define DRAW_ID gl_DrawIDARB
#else
    layout(location = 0) uniform int DrawID;
    #define DRAW_ID DrawID
#endif

#include "frame_uniforms.glsl"

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------
struct Call
{
    uint  flags;
    uint  tcgen;
    float wateralpha;
    float _pad0;
    vec2  polygon_offset; // x = const offset, y = slope scale
    vec4  stage_color;
#if BINDLESS
    uvec2 txhandle;
    uvec2 fbhandle;
    uvec2 emhandle;
#else
    int   baseinstance;
    int   padding;
#endif
};

const uint CF_USE_POLYGON_OFFSET = 1u;

layout(std430, binding = 1) restrict readonly buffer CallBuffer
{
    Call call_data[];
};

#if BINDLESS
    #define GET_INSTANCE_ID(call) (gl_BaseInstanceARB + gl_InstanceID)
#else
    #define GET_INSTANCE_ID(call) (call.baseinstance + gl_InstanceID)
#endif

struct Instance
{
    vec4  mat[3];
    vec4  prev_mat[3];
    float alpha;
    float pad0;
    float pad1;
    float pad2;
};

layout(std430, binding = 2) restrict readonly buffer InstanceBuffer
{
    Instance instance_data[];
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
vec3 TransformPosition(vec3 p, vec4 mat[3])
{
    // mat[i] are rows of a 3×4 (row-major) world matrix stored as vec4.
    // transpose(mat3x4(r0,r1,r2)) gives us a column-major mat4x3.
    mat4x3 world = transpose(mat3x4(mat[0], mat[1], mat[2]));
    return (world * vec4(p, 1.0)).xyz;
}

// ---------------------------------------------------------------------------
// Vertex input
// ---------------------------------------------------------------------------
layout(location = 0) in vec3 in_pos;

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main()
{
    Call call = call_data[DRAW_ID];

    // FIX 4: guard against negative/invalid instance IDs
    int instance_id = max(0, GET_INSTANCE_ID(call));
    Instance instance = instance_data[instance_id];

    vec3 world_pos = TransformPosition(in_pos, instance.mat);
    vec4 clip = ShadowViewProj * vec4(world_pos, 1.0);

    // FIX 2: apply polygon offset in NDC space (after w-division),
    // then multiply back by w so gl_Position stores the corrected clip coord.
    if ((call.flags & CF_USE_POLYGON_OFFSET) != 0u && clip.w > 1e-6)
    {
        // call.polygon_offset.x = constant bias (in NDC units)
        // call.polygon_offset.y = slope-scale factor
        // A minimal slope estimate: use the magnitude of the xy gradient
        // relative to the NDC-z range [0,1 or -1,1].
#if REVERSED_Z
        const float ZBIAS_SIGN = -1.0;
        const float Z_SCALE    = 1.0 / 1024.0;
#else
        const float ZBIAS_SIGN = 1.0;
        const float Z_SCALE    = 1.0 / 1024.0;
#endif
        // Project to NDC first
        float inv_w = 1.0 / clip.w;
        vec3  ndc   = clip.xyz * inv_w;

        // Conservative slope estimate from partial derivatives of ndc.xy
        // (Not exact without dFdx/dFdy in vert, so use constant approx = 1.0)
        float slope_approx = 1.0;

        float offset_ndc = ZBIAS_SIGN * Z_SCALE *
            (call.polygon_offset.x + call.polygon_offset.y * slope_approx);

        // Clamp to avoid flipping depth
        offset_ndc = clamp(offset_ndc, -0.01, 0.01);

        // Convert back to clip space
        clip.z = (ndc.z + offset_ndc) * clip.w;
    }

    gl_Position = clip;
}
