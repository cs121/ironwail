#ifndef FRAME_UNIFORMS_GLSL
#define FRAME_UNIFORMS_GLSL

#define SHADOW_DLIGHT_MAX 4

layout(std140, binding=0) uniform FrameDataUBO
{
    //  View  offset   0
    mat4    ViewProj;       //   0
    mat4    PrevViewProj;   //  64
    mat4    View;           // 128

    //  Environment  offset 192
    vec4    Fog;            // 192
    vec4    SkyFog;         // 208

    //  Wind / timing  offset 224
    vec3    WindDir;        // 224  (vec3 in std140  12B data, 4B implicit after)
    float   WindPhase;      // 236

    //  Screen / render params  offset 240
    float   ScreenDither;   // 240
    float   TextureDither;  // 244
    float   Overbright;     // 248
    float   _Pad0;          // 252  (reserved)

    //  Camera  offset 256
    vec3    EyePos;         // 256
    float   Time;           // 268
    vec3    PrevEyePos;     // 272
    float   DeltaTime;      // 284

    //  Logarithmic depth  offset 288
    float   ZLogScale;      // 288
    float   ZLogBias;       // 292
    // BUG FIX: std140 legt vec4 auf 16-Byte-Grenze. Nach ZLogBias (offset 296)
    // fehlen 8 Byte bis zur nachsten 16B-Grenze (304). Ohne explizites Padding
    // liest die CPU LightmapParams 8 Byte zu fruh  alle nachfolgenden Uniforms
    // sind verschoben. Explizites Padding macht den Gap sichtbar und erzwingt
    // korrektes CPU-seitiges Struct-Layout.
    float   _PadZ0;         // 296
    float   _PadZ1;         // 300

    //  Lighting params  offset 304
    vec4    LightmapParams;     // 304
    vec4    LightgridParams;    // 320
    vec4    DLightParams;       // 336
    vec4    ColorSpaceParams;   // 352
    vec4    ShaderParams;       // 368  x: shader debug, y: tcgen debug, z: legacy sun visibility, w: ambient-sky scale
    vec4    SunDirEnabled;  // 384  xyz: scene->sun direction, w: enabled
    vec4    SunColorIntensity; // 400 rgb: sun color, w: intensity
    vec4    SkyVisTint;     // 416 shared ambient-sky contract: rgb tint, w cap
    vec4    CausticsParams0; // 432 x: enabled, y: medium(1=water,2=lava), z: intensity, w: scale
    vec4    CausticsParams1; // 448 x: speed, y: debug, z/w: reserved

    //  Global params  offset 464
    // NOTE: These members must stay layout-compatible with gpuframedata_t
    // their expected offsets.


    //  Misc  offset 464
    uint    NumLights;      // 464
    uint    PrevFrameValid; // 468
    uint    _Pad1;          // 472
    uint    _Pad2;          // 476
};                          // Total: 480 bytes

float AmbientSkyEnabled()
{
    return (LightgridParams.z > 0.5) ? 1.0 : 0.0;
}

float AmbientSkyDebugEnabled()
{
    return (LightgridParams.w > 0.5) ? 1.0 : 0.0;
}

float AmbientSkyScale()
{
    return max(ShaderParams.w, 0.0);
}

vec3 AmbientSkyTint()
{
    return max(SkyVisTint.rgb, vec3(0.0));
}

float AmbientSkyCap()
{
    return max(SkyVisTint.a, 0.0);
}

vec3 AmbientSkyDiffuse(float visibility, float upness, float room)
{
    float sky = AmbientSkyEnabled() * AmbientSkyScale() * clamp(visibility, 0.0, 1.0)
        * mix(0.2, 1.0, clamp(upness, 0.0, 1.0)) * max(room, 0.0);
    vec3 diffuse = AmbientSkyTint() * sky;
    return min(diffuse, vec3(AmbientSkyCap()));
}


#endif // FRAME_UNIFORMS_GLSL
