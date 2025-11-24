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
        float   RimWorld;
        float   RimModels;
        float   _Pad0;
        float   _Pad1;
        float   _Pad2;
        float   _Pad3;
        vec3    EyePos;
        float   Time;
        vec3    PrevEyePos;
        float   DeltaTime;
        float   ZLogScale;
        float   ZLogBias;
        uint    NumLights;
        uint    PrevFrameValid;
        uint    _Pad4;
        uint    _Pad5;
};

#endif // FRAME_UNIFORMS_GLSL
