layout(binding=0) uniform sampler2D BloomTexture;
layout(location=0) uniform vec4 BlurParams; // xy: texel size, zw: direction

layout(location=0) out vec4 outColor;

// Optimierte Gaussian-Gewichte für bessere visuelle Qualität
const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 texelSize = BlurParams.xy;
    vec2 direction = BlurParams.zw;
    
    // Präzisere UV-Berechnung
    vec2 uv = gl_FragCoord.xy * texelSize;
    
    // Zentrum-Sample mit Gewicht
    vec3 color = texture(BloomTexture, uv).rgb * weights[0];
    
    // Vorberechnung der Richtung mit texelSize (Optimierung)
    vec2 offsetDir = direction * texelSize;
    
    // Symmetrische Samples (reduziert Texture-Lookups auf 8 statt 9)
    for (int i = 1; i < 5; ++i)
    {
        float offset = float(i);
        vec2 sampleOffset = offsetDir * offset;
        
        // Beide Richtungen gleichzeitig samplen
        color += texture(BloomTexture, uv + sampleOffset).rgb * weights[i];
        color += texture(BloomTexture, uv - sampleOffset).rgb * weights[i];
    }
    
    // Gamma-Korrektur für visuell besseres Bloom (optional)
    // color = pow(color, vec3(1.0 / 2.2));
    
    outColor = vec4(color, 1.0);
}