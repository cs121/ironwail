layout(binding=0) uniform sampler2D GodraysSourceTexture;

layout(location=0) uniform vec4 MaskParams; // x: threshold, y: softness (px), z: sharpness, w: unused
layout(location=1) uniform vec4 DownsampleParams; // xy: source size, zw: scale from target to source

layout(location=0) out vec4 outColor;

vec4 FetchSource(ivec2 coord, vec2 maxCoord)
{
        vec2 clamped = clamp(vec2(coord), vec2(0.0), maxCoord);
        return texelFetch(GodraysSourceTexture, ivec2(clamped), 0);
}

vec4 SampleMaskAt(vec2 targetCoord)
{
        float threshold = MaskParams.x;
        vec2 sourceSize = DownsampleParams.xy;
        vec2 scale = DownsampleParams.zw;
        vec2 base = (targetCoord + 0.5) * scale - 0.5;
        vec2 maxCoord = max(sourceSize - vec2(1.0), vec2(0.0));
        ivec2 baseCoord = ivec2(floor(base));
        vec3 accum_rgb = vec3(0.0);
        float accum_a = 0.0;
        for (int j = 0; j < 2; ++j)
        {
                for (int i = 0; i < 2; ++i)
                {
                        ivec2 sampleCoord = baseCoord + ivec2(i, j);
                        vec4 sourceSample = FetchSource(sampleCoord, maxCoord);
                        accum_rgb += sourceSample.rgb * sourceSample.a;
                        accum_a += sourceSample.a;
                }
        }
        float avg_a = accum_a * 0.25;
        vec3 color = (accum_a > 0.0) ? (accum_rgb / accum_a) : vec3(0.0);
        float mask = max(avg_a - threshold, 0.0);
        return vec4(color, mask);
}

void main()
{
        float softness = max(MaskParams.y, 0.0);
        float sharpness = MaskParams.z;
        vec2 center = vec2(gl_FragCoord.xy);
        vec4 centerMask = SampleMaskAt(center);
        vec3 accum_rgb = centerMask.rgb * centerMask.a;
        float accum_a = centerMask.a;
        float count = 1.0;
        if (softness > 0.0)
        {
                vec2 offset = vec2(softness, 0.0);
                vec4 up = SampleMaskAt(center + vec2(0.0, offset.x));
                vec4 down = SampleMaskAt(center - vec2(0.0, offset.x));
                vec4 left = SampleMaskAt(center - vec2(offset.x, 0.0));
                vec4 right = SampleMaskAt(center + vec2(offset.x, 0.0));
                accum_rgb += up.rgb * up.a;
                accum_rgb += down.rgb * down.a;
                accum_rgb += left.rgb * left.a;
                accum_rgb += right.rgb * right.a;
                accum_a += up.a + down.a + left.a + right.a;
                count = 5.0;
        }

        float mask = accum_a / max(count, 1.0);
        if (sharpness > 0.0)
                mask = pow(mask, sharpness);

        vec3 color = (accum_a > 0.0) ? (accum_rgb / accum_a) : vec3(0.0);
        outColor = vec4(color, mask);
}
