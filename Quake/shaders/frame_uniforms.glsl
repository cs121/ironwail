#ifndef FRAME_UNIFORMS_GLSL
#define FRAME_UNIFORMS_GLSL

layout(std140, binding=0) uniform FrameDataUBO
{
        mat4    ViewProj;
        mat4    PrevViewProj;
        vec4    Fog;
        vec4    SkyFog;
        vec3    WindDir;
        float   WindPhase;
        float   ScreenDither;
        float   TextureDither;
        float   Overbright;
        uint    ShadingModel;
        vec3    EyePos;
        float   Time;
        vec3    PrevEyePos;
        float   DeltaTime;
        float   ZLogScale;
        float   ZLogBias;
        vec4    LightmapMod;
        vec4    LightmapWave;
        vec4    GlowParams;
        uint    NumLights;
        uint    PrevFrameValid;
        uint    _Pad1;
        uint    _Pad2;
};

#endif // FRAME_UNIFORMS_GLSL
