#include "depth_common.glsl"

float AO_Hash12(vec2 p)
{
        vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
}

vec3 AO_ReconstructViewPos(mat4 invProj, vec2 uv, float depth, float reversedZ)
{
        vec4 clip = vec4(uv * 2.0 - 1.0, DepthRawToNdc(depth, reversedZ), 1.0);
        vec4 view = invProj * clip;
        float w = max(abs(view.w), 1e-6);
        return view.xyz / w;
}

vec3 AO_ReconstructNormalFromDepth(sampler2D depthTex, mat4 invProj, ivec2 pixel, vec2 invScreen, float reversedZ)
{
        ivec2 size = textureSize(depthTex, 0);
        ivec2 px = clamp(pixel, ivec2(1), size - ivec2(2));
        float dC = texelFetch(depthTex, px, 0).r;
        float dR = texelFetch(depthTex, px + ivec2(1, 0), 0).r;
        float dU = texelFetch(depthTex, px + ivec2(0, 1), 0).r;
        vec2 uvC = (vec2(px) + 0.5) * invScreen;
        vec2 uvR = (vec2(px + ivec2(1, 0)) + 0.5) * invScreen;
        vec2 uvU = (vec2(px + ivec2(0, 1)) + 0.5) * invScreen;
        vec3 pC = AO_ReconstructViewPos(invProj, uvC, dC, reversedZ);
        vec3 pR = AO_ReconstructViewPos(invProj, uvR, dR, reversedZ);
        vec3 pU = AO_ReconstructViewPos(invProj, uvU, dU, reversedZ);
        vec3 n = normalize(cross(pR - pC, pU - pC));
        if (any(isnan(n)) || any(isinf(n)))
                n = vec3(0.0, 0.0, 1.0);
        return n;
}
