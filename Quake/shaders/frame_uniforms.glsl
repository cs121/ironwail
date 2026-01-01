#ifndef FRAME_UNIFORMS_GLSL
#define FRAME_UNIFORMS_GLSL

#define SHADOW_DLIGHT_MAX 4

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
        float   _Pad0;
        vec3    EyePos;
        float   Time;
        vec3    PrevEyePos;
        float   DeltaTime;
        float   ZLogScale;
        float   ZLogBias;
        vec4    LightmapParams;
        vec4    LightgridParams;
        vec4    DLightParams;
        vec4    ColorSpaceParams;
        vec4    ShaderParams;
        mat4    ShadowViewProj;
        vec4    ShadowParams;
        vec4    ShadowControl;
        vec4    ShadowDebug;
        vec4    ShadowSunDir;
        mat4    ShadowDlightViewProj[SHADOW_DLIGHT_MAX];
        vec4    ShadowDlightAtlas[SHADOW_DLIGHT_MAX];
        vec4    ShadowDlightInfo[SHADOW_DLIGHT_MAX];
        vec4    ShadowDlightParams;
        uint    NumLights;
        uint    PrevFrameValid;
        uint    _Pad1;
        uint    _Pad2;
};

#endif // FRAME_UNIFORMS_GLSL
