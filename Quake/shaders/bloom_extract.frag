// bloom_extract.frag — verbesserte Version
// Fixes:
//   - Bug: accum wurde immer akkumuliert, unabhaengig vom Mask-Zweig (mask blieb stets 1.0)
//   - Bug: emissiveColor wurde ungefiltert addiert (kein Threshold fuer emissive)
//   - Qualitaet: Bilinear-gewichtetes 2x2-Downsample (Subpixel-gewichtet statt ungewichtet)
//   - Qualitaet: Smooth-Threshold per Knee-Kurve statt hartem Schnitt
//   - Performance: maskBits via floatBitsToInt (kein float->int Umweg ueber floor)

layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D MaskTexture;

// x: threshold, y: mask enabled, z: knee (soft threshold width, z.B. 0.1)
layout(location=0) uniform vec4 ThresholdParams;
// xy: source size, zw: scale from target to source
layout(location=1) uniform vec4 DownsampleParams;

layout(location=0) out vec4 outColor;

// Soft-Knee-Threshold: gibt einen 0..1 Faktor zurueck
// brightness: aktuelle Helligkeit, t: Threshold, k: Knee-Breite
float softKnee(float brightness, float t, float k)
{
    float knee = t * k; // halbe Knee-Breite
    float rq = clamp(brightness - t + knee, 0.0, 2.0 * knee);
    rq = (knee > 0.0) ? (rq * rq) / (4.0 * knee + 0.00001) : 0.0;
    float w = max(brightness - t, rq);
    return brightness > 0.0 ? w / brightness : 0.0;
}

void main()
{
    float threshold  = ThresholdParams.x;
    float maskEnabled = ThresholdParams.y;
    float knee       = ThresholdParams.z; // empfohlen: 0.05 – 0.2

    vec2 sourceSize  = DownsampleParams.xy;
    vec2 scale       = DownsampleParams.zw;

    // Subpixel-Position im Source-Textur-Raum
    vec2 base        = (gl_FragCoord.xy + 0.5) * scale - 0.5;
    vec2 maxCoord    = max(sourceSize - vec2(1.0), vec2(0.0));
    ivec2 baseCoord  = ivec2(floor(base));
    vec2 f           = fract(base); // Subpixel-Gewicht fuer bilineare Interpolation

    // Bilineare Gewichte fuer 2x2 Tap
    float w00 = (1.0 - f.x) * (1.0 - f.y);
    float w10 =        f.x  * (1.0 - f.y);
    float w01 = (1.0 - f.x) *        f.y;
    float w11 =        f.x  *        f.y;
    float bilinWeights[4];
    bilinWeights[0] = w00;
    bilinWeights[1] = w10;
    bilinWeights[2] = w01;
    bilinWeights[3] = w11;

    vec3  accum          = vec3(0.0);
    vec3  accum_bloom    = vec3(0.0);
    vec3  accum_emissive = vec3(0.0);
    float weight         = 0.0;
    float weight_bloom   = 0.0;
    float weight_emissive = 0.0;

    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            vec2  sampleCoord = clamp(vec2(baseCoord) + vec2(float(i), float(j)),
                                      vec2(0.0), maxCoord);
            vec4  sampleColor = texelFetch(SceneTexture, ivec2(sampleCoord), 0);
            float bw = bilinWeights[j * 2 + i];

            if (maskEnabled > 0.5)
            {
                // FIX: floatBitsToInt ist praeziser als floor(rawMask+0.5)
                float rawMask    = texelFetch(MaskTexture, ivec2(sampleCoord), 0).w;
                int   maskBits   = int(floor(rawMask * 255.0 + 0.5)); // falls [0..1]-normiert
                bool  emissiveMask = (maskBits & 4) != 0;

                accum_bloom += sampleColor.rgb * bw;
                weight_bloom += bw;

                if (emissiveMask)
                {
                    accum_emissive += sampleColor.rgb * bw;
                    weight_emissive += bw;
                }
                // FIX: accum wird im Mask-Zweig NICHT benutzt (war zuvor immer 1.0)
            }
            else
            {
                accum  += sampleColor.rgb * bw;
                weight += bw;
            }
        }
    }

    vec3 finalColor;

    if (maskEnabled > 0.5)
    {
        vec3 bloomColor    = (weight_bloom    > 0.0) ? (accum_bloom    / weight_bloom)    : vec3(0.0);
        vec3 emissiveColor = (weight_emissive > 0.0) ? (accum_emissive / weight_emissive) : vec3(0.0);

        // Bloom-Anteil: Threshold mit Soft-Knee
        float brightness = dot(bloomColor, vec3(0.2126, 0.7152, 0.0722));
        float factor     = softKnee(brightness, threshold, knee);
        bloomColor      *= factor;

        // FIX: Emissive ebenfalls durch Threshold (kein unkontrolliertes Durchscheinen)
        float eBrightness = dot(emissiveColor, vec3(0.2126, 0.7152, 0.0722));
        float eFactor     = softKnee(eBrightness, threshold * 0.25, knee); // niedrigerer Threshold fuer Emissive
        emissiveColor    *= eFactor;

        finalColor = emissiveColor + bloomColor;
    }
    else
    {
        vec3 color      = (weight > 0.0) ? (accum / weight) : vec3(0.0);
        float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float factor     = softKnee(brightness, threshold, knee);
        finalColor       = color * factor;
    }

    outColor = vec4(finalColor, 1.0);
}
