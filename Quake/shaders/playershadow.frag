layout(std140, binding=0) uniform FrameDataUBO
{
       mat4    ViewProj;
       mat4    InvProj;
       mat4    Proj;
       vec4    Fog;
       vec4    SkyFog;
       vec4    WindDir;
       float   ScreenDither;
       float   TextureDither;
       float   ShadowFilterRadius;
       float   OverbrightScale;
       vec4    EyePosTime;
       float   StaticShadowBlur;
       float   StaticLightmapStrength;
       float   ZLogScale;
       float   ZLogBias;
       float   CameraNear;
       float   CameraFar;
       uint    NumLights;
       int     ClipZeroToOne;
       int     _FrameDataPad0;
       int     _FrameDataPad1;
};

vec3 ApplyFog(vec3 clr, vec3 p)
{
       float fog = exp2(-Fog.w * dot(p, p));
       fog = clamp(fog, 0.0, 1.0);
       return mix(Fog.rgb, clr, fog);
}


layout(location=0) in vec2 in_local;
layout(location=1) flat in float in_alpha;
layout(location=2) flat in float in_softness;
layout(location=3) in vec3 in_viewpos;

layout(location=0) out vec4 out_fragcolor;

void main()
{
       float r = length(in_local);
       float softness = max(in_softness, 0.0001);
       float fade = clamp((1.0 - r) / softness, 0.0, 1.0);
       fade *= fade;
       float alpha = in_alpha * fade;
       if (alpha <= 0.0)
               discard;
       float fog = exp2(abs(Fog.w) * -dot(in_viewpos, in_viewpos));
       fog = clamp(fog, 0.0, 1.0);

       // Fade shadow intensity with fog distance without tinting towards the fog colour.
       // Using the fog colour here made the shadow appear as a bright disc instead of darkening
       // the ground beneath the player.
       alpha *= fog;
       out_fragcolor = vec4(vec3(0.0), alpha);
}
