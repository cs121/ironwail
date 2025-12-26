layout(binding=0) uniform sampler2D SSAOTexture;
layout(binding=1) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4 u_params0; // xy: inv resolution, zw: direction
layout(location=1) uniform vec4 u_depthParams; // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=2) uniform vec4 u_params1; // x: sigma, y: radius, z: depth threshold scale, w: unused
layout(location=3) uniform vec4 u_viewRect; // xy: view min, zw: view max
layout(location=4) uniform mat4 u_invProj;
layout(location=5) uniform int u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc

layout(location=0) out vec4 outColor;

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

vec3 ReconstructViewPos(vec2 uv, float depth)
{
        float ndcDepth = DepthToNdcZ(depth, u_depthParams.z, u_reversedZMode);
        vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
        vec4 view = u_invProj * clip;
        return view.xyz / max(view.w, 1e-6);
}

float GetViewZ(vec2 uv, float depth)
{
        vec3 view = ReconstructViewPos(uv, depth);
        return -view.z;
}

bool IsSkyDepth(float depth, vec4 depthParams)
{
        float reversed = depthParams.z;
        float cutoff = depthParams.w;
        if (reversed > 0.5)
                return depth <= cutoff;
        return depth >= cutoff;
}

float Gaussian(float x, float sigma)
{
        float denom = max(2.0 * sigma * sigma, 1e-6);
        return exp(-x * x / denom);
}

void main()
{
        vec2 invResolution = u_params0.xy;
        vec2 uv = (gl_FragCoord.xy + 0.5) * invResolution;
        if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
        {
                outColor = vec4(1.0);
                return;
        }

        float centerDepthRaw = texture(DepthTexture, uv).r;
        if (IsSkyDepth(centerDepthRaw, u_depthParams))
        {
                outColor = vec4(1.0);
                return;
        }

        float centerDepth = GetViewZ(uv, centerDepthRaw);
        float sigma = max(u_params1.x, 0.01);
        int radius = int(u_params1.y + 0.5);
        float depthThreshold = max(u_params1.z, 0.0) * max(centerDepth, 1e-4);

        float total = 0.0;
        float accum = 0.0;
        vec2 direction = u_params0.zw;

        for (int i = -4; i <= 4; ++i)
        {
                if (abs(i) > radius)
                        continue;
                vec2 offset = direction * (float(i) * invResolution);
                vec2 sampleUV = clamp(uv + offset, u_viewRect.xy, u_viewRect.zw);
                float sampleDepthRaw = texture(DepthTexture, sampleUV).r;
                if (IsSkyDepth(sampleDepthRaw, u_depthParams))
                        continue;
                float sampleDepth = GetViewZ(sampleUV, sampleDepthRaw);
                float depthDiff = abs(sampleDepth - centerDepth);
                float depthWeight = smoothstep(0.0, 1.0, depthThreshold / max(depthDiff, 1e-4));
                float weight = Gaussian(float(i), sigma) * depthWeight;
                accum += texture(SSAOTexture, sampleUV).r * weight;
                total += weight;
        }

        float ao = (total > 0.0) ? (accum / total) : 1.0;
        outColor = vec4(ao, ao, ao, 1.0);
}
