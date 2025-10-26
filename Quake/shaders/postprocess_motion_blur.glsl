#ifndef POSTPROCESS_MOTION_BLUR_GLSL
#define POSTPROCESS_MOTION_BLUR_GLSL

const int MOTION_MAX_SAMPLES = 64;

// Optimierte Depth-Sampling-Funktion mit frühem Exit
bool IsDepthValid(vec2 sampleCoordPx, float centerDepth, float tolerance, DepthSamplingInfo info)
{
    float sampleDepth = SampleLinearDepth(sampleCoordPx, info);
    return abs(sampleDepth - centerDepth) <= tolerance;
}

// Inline-optimierte Sample-Akkumulation
void AccumulateMotionSample(inout vec3 accum, inout float weight, vec2 sampleUV, vec2 sampleCoordPx,
        vec2 viewMin, vec2 viewMax, DepthSamplingInfo info, bool useDepth, float centerDepth,
        float depthThresholdRatio)
{
    // Bounds-Check mit early exit
    if (any(lessThan(sampleUV, viewMin)) || any(greaterThan(sampleUV, viewMax)))
        return;
    
    // Depth-Check vor Texture-Sampling (spart Texture-Lookups)
    if (useDepth)
    {
        float tolerance = depthThresholdRatio * max(centerDepth, 1e-6);
        if (!IsDepthValid(sampleCoordPx, centerDepth, tolerance, info))
            return;
    }
    
    // Texture-Lookup nur wenn alle anderen Checks bestanden
    vec4 sampleColor = texture(GammaTexture, sampleUV);
    if (sampleColor.a < OPAQUE_ALPHA_THRESHOLD)
        return;
    
    accum += sampleColor.rgb;
    weight += 1.0;
}

void ApplyMotionBlur(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 texSize, vec2 viewMin, vec2 viewMax,
        bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, vec2 velocity,
        bool hasVelocityTexture, float viewModelMask)
{
    // Frühe Exits für bessere Performance
    if (MotionParams0.x <= 0.5 || !inView || !hasVelocityTexture || viewModelMask >= 0.5 || !centerOpaque)
        return;
    
    float effectiveShutter = MotionParams0.y;
    if (effectiveShutter <= 0.0)
        return;
    
    // Vorberechnungen
    vec2 velocityPx = velocity * effectiveShutter * texSize;
    float speed = length(velocityPx);
    float minVelocity = max(MotionParams0.z, 0.0);
    
    if (speed <= minVelocity)
        return;
    
    float maxRadius = MotionParams1.x;
    int maxSamples = clamp(int(MotionParams1.y + 0.5), 1, MOTION_MAX_SAMPLES);
    
    if (maxSamples <= 0)
        return;
    
    // Radius-Berechnung optimiert
    maxRadius = (maxRadius > 0.0) ? maxRadius : speed;
    float radius = min(speed, maxRadius);
    
    // Sample-Count-Berechnung
    float radiusNormDenom = max(maxRadius, 1e-3);
    float normalizedRadius = clamp(radius / radiusNormDenom, 0.0, 1.0);
    int sampleCount = clamp(int(normalizedRadius * float(maxSamples) + 0.5), 1, maxSamples);
    
    // Direction nur einmal berechnen
    vec2 direction = (speed > 1e-4) ? (velocityPx / speed) : vec2(0.0);
    
    // Depth-Setup
    bool useDepth = (MotionParams0.w > 0.0) && depthInfo.valid;
    float centerDepth = useDepth ? SampleLinearDepth(gl_FragCoord.xy, depthInfo) : 0.0;
    float depthThresholdRatio = max(MotionParams0.w, 0.0);
    
    // Akkumulation
    vec3 accum = color;
    float weight = 1.0;
    float jitter = SCREEN_SPACE_NOISE();
    float invSampleCount = 1.0 / float(sampleCount);
    
    // Loop-Unrolling für bessere Performance bei wenigen Samples
    for (int i = 1; i <= MOTION_MAX_SAMPLES; ++i)
    {
        if (i > sampleCount)
            break;
        
        // Optimierte t-Berechnung
        float t = (float(i) - 0.5 + jitter) * invSampleCount;
        vec2 offsetPx = direction * (t * radius);
        
        // Frühes Continue für zu kleine Offsets
        if (dot(offsetPx, offsetPx) < 1e-12)
            continue;
        
        vec2 offsetUV = offsetPx * invTexSize;
        
        // Positive und negative Samples
        vec2 sampleUVPos = uv + offsetUV;
        vec2 sampleUVNeg = uv - offsetUV;
        vec2 fragCoordPos = gl_FragCoord.xy + offsetPx;
        vec2 fragCoordNeg = gl_FragCoord.xy - offsetPx;
        
        AccumulateMotionSample(accum, weight, sampleUVPos, fragCoordPos, viewMin, viewMax, depthInfo,
                useDepth, centerDepth, depthThresholdRatio);
        AccumulateMotionSample(accum, weight, sampleUVNeg, fragCoordNeg, viewMin, viewMax, depthInfo,
                useDepth, centerDepth, depthThresholdRatio);
    }
    
    // Finale Normalisierung
    color = accum * (1.0 / weight);
}

#endif // POSTPROCESS_MOTION_BLUR_GLSL