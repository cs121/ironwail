layout(binding=0) uniform sampler2D SSAOTexture;
layout(binding=1) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4 u_screenParams; // xy: inv screen size, zw: screen size
layout(location=1) uniform vec4 u_aoParams; // xy: inv AO size, zw: AO size
layout(location=2) uniform vec4 u_params0; // xy: blur direction, zw: unused
layout(location=3) uniform vec4 u_depthParams; // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=4) uniform vec4 u_params1; // x: sigma, y: radius, z: depth threshold scale, w: bilateral toggle
layout(location=5) uniform vec4 u_viewRect; // xy: view min, zw: view max
layout(location=6) uniform int u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc
layout(location=7) uniform int u_yFlip; // 0: none, 1: flip Y

layout(location=0) out vec4 outColor;

vec2 ApplyYFlip(vec2 uv)
{
        if (u_yFlip != 0)
                uv.y = 1.0 - uv.y;
        return uv;
}

vec2 ScreenInvSize()
{
        return u_screenParams.xy;
}

vec2 ScreenSize()
{
        return u_screenParams.zw;
}

vec2 AoInvSize()
{
        return u_aoParams.xy;
}

vec2 AoSize()
{
        return u_aoParams.zw;
}

vec2 AoToScreenScale()
{
        return ScreenSize() * AoInvSize();
}

vec2 UnflipUv(vec2 uv)
{
        if (u_yFlip != 0)
                uv.y = 1.0 - uv.y;
        return uv;
}

vec2 AoUvFromPixel(vec2 aoPixel)
{
        return ApplyYFlip((aoPixel + 0.5) * AoInvSize());
}

ivec2 ClampScreenPixel(vec2 pixel)
{
        vec2 maxPx = ScreenSize() - vec2(1.0);
        return ivec2(clamp(pixel, vec2(0.0), maxPx));
}

ivec2 ScreenPixelFromAoPixelNearest(vec2 aoPixel)
{
        vec2 screenSize = ScreenSize();
        vec2 aoSize = max(AoSize(), vec2(1.0));
        vec2 screenPixel = floor(aoPixel * screenSize / aoSize);
        return ClampScreenPixel(screenPixel);
}

vec2 ScreenUvFromAoPixel(vec2 aoPixel)
{
        vec2 scale = AoToScreenScale();
        vec2 screenPixel = aoPixel * scale + vec2(0.5);
        return ApplyYFlip(screenPixel * ScreenInvSize());
}

vec2 ScreenUvFromAoUv(vec2 uv)
{
        vec2 aoPixel = floor(uv * AoSize());
        return ScreenUvFromAoPixel(aoPixel);
}

ivec2 ScreenPixelFromUv(vec2 uv)
{
        vec2 unflipped = UnflipUv(uv);
        vec2 screenPixel = floor(unflipped * ScreenSize());
        return ClampScreenPixel(screenPixel);
}

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

// Centralized SSAO depth conversion. Returns positive view-space depth (+X forward).
float ViewZFromDepth(float depth01, mat4 proj, bool reversedZ)
{
        float ndcDepth = DepthToNdcZ(depth01, reversedZ ? 1.0 : 0.0, u_reversedZMode);
        float nearPlane = u_depthParams.x;
        float farPlane = u_depthParams.y;
        if (reversedZ)
        {
                float denom = nearPlane + ndcDepth * (farPlane - nearPlane);
                return (nearPlane * farPlane) / max(denom, 1e-6);
        }
        float denom = farPlane + nearPlane - ndcDepth * (farPlane - nearPlane);
        return (2.0 * nearPlane * farPlane) / max(denom, 1e-6);
}

float DepthRawFromPixel(ivec2 pixel)
{
        return texelFetch(DepthTexture, pixel, 0).r;
}

float DepthRawFromUv(vec2 uv)
{
        return DepthRawFromPixel(ScreenPixelFromUv(uv));
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
        vec2 aoPixel = floor(gl_FragCoord.xy);
        vec2 invResolution = AoInvSize();
        vec2 uv = AoUvFromPixel(aoPixel);
        if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
        {
                outColor = vec4(1.0);
                return;
        }

        ivec2 screenPixel = ScreenPixelFromAoPixelNearest(aoPixel);
        float centerDepthRaw = DepthRawFromPixel(screenPixel);
        if (IsSkyDepth(centerDepthRaw, u_depthParams))
        {
                outColor = vec4(1.0);
                return;
        }

        float centerDepth = ViewZFromDepth(centerDepthRaw, mat4(1.0), u_depthParams.z > 0.5);
        float sigma = max(u_params1.x, 0.01);
        int radius = int(u_params1.y + 0.5);
        float depthThreshold = max(u_params1.z, 0.0) * max(centerDepth, 1e-4);
        bool useBilateral = (u_params1.w > 0.5);

        float total = 0.0;
        float accum = 0.0;
        vec2 direction = u_params0.xy;

        for (int i = -4; i <= 4; ++i)
        {
                if (abs(i) > radius)
                        continue;
                vec2 offset = direction * (float(i) * invResolution);
                vec2 sampleUV = clamp(uv + offset, u_viewRect.xy, u_viewRect.zw);
                vec2 sampleDepthUv = ScreenUvFromAoUv(sampleUV);
                float sampleDepthRaw = DepthRawFromUv(sampleDepthUv);
                if (IsSkyDepth(sampleDepthRaw, u_depthParams))
                        continue;
                float sampleDepth = ViewZFromDepth(sampleDepthRaw, mat4(1.0), u_depthParams.z > 0.5);
                float depthDiff = abs(sampleDepth - centerDepth);
                float depthWeight = useBilateral ? smoothstep(0.0, 1.0, depthThreshold / max(depthDiff, 1e-4)) : 1.0;
                float weight = Gaussian(float(i), sigma) * depthWeight;
                accum += texture(SSAOTexture, sampleUV).r * weight;
                total += weight;
        }

        float ao = (total > 0.0) ? (accum / total) : 1.0;
        outColor = vec4(ao, ao, ao, 1.0);
}
