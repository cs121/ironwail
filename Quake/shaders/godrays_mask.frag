layout(binding=0) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4 MaskParams; // x: sun x, y: sun y, z: reversed-z flag, w: debug mode
#include "depth_common.glsl"
layout(location=1) uniform vec4 DownsampleParams; // xy: source size, zw: scale from target to source

layout(location=0) out vec4 outColor;

void main()
{
        vec2 sourceSize = DownsampleParams.xy;
        vec2 scale = DownsampleParams.zw;
        vec2 base = (gl_FragCoord.xy + vec2(0.5)) * scale;
        ivec2 coord = ivec2(clamp(base, vec2(0.0), max(sourceSize - vec2(1.0), vec2(0.0))));
        float depth = texelFetch(DepthTexture, coord, 0).r;
        bool reversedZ = MaskParams.z > 0.5;
        float linearDepth = DepthRawToCanonical(depth, reversedZ ? 1.0 : 0.0);

        float skyVis = DepthIsSkyDepth(depth, reversedZ ? 1.0 : 0.0, reversedZ ? 0.0015 : 0.9985) ? 1.0 : 0.0;
        vec2 uv = (gl_FragCoord.xy + vec2(0.5)) / max(vec2(textureSize(DepthTexture, 0)) / max(scale, vec2(1.0)), vec2(1.0));
        float distToSun = length(uv - MaskParams.xy);
        float sunDisk = smoothstep(0.25, 0.0, distToSun);
        float occ = skyVis * sunDisk;

        if (MaskParams.w > 2.5 && MaskParams.w < 3.5)
        {
                outColor = vec4(vec3(linearDepth), 1.0);
                return;
        }

        outColor = vec4(vec3(occ), occ);
}
