layout(binding=0) uniform sampler2D MaskTexture;

layout(location=0) uniform vec4 LightParams; // xy: light position, z: density, w: weight
layout(location=1) uniform vec4 ScatterParams; // x: decay, y: exposure, z: max radius, w: samples

layout(location=0) out vec4 outColor;

void main()
{
        vec2 texSize = vec2(textureSize(MaskTexture, 0));
        vec2 uv = (gl_FragCoord.xy + 0.5) / max(texSize, vec2(1.0));
        vec2 lightPos = clamp(LightParams.xy, vec2(0.0), vec2(1.0));
        float density = max(LightParams.z, 0.0);
        float weight = max(LightParams.w, 0.0);
        float decay = max(ScatterParams.x, 0.0);
        float exposure = max(ScatterParams.y, 0.0);
        float maxRadius = max(ScatterParams.z, 0.0);
        int samples = int(ScatterParams.w + 0.5);
        samples = clamp(samples, 1, 128);

        vec2 delta = uv - lightPos;
        float dist = length(delta);
        if (maxRadius > 0.0 && dist > maxRadius)
                delta *= maxRadius / max(dist, 1e-4);
        vec2 step = delta * (density / float(samples));

        vec2 coord = uv;
        vec3 accum = vec3(0.0);
        float illuminationDecay = 1.0;
        for (int i = 0; i < 128; ++i)
        {
                if (i >= samples)
                        break;
                coord -= step;
                coord = clamp(coord, vec2(0.0), vec2(1.0));
                vec4 sampleColor = texture(MaskTexture, coord);
                accum += sampleColor.rgb * sampleColor.a * illuminationDecay * weight;
                illuminationDecay *= decay;
        }

        outColor = vec4(accum * exposure, 1.0);
}
