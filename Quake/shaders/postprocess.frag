layout(binding=0) uniform sampler2D GammaTexture;
layout(binding=1) uniform usampler3D PaletteLUT;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D BloomTexture;
layout(binding=4) uniform sampler2D VelocityTexture;
layout(std430, binding=0) restrict readonly buffer PaletteBuffer
{
        uint Palette[256];
};

layout(location=0) uniform vec4 Params;
layout(location=1) uniform vec4 DoFParams0; // x: enabled, y: focus distance, z: focus range, w: max blur radius (pixels)
layout(location=2) uniform vec4 DoFParams1; // x: near plane, y: far plane, z: reversed-Z flag (>0.5 when reversed)
layout(location=3) uniform vec4 ViewRect;   // xy: view min (normalized), zw: view max (normalized)
layout(location=4) uniform vec4 DepthParams; // xy: inverse view scale, zw: unused
layout(location=5) uniform vec3 HDRParams; // x: bloom intensity, y: exposure, z: tonemap mode
layout(location=6) uniform vec4 MotionParams0; // x: enabled, y: shutter strength, z: min velocity (pixels), w: depth threshold ratio
layout(location=7) uniform vec4 MotionParams1; // x: max blur radius (pixels), y: max samples, z: velocity texture available, w: reserved
layout(location=8) uniform vec4 PostFXParams0; // x: vignette strength, y: inner radius, z: outer radius, w: falloff
layout(location=9) uniform vec4 PostFXParams1; // xyz: vignette color, w: blend mode
layout(location=10) uniform vec4 PostFXParams2; // x: vignette noise amount, y: chromatic aberration (pixels), zw: reserved
layout(location=11) uniform vec4 FilmGrainParams; // x: intensity, y: grain size (px), z: strength, w: unused
layout(location=12) uniform vec4 FilmGrainOffset; // xy: temporal offsets, zw: unused
layout(location=13) uniform vec4 SSAOParams0; // x: enabled, y: radius (px), z: bias, w: intensity
layout(location=14) uniform vec4 SSAOParams1; // x: samples, y: power, zw: reserved
layout(location=15) uniform mat4 ProjectionMatrix;
layout(location=16) uniform vec4 GodRayParams0; // x: enabled, y: intensity, z: decay, w: weight
layout(location=17) uniform vec4 GodRayParams1; // x: light x, y: light y, z: threshold, w: density
layout(location=18) uniform vec4 GodRayParams2; // x: samples, y: threshold softness, zw: unused
layout(location=19) uniform mat4 InverseProjectionMatrix;

#include "postprocess_common.glsl"
#include "postprocess_ssao.glsl"
#include "postprocess_motion_blur.glsl"
#include "postprocess_dof.glsl"
#include "postprocess_vignette.glsl"
#include "postprocess_chromatic.glsl"
#include "postprocess_godrays.glsl"
#include "postprocess_hdr.glsl"

layout(location=0) out vec4 out_fragcolor;

void main()
{
        float gamma = Params.x;
        float contrast = Params.y;
        float scale = Params.z;
        float dither = Params.w;

        ivec2 pixel = ivec2(gl_FragCoord.xy);
        vec4 color = texelFetch(GammaTexture, pixel, 0);
        bool centerOpaque = color.a >= OPAQUE_ALPHA_THRESHOLD;

        vec2 texSize = vec2(textureSize(GammaTexture, 0));
        vec2 invTexSize = vec2(1.0) / max(texSize, vec2(1.0));
        vec2 uv = (vec2(pixel) + 0.5) / texSize;
        vec2 viewMin = ViewRect.xy;
        vec2 viewMax = ViewRect.zw;
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        vec2 invScale = max(DepthParams.xy, vec2(1e-4));
        bool inView = all(greaterThanEqual(uv, viewMin)) && all(lessThanEqual(uv, viewMax));
        DepthSamplingInfo depthInfo = MakeDepthSamplingInfo();

        bool hasVelocityTexture = MotionParams1.z > 0.5;
        vec2 velocity = vec2(0.0);
        float viewModelMask = 0.0;
        if (hasVelocityTexture && inView)
        {
                vec2 velocityUV = clamp((uv - viewMin) * invScale, vec2(0.0), viewSize * invScale);
                vec4 velocitySample = texture(VelocityTexture, velocityUV);
                velocity = velocitySample.xy;
                viewModelMask = velocitySample.z;
        }

        ApplySSAO(color.rgb, uv, invTexSize, viewMin, viewMax, inView, centerOpaque, depthInfo, viewModelMask);
        ApplyMotionBlur(color.rgb, uv, invTexSize, texSize, viewMin, viewMax, inView, centerOpaque, depthInfo,
                velocity, hasVelocityTexture, viewModelMask);
        ApplyDepthOfField(color.rgb, uv, invTexSize, inView, centerOpaque, depthInfo, viewModelMask);
        if (inView)
        {
                ApplyVignette(color.rgb, uv, viewMin, viewMax, texSize);
                ApplyChromaticAberration(color.rgb, uv, invTexSize, viewMin, viewMax);
                ApplyGodRays(color.rgb, uv, inView, viewMin, viewMax);
        }

        out_fragcolor = color;
#if PALETTIZE == 1
        vec2 noiseuv = floor(gl_FragCoord.xy * scale) + 0.5;
        out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
        out_fragcolor.rgb += DITHER_NOISE(noiseuv) * dither;
        out_fragcolor.rgb *= out_fragcolor.rgb;
#endif // PALETTIZE == 1
#if PALETTIZE
        ivec3 clr = ivec3(clamp(out_fragcolor.rgb, 0., 1.) * 127. + 0.5);
        uint remap = Palette[texelFetch(PaletteLUT, clr, 0).x];
        out_fragcolor.rgb = vec3(UnpackRGB8(remap)) * (1./255.);
#else
        vec3 hdrColor = out_fragcolor.rgb;
        out_fragcolor = ApplyHDRAndFilmGrain(hdrColor, uv, contrast, gamma);
#endif // PALETTIZE
}
