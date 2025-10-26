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

#define PLAYER_SHADOW_SEGMENTS 16

layout(location=0) uniform vec4 CenterRadius; // xyz=center, w=radius
layout(location=1) uniform vec4 TangentAlpha; // xyz=tangent, w=alpha
layout(location=2) uniform vec4 BitangentSoftness; // xyz=bitangent, w=softness
layout(location=3) uniform vec4 NormalOffset; // xyz=normal, w=offset

layout(location=0) out vec2 out_local;
layout(location=1) flat out float out_alpha;
layout(location=2) flat out float out_softness;
layout(location=3) out vec3 out_viewpos;

void main()
{
       vec3 center = CenterRadius.xyz;
       float radius = CenterRadius.w;
       vec3 tangent = TangentAlpha.xyz;
       vec3 bitangent = BitangentSoftness.xyz;
       vec3 normal = NormalOffset.xyz;
       float offset = NormalOffset.w;
       vec2 local;
       if (gl_VertexID == 0)
       {
               local = vec2(0.0);
       }
       else
       {
               int idx = gl_VertexID - 1;
               float angle = float(idx % PLAYER_SHADOW_SEGMENTS) / float(PLAYER_SHADOW_SEGMENTS) * (3.14159265 * 2.0);
               local = vec2(cos(angle), sin(angle));
       }
       out_local = local;
       out_alpha = TangentAlpha.w;
       out_softness = BitangentSoftness.w;
       vec3 worldPos = center + (tangent * local.x + bitangent * local.y) * radius + normal * offset;
       out_viewpos = worldPos - EyePosTime.xyz;
       gl_Position = ViewProj * vec4(worldPos, 1.0);
}
