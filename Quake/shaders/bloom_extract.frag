layout(binding=0) uniform sampler2D SceneTexture;
layout(location=0) uniform vec4 ThresholdParams; // x: threshold, y: soft knee
layout(location=1) uniform vec4 DownsampleParams; // xy: source size, zw: scale from target to source
layout(location=0) out vec4 outColor;

void main()
{
    float threshold = ThresholdParams.x;
    float knee = ThresholdParams.y;
    vec2 sourceSize = DownsampleParams.xy;
    vec2 scale = DownsampleParams.zw;
    
    // Präzisere Berechnung der Basis-Koordinate
    vec2 base = (gl_FragCoord.xy + 0.5) * scale - 0.5;
    vec2 maxCoord = sourceSize - vec2(1.0);
    
    // Bilineare Interpolation für bessere Qualität
    vec2 f = fract(base);
    ivec2 baseCoord = ivec2(floor(base));
    
    // Optimierte 2x2 Box-Filter mit bilinearer Interpolation
    vec3 accum = vec3(0.0);
    float weightSum = 0.0;
    
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            ivec2 sampleCoord = clamp(baseCoord + ivec2(i, j), ivec2(0), ivec2(maxCoord));
            vec2 w2d = vec2(1.0 - abs(float(i) - f.x), 1.0 - abs(float(j) - f.y));
            float weight = w2d.x * w2d.y;
            
            accum += texelFetch(SceneTexture, sampleCoord, 0).rgb * weight;
            weightSum += weight;
        }
    }
    
    vec3 color = accum / max(weightSum, 0.0001);
    
    // Luminanz-Berechnung (Rec. 709)
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    
    // Soft Threshold mit Knee für weicheren Übergang
    float softThreshold = threshold - knee;
    float hardThreshold = threshold + knee;
    
    float response;
    if (brightness < softThreshold)
    {
        response = 0.0;
    }
    else if (brightness < hardThreshold)
    {
        // Quadratische Kurve im Knee-Bereich
        float t = brightness - softThreshold;
        response = t * t / (4.0 * knee + 0.0001);
    }
    else
    {
        response = brightness - threshold;
    }
    
    // Sicherer Faktor-Berechnung mit Epsilon
    float factor = brightness > 0.0001 ? response / brightness : 0.0;
    color *= factor;
    
    outColor = vec4(color, 1.0);
}