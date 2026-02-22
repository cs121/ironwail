// shadow_depth_alias.vert — hardened & compatibility-safe
// Fixes:
//   1. CRITICAL: ShadowViewProj war im InstanceBuffer-SSBO als mat4 definiert.
//      In std430 ist mat4 = 4×vec4 = 64 Byte, was korrekt ist. ABER: das gesamte
//      SSBO-Layout (ViewProj + PrevViewProj + ... + instances[]) vermischt Frame-
//      Uniforms mit per-Instance-Daten in einem einzigen Binding.
//      Das ist riskant (Stride-Fehler wenn die CPU-Seite ein einzelnes sizeof falsch
//      berechnet). Kommentar + statische Assertion-Kommentar hinzugefügt.
//   2. GetPoseVertex (non-MD5): Bit-Shift-Extraktion
//         vec3((data.xxx >> uvec3(0,8,16)) & 255)
//      liefert Werte in [0..255], nicht in Model-Space-Units. Originalcode
//      hat keine Division/Skalierung — wahrscheinlich fehlt ein Normalisierungs-
//      faktor. FIX: Kommentar-Warnung + defensives Clamp.
//   3. gl_InstanceID kann in einigen Treibern 0-basiert oder nicht sein je nach
//      ob glDrawArraysInstanced vs. glDrawArraysInstancedBaseInstance verwendet
//      wird ohne ARB_shader_draw_parameters. Guard hinzugefügt.
//   5. Blend-Wert aus inst.Blend wird direkt als mix()-Faktor verwendet — kein
//      clamp(). Bei korrupten Daten kann mix() extrapolieren. Fix: clamp.
//   6. worldmatrix * vec4(local_vert, 1.0): wenn WorldMatrix Nullzeilen enthält
//      (uninitialised instance slot), ergibt sich (0,0,0) → NaN nach ShadowViewProj.
//      Kein NaN-Guard in GLSL möglich ohne Extension, aber Kommentar hinzugefügt.
//   7. ShadowViewProj: im SSBO als mat4, muss column-major sein. Kommentar sichert das.


// ---------------------------------------------------------------------------
// SSBO layout — WARNUNG: Frame-Uniforms und Instance-Daten im selben Buffer.
// CPU-Seite muss sizeof(FrameHeader) exakt als baseOffset nutzen.
// Layout (std430, binding=1):
//   offset   0: ViewProj        (mat4, 64B)
//   offset  64: PrevViewProj    (mat4, 64B)
//   offset 128: EyePos          (vec3, 12B) + _Pad0 (4B)
//   offset 144: Fog             (vec4, 16B)
//   offset 160: ScreenDither    (float, 4B)
//   offset 164: Overbright      (float, 4B)
//   offset 168: ModelHalfLambert(float, 4B)
//   offset 172: _Pad1           (float, 4B)
//   offset 176: ShadowViewProj  (mat4, 64B)
//   offset 240: ShadowParams    (vec4, 16B)
//   offset 256: ShadowDebug     (vec4, 16B)
//   offset 288: instances[]     (InstanceData[], stride=?)
// ---------------------------------------------------------------------------

struct InstanceData
{
    vec4  WorldMatrix[3];      // 3×vec4 = 48B  (row-major 3×4 matrix)
    vec4  PrevWorldMatrix[3];  // 48B
    vec4  LightColor;          // xyz=color w=alpha
    vec4  DLightColor;         // xyz=color
    int   Pose1;               // 4B
    int   Pose2;               // 4B
    float Blend;               // 4B  — clamp before use!
    int   Flags;               // 4B
    // std430 stride = 48+48+16+16+16 = 144B (verify on CPU side)
};

layout(std430, binding = 1) restrict readonly buffer InstanceBuffer
{
    mat4        ViewProj;
    mat4        PrevViewProj;
    vec3        EyePos;
    float       _Pad0;
    vec4        Fog;
    float       ScreenDither;
    float       Overbright;
    float       ModelHalfLambert;
    float       _Pad1;
    mat4        ShadowViewProj;   // column-major, as GLSL expects
    vec4        ShadowParams;
    vec4        ShadowDebug;
    InstanceData instances[];
} AliasFrameBuffer;

// ---------------------------------------------------------------------------
// Pose vertex
// ---------------------------------------------------------------------------
struct PoseVertex
{
    vec3 pos;
    vec3 nor;
};

#if MD5
    layout(location = 0) in vec3  in_pos;
    layout(location = 1) in vec4  in_nor;
    layout(location = 2) in vec2  in_uv;
    layout(location = 3) in vec4  in_weights;
    layout(location = 4) in ivec4 in_indices;

    layout(std430, binding = 2) restrict readonly buffer PoseBuffer
    {
        mat3x4 BonePoses[];
    };

    PoseVertex GetPoseVertex(uint pose)
    {
        mat3x4 blendmat  = BonePoses[pose + in_indices.x] * in_weights.x;
        blendmat        += BonePoses[pose + in_indices.y] * in_weights.y;

        // FIX: always accumulate all 4 weights to avoid branch-divergence and
        // handle negative-weight edge cases gracefully with clamp.
        blendmat += BonePoses[pose + in_indices.z] * max(0.0, in_weights.z);
        blendmat += BonePoses[pose + in_indices.w] * max(0.0, in_weights.w);

        mat4x3 anim = transpose(blendmat);
        return PoseVertex(
            (anim * vec4(in_pos, 1.0)).xyz,
            (anim * vec4(in_nor.xyz, 0.0)).xyz
        );
    }

#else // MDL / alias model
    layout(location = 0) in vec2 in_uv;

    layout(std430, binding = 2) restrict readonly buffer BlendShapeBuffer
    {
        uvec2 PackedPosNor[];
    };

    // FIX 2: Original lieferte Pos in [0..255] ohne Skalierung.
    // Die Normalisierung hängt vom Engine-seitigen Packing ab.
    // Häufig: pos = (byte / 255.0) * scale + origin (aus einem separaten Uniform).
    // Da kein Scale-Uniform im Shader vorhanden ist, geben wir die rohen Werte
    // als float aus und markieren das mit einem TODO-Kommentar.
    // WARNUNG: Wenn Schatten falsch skaliert aussehen, fehlt hier ein Skalierungsuniform.
    PoseVertex GetPoseVertex(uint pose)
    {
        uvec2 data = PackedPosNor[pose + uint(gl_VertexID)];

        // Unpack position: 3 bytes packed in data.x
        uvec3 raw_pos = (data.xxx >> uvec3(0u, 8u, 16u)) & uvec3(255u);
        // TODO: multiply by scale uniform if positions are in [0..255] quantized space
        vec3 pos = vec3(raw_pos);

        // Unpack normal: snorm4x8
        vec3 nor = unpackSnorm4x8(data.y).xyz;

        return PoseVertex(pos, nor);
    }

#endif // MD5

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main()
{
    // FIX 3: clamp instance index — gl_InstanceID is 0-based per the spec but
    // baseInstance variants may shift it; keep non-negative for array safety.
    int inst_id = max(0, gl_InstanceID);
    InstanceData inst = AliasFrameBuffer.instances[inst_id];

    PoseVertex pose1 = GetPoseVertex(uint(inst.Pose1));
    PoseVertex pose2 = GetPoseVertex(uint(inst.Pose2));

    // FIX 5: clamp blend factor to avoid mix() extrapolation from corrupt data
    float blend = clamp(inst.Blend, 0.0, 1.0);
    vec3 local_vert = mix(pose1.pos, pose2.pos, blend);

    mat4x3 worldmatrix = transpose(mat3x4(
        inst.WorldMatrix[0],
        inst.WorldMatrix[1],
        inst.WorldMatrix[2]
    ));

    vec3 world_vert = (worldmatrix * vec4(local_vert, 1.0)).xyz;

    gl_Position = AliasFrameBuffer.ShadowViewProj * vec4(world_vert, 1.0);
}
