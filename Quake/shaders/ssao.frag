layout(binding=0) uniform sampler2D DepthTexture;
layout(binding=1) uniform sampler2D NoiseTexture;

layout(location=0) uniform mat4 u_proj;
layout(location=1) uniform mat4 u_invProj;
layout(location=2) uniform vec4 u_params0; // x: radius, y: bias, z: power, w: min AO
layout(location=3) uniform vec4 u_params1; // xy: inv resolution, zw: noise scale
layout(location=4) uniform vec4 u_depthParams; // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=5) uniform vec4 u_viewRect; // xy: view min, zw: view max
layout(location=6) uniform int u_samples;
layout(location=7) uniform vec4 u_debugParams; // x: debug mode, y: debug far
layout(location=8) uniform int u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc

layout(location=0) out vec4 outColor;

const int SSAO_MAX_SAMPLES = 32;
const vec3 SSAO_KERNEL[SSAO_MAX_SAMPLES] = vec3[](
        vec3(0.5381, 0.1856, 0.4319),
        vec3(0.1379, 0.2486, 0.4430),
        vec3(0.3371, 0.5679, 0.0057),
        vec3(-0.6999, -0.0451, 0.0019),
        vec3(0.0689, -0.1598, 0.8547),
        vec3(0.0560, 0.0069, 0.1843),
        vec3(-0.0146, 0.1402, 0.0762),
        vec3(0.0100, -0.1924, 0.0344),
        vec3(-0.3577, -0.5301, 0.4358),
        vec3(-0.3169, 0.1063, 0.0158),
        vec3(0.0103, -0.5869, 0.0046),
        vec3(-0.0897, -0.4940, 0.3287),
        vec3(0.7119, -0.0154, 0.0918),
        vec3(-0.0533, 0.0596, 0.5411),
        vec3(0.0352, -0.0631, 0.5460),
        vec3(-0.4776, 0.2847, 0.0271),
        vec3(0.6281, 0.2908, 0.1163),
        vec3(0.3174, -0.1646, 0.5042),
        vec3(-0.2505, 0.4580, 0.0136),
        vec3(0.2067, -0.3752, 0.0631),
        vec3(-0.6681, -0.5057, 0.1395),
        vec3(0.1885, 0.4704, 0.1952),
        vec3(0.4441, 0.1386, 0.1782),
        vec3(-0.0791, 0.3005, 0.4190),
        vec3(-0.1153, 0.5775, 0.1735),
        vec3(0.4251, 0.0601, 0.1049),
        vec3(0.0587, -0.6517, 0.0442),
        vec3(-0.0837, 0.1302, 0.5580),
        vec3(0.1036, 0.0927, 0.1655),
        vec3(-0.0347, -0.3855, 0.3385),
        vec3(-0.3756, 0.4976, 0.0275),
        vec3(0.2397, -0.1794, 0.3984)
);

// SSAO conventions:
// - View space uses +X forward (camera looks down +X).
// - When reverse-Z (clip control) is enabled, NDC depth is [0..1]; otherwise [-1..1].
// - All comparisons are done in view space using +X depth.
float DepthRaw(vec2 uv)
{
        return texture(DepthTexture, uv).r;
}

// Ironwail uses reverse-Z with clip control: near depth ~1, far depth ~0 when reversed is enabled.
float DepthToNdcZ(float depth, float reversed, int mode)
{
        float raw = depth;
        if (mode == 1)
                raw = 1.0 - raw;
        if (reversed > 0.5)
        {
                if (mode == 2)
                        raw = 1.0 - raw;
                return raw;
        }
        float ndc = raw * 2.0 - 1.0;
        if (mode == 2)
                ndc = -ndc;
        return ndc;
}

float LinearizeViewZ(float depth)
{
        float ndcDepth = DepthToNdcZ(depth, u_depthParams.z, u_reversedZMode);
        float nearPlane = u_depthParams.x;
        float farPlane = u_depthParams.y;
        if (u_depthParams.z > 0.5)
        {
                float denom = nearPlane + ndcDepth * (farPlane - nearPlane);
                return (nearPlane * farPlane) / max(denom, 1e-6);
        }
        float denom = farPlane + nearPlane - ndcDepth * (farPlane - nearPlane);
        return (2.0 * nearPlane * farPlane) / max(denom, 1e-6);
}

vec3 ReconstructViewPos(vec2 uv, float depth)
{
        float ndcDepth = DepthToNdcZ(depth, u_depthParams.z, u_reversedZMode);
        vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4 view = u_invProj * clip;
        float w = view.w;
        if (abs(w) < 1e-6)
                return vec3(0.0);
        return view.xyz / w;
}

