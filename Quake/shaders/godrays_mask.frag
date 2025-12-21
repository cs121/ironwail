layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D DepthTexture;

layout(location=0) uniform vec4 MaskParams; // x: threshold, y: sky softness (px), z: light sharpness, w: reversed z flag
layout(location=1) uniform vec4 DownsampleParams; // xy: source size, zw: scale from target to source
layout(location=2) uniform vec4 SkyParams; // x: depth cutoff, yzw: unused

layout(location=0) out vec4 outColor;

vec3 FetchSceneColor(ivec2 coord, vec2 maxCoord)
{
        vec2 clamped = clamp(vec2(coord), vec2(0.0), maxCoord);
        return texelFetch(SceneTexture, ivec2(clamped), 0).rgb;
}

float FetchDepth(ivec2 coord, vec2 maxCoord)
{
        vec2 clamped = clamp(vec2(coord), vec2(0.0), maxCoord);
        return texelFetch(DepthTexture, ivec2(clamped), 0).r;
}

vec2 SampleMaskAt(vec2 targetCoord)
{
        float threshold = MaskParams.x;
        float reversedZ = MaskParams.w;
        float skyDepthCutoff = SkyParams.x;
        vec2 sourceSize = DownsampleParams.xy;
        vec2 scale = DownsampleParams.zw;
        vec2 base = (targetCoord + 0.5) * scale - 0.5;
        vec2 maxCoord = max(sourceSize - vec2(1.0), vec2(0.0));
        ivec2 baseCoord = ivec2(floor(base));
        vec3 accum = vec3(0.0);
        float depthAccum = 0.0;
        for (int j = 0; j < 2; ++j)
        {
                for (int i = 0; i < 2; ++i)
                {
                        ivec2 sampleCoord = baseCoord + ivec2(i, j);
                        accum += FetchSceneColor(sampleCoord, maxCoord);
                        depthAccum += FetchDepth(sampleCoord, maxCoord);
                }
        }
        vec3 color = accum * 0.25;
        float depth = depthAccum * 0.25;
        float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float diff = max(brightness - threshold, 0.0);
        float factor = brightness > 0.0 ? diff / brightness : 0.0;
        bool isSky = (reversedZ > 0.5) ? (depth <= skyDepthCutoff) : (depth >= skyDepthCutoff);
        float skyMask = isSky ? factor : 0.0;
        float lightMask = isSky ? 0.0 : factor;
        return vec2(skyMask, lightMask);
}

void main()
{
        float softness = max(MaskParams.y, 0.0);
        float sharpness = MaskParams.z;
        vec2 center = vec2(gl_FragCoord.xy);
        vec2 centerMask = SampleMaskAt(center);
        float skyMask = centerMask.x;
        if (softness > 0.0)
        {
                vec2 offset = vec2(softness, 0.0);
                vec2 up = SampleMaskAt(center + vec2(0.0, offset.x));
                vec2 down = SampleMaskAt(center - vec2(0.0, offset.x));
                vec2 left = SampleMaskAt(center - vec2(offset.x, 0.0));
                vec2 right = SampleMaskAt(center + vec2(offset.x, 0.0));
                skyMask = (centerMask.x + up.x + down.x + left.x + right.x) * 0.2;
        }

        float lightMask = centerMask.y;
        if (sharpness > 0.0)
                lightMask = pow(lightMask, sharpness);

        float mask = skyMask + lightMask;
        outColor = vec4(mask, mask, mask, 1.0);
}
