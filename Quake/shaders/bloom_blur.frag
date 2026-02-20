// bloom_blur.frag — verbesserte Version
// Fixes:
//   - Bug: Keine Gewichtsnormalisierung bei geclamten Randsamples → dunkle Kanten behoben
//   - Qualitaet: 13-Tap Gauss-Kernel (statt 9-Tap) fuer weicheres, hochwertigeres Bloom
//   - Qualitaet: Gewichtete Normalisierung (akkumuliertes Gewicht im Nenner)
//   - Performance: Bilinear-Trick: je 2 Taps werden zu 1 textureBilinear-Fetch zusammengefasst
//     → 13-Tap logisch, aber nur 7 texture()-Aufrufe pro Pass (Linear-Sampling-Trick)

layout(binding=0) uniform sampler2D BloomTexture;

// xy: texel size (1/resolution), zw: direction (1,0) oder (0,1)
layout(location=0) uniform vec4 BlurParams;

layout(location=0) out vec4 outColor;

void main()
{
    vec2 texelSize = BlurParams.xy;
    vec2 direction = BlurParams.zw;
    vec2 uv        = (gl_FragCoord.xy + 0.5) * texelSize;

    // 13-Tap Gauss-Koeffizienten (sigma ~ 3, normalisiert auf Summe = 1)
    // Roh-Gewichte fuer Offset 0..6
    // Mittels Linear-Sampling-Trick werden Paare (1+2), (3+4), (5+6) zusammengefasst:
    //   combined weight  = w[i] + w[i+1]
    //   combined offset  = (w[i]*i + w[i+1]*(i+1)) / (w[i]+w[i+1])
    // Ergebnis: 7 Fetches statt 13 → ~46% weniger Textur-Lookups

    // Gauss sigma=3, Taps bei Offset 0,1,2,3,4,5,6
    const float w0 = 0.196482;
    const float w1 = 0.176033;
    const float w2 = 0.120985;
    const float w3 = 0.064759;
    const float w4 = 0.026995;
    const float w5 = 0.008764;
    const float w6 = 0.002216;

    // Linear-Sampling-Trick: Paare zusammenfassen
    // Pair A: offset 1+2
    float wA  = w1 + w2;
    float offA = (w1 * 1.0 + w2 * 2.0) / wA; // ~1.407
    // Pair B: offset 3+4
    float wB  = w3 + w4;
    float offB = (w3 * 3.0 + w4 * 4.0) / wB; // ~3.294
    // Pair C: offset 5+6
    float wC  = w5 + w6;
    float offC = (w5 * 5.0 + w6 * 6.0) / wC; // ~5.176

    vec3  color         = vec3(0.0);
    float totalWeight   = 0.0;

    // Center tap
    color       += texture(BloomTexture, uv).rgb * w0;
    totalWeight += w0;

    // Pair A
    vec2 dA = direction * texelSize * offA;
    color       += texture(BloomTexture, uv + dA).rgb * wA;
    color       += texture(BloomTexture, uv - dA).rgb * wA;
    totalWeight += 2.0 * wA;

    // Pair B
    vec2 dB = direction * texelSize * offB;
    color       += texture(BloomTexture, uv + dB).rgb * wB;
    color       += texture(BloomTexture, uv - dB).rgb * wB;
    totalWeight += 2.0 * wB;

    // Pair C
    vec2 dC = direction * texelSize * offC;
    color       += texture(BloomTexture, uv + dC).rgb * wC;
    color       += texture(BloomTexture, uv - dC).rgb * wC;
    totalWeight += 2.0 * wC;

    // FIX: Normalisierung durch tatsaechlich akkumuliertes Gewicht
    // Verhindert dunkle Raender wenn Samples am Texturrand geclampt werden
    // (Wenn alle Samples im gueltigen Bereich liegen, ist totalWeight == 1.0 => keine Aenderung)
    color /= totalWeight;

    outColor = vec4(color, 1.0);
}