vec3 ComputeNormalFromViewPos(vec3 viewPos)
{
        vec3 dx = dFdx(viewPos);
        vec3 dy = dFdy(viewPos);
        vec3 normal = normalize(cross(dx, dy));
        if (length(normal) < 1e-4)
                return vec3(0.0, 0.0, 1.0);
        // SSAO FIX: Orient reconstructed normals toward the camera for stable TBN.
        vec3 viewDir = normalize(-viewPos);
        if (dot(normal, viewDir) < 0.0)
                normal = -normal;
        return normal;
}

bool IsSkyDepth(float depth, vec4 depthParams)
{
        float reversed = depthParams.z;
        float cutoff = depthParams.w;
        if (reversed > 0.5)
                return depth <= cutoff;
        return depth >= cutoff;
}

void main()
{
        vec2 invResolution = u_params1.xy;
        vec2 uv = (gl_FragCoord.xy + 0.5) * invResolution;
        int debugMode = -1;
        if (u_debugParams.x >= -0.5)
                debugMode = int(u_debugParams.x + 0.5);
        float debugFar = max(u_debugParams.y, 1e-3);
        if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
        {
                outColor = vec4(1.0);
                return;
        }

        float depth = DepthRaw(uv);
        if (debugMode == 1)
        {
                outColor = vec4(depth, depth, depth, 1.0);
                return;
        }
        if (debugMode == 2)
        {
                if (IsSkyDepth(depth, u_depthParams))
                {
                        outColor = vec4(1.0);
                        return;
                }
                float viewZ = LinearizeViewZ(depth);
                float v = clamp(viewZ / debugFar, 0.0, 1.0);
                outColor = vec4(v, v, v, 1.0);
                return;
        }
        if (debugMode == 3)
        {
                if (IsSkyDepth(depth, u_depthParams))
                {
                        outColor = vec4(1.0);
                        return;
                }
                vec3 viewPos = ReconstructViewPos(uv, depth);
                float v = clamp(length(viewPos) / debugFar, 0.0, 1.0);
                outColor = vec4(v, v, v, 1.0);
                return;
        }
        if (IsSkyDepth(depth, u_depthParams))
        {
                outColor = vec4(1.0);
                return;
        }

        vec3 viewPos = ReconstructViewPos(uv, depth);
        vec3 normal = ComputeNormalFromViewPos(viewPos);
        if (debugMode == 4)
        {
                vec3 debugNormal = normal * 0.5 + 0.5;
                outColor = vec4(debugNormal, 1.0);
                return;
        }

        vec2 noise = texture(NoiseTexture, uv * u_params1.zw).xy * 2.0 - 1.0;
        vec3 tangent = normalize(vec3(noise, 0.0) - normal * dot(vec3(noise, 0.0), normal));
        vec3 bitangent = cross(normal, tangent);
        mat3 tbn = mat3(tangent, bitangent, normal);

        float radius = u_params0.x;
        float bias = u_params0.y;
        float occlusion = 0.0;
        int samples = clamp(u_samples, 1, SSAO_MAX_SAMPLES);

        for (int i = 0; i < SSAO_MAX_SAMPLES; ++i)
        {
                if (i >= samples)
                        break;
                vec3 sampleVec = tbn * SSAO_KERNEL[i];
                vec3 samplePos = viewPos + sampleVec * radius;
                vec4 offset = u_proj * vec4(samplePos, 1.0);
                if (offset.w <= 1e-6)
                        continue;
                vec2 sampleUV = offset.xy / offset.w * 0.5 + 0.5;
                if (!all(greaterThanEqual(sampleUV, u_viewRect.xy)) || !all(lessThanEqual(sampleUV, u_viewRect.zw)))
                        continue;
                float sampleDepth = texture(DepthTexture, sampleUV).r;
                if (IsSkyDepth(sampleDepth, u_depthParams))
                        continue;
                float sampleViewDepth = LinearizeViewZ(sampleDepth);
                float samplePosDepth = samplePos.x;
                float dz = sampleViewDepth - samplePosDepth - bias;
                float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(dz), 1e-4));
                if (dz < 0.0)
                        occlusion += rangeCheck;
        }

        float ao = 1.0 - occlusion / float(samples);
        ao = clamp(ao, 0.0, 1.0);
        ao = pow(ao, u_params0.z);
        ao = max(ao, u_params0.w);
        if (debugMode == 5 || debugMode == 6)
        {
                outColor = vec4(ao, ao, ao, 1.0);
                return;
        }
        outColor = vec4(ao, ao, ao, 1.0);
}
