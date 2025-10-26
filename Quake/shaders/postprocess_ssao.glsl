#ifndef POSTPROCESS_SSAO_GLSL
#define POSTPROCESS_SSAO_GLSL

const int SSAO_MAX_SAMPLES = 64;

// Generiere hemisphere samples (Ersatz für uniform samples array)
vec3 GetHemisphereSample(int index, int totalSamples) {
        float i = float(index);
        float n = float(totalSamples);
        
        // Fibonacci sphere distribution
        float phi = 3.14159265359 * (3.0 - sqrt(5.0)); // Golden angle
        float y = 1.0 - (i / (n - 1.0)) * 2.0; // Y von 1 bis -1
        float radius = sqrt(1.0 - y * y);
        float theta = phi * i;
        
        float x = cos(theta) * radius;
        float z = sin(theta) * radius;
        
        // Scale sample (mehr samples näher am center)
        float scale = i / n;
        scale = mix(0.1, 1.0, scale * scale);
        
        return vec3(x, y, z) * scale;
}

// Generiere noise basierend auf screen position
vec3 GetNoiseVector(vec2 fragCoord) {
        const int noiseSize = 4;
        int noiseX = int(fragCoord.x) % noiseSize;
        int noiseY = int(fragCoord.y) % noiseSize;
        
        // Pseudo-random basierend auf position
        float seed = float(noiseX + noiseY * noiseSize);
        float angle = fract(sin(seed * 78.233) * 43758.5453) * 6.283185307;
        
        return normalize(vec3(
                cos(angle),
                sin(angle),
                fract(sin(seed * 12.9898) * 43758.5453) * 2.0 - 1.0
        ));
}

// Rekonstruiere view-space normale aus depth buffer
vec3 ReconstructNormal(vec2 fragCoord, float centerDepth, DepthSamplingInfo depthInfo) {
        float depthL = SampleLinearDepth(fragCoord + vec2(-1.0, 0.0), depthInfo);
        float depthR = SampleLinearDepth(fragCoord + vec2(1.0, 0.0), depthInfo);
        float depthD = SampleLinearDepth(fragCoord + vec2(0.0, -1.0), depthInfo);
        float depthU = SampleLinearDepth(fragCoord + vec2(0.0, 1.0), depthInfo);
        
        // Fallback zu center depth wenn ungültig
        if (depthL <= 0.0) depthL = centerDepth;
        if (depthR <= 0.0) depthR = centerDepth;
        if (depthD <= 0.0) depthD = centerDepth;
        if (depthU <= 0.0) depthU = centerDepth;
        
        // Depth gradients
        vec3 dx = vec3(2.0, 0.0, depthR - depthL);
        vec3 dy = vec3(0.0, 2.0, depthU - depthD);
        
        return normalize(cross(dx, dy));
}

// Rekonstruiere view-space position aus depth
vec3 ReconstructViewPosition(vec2 texCoord, float depth, vec2 invTexSize) {
        // NDC coordinates
        vec2 ndc = texCoord * 2.0 - 1.0;
        
        // Simple perspective reconstruction
        // Annahme: FOV ~90 degrees, aspect ratio aus texSize
        float aspect = 1.0 / invTexSize.y * invTexSize.x;
        
        return vec3(ndc.x * depth * aspect, ndc.y * depth, -depth);
}

void ApplySSAO(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax,
        bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        // Early exit checks
        if (!(SSAOParams0.x > 0.5 && inView && depthInfo.valid && centerOpaque && viewModelMask < 0.5))
                return;
        
        // Parameters (matching reference shader)
        float radius = max(SSAOParams0.y, 0.0) * 0.01; // Convert to world scale
        float bias = max(SSAOParams0.z, 0.005);
        float magnitude = max(SSAOParams0.w, 1.0);
        int sampleCount = clamp(int(SSAOParams1.x + 0.5), 1, SSAO_MAX_SAMPLES);
        float contrast = max(SSAOParams1.y, 1.0);
        
        if (!(radius > 0.0 && sampleCount > 0))
                return;
        
        vec2 fragCoord = gl_FragCoord.xy;
        vec2 texCoord = fragCoord * invTexSize;
        
        // Get center depth
        float centerDepth = SampleLinearDepth(fragCoord, depthInfo);
        if (centerDepth <= 0.0)
                return;
        
        // Reconstruct geometry
        vec3 position = ReconstructViewPosition(texCoord, centerDepth, invTexSize);
        vec3 normal = ReconstructNormal(fragCoord, centerDepth, depthInfo);
        
        // Build TBN matrix
        vec3 randomVec = GetNoiseVector(fragCoord);
        vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
        vec3 binormal = cross(normal, tangent);
        mat3 tbn = mat3(tangent, binormal, normal);
        
        // Sample occlusion
        float occlusion = float(sampleCount);
        
        for (int i = 0; i < sampleCount; i++)
        {
                // Get hemisphere sample
                vec3 sampleVec = GetHemisphereSample(i, sampleCount);
                vec3 samplePosition = tbn * sampleVec;
                samplePosition = position + samplePosition * radius;
                
                // Project sample position to screen space
                vec2 sampleUV = samplePosition.xy / -samplePosition.z; // Simple projection
                sampleUV = sampleUV * 0.5 + 0.5;
                
                // Bounds check
                if (any(lessThan(sampleUV, viewMin)) || any(greaterThan(sampleUV, viewMax)))
                        continue;
                
                // Get sample depth
                vec2 sampleFragCoord = sampleUV / invTexSize;
                float sampleDepth = SampleLinearDepth(sampleFragCoord, depthInfo);
                if (sampleDepth <= 0.0)
                        continue;
                
                vec3 samplePos = ReconstructViewPosition(sampleUV, sampleDepth, invTexSize);
                
                // Check occlusion (using Y-axis like reference)
                float occluded = 0.0;
                if (samplePosition.y + bias <= samplePos.y) {
                        occluded = 0.0;
                } else {
                        occluded = 1.0;
                }
                
                // Range check with smoothstep
                float rangeCheck = smoothstep(0.0, 1.0, radius / (abs(position.y - samplePos.y) + 0.001));
                
                occluded *= rangeCheck;
                occlusion -= occluded;
        }
        
        // Normalize occlusion
        occlusion /= float(sampleCount);
        
        // Apply magnitude (power)
        occlusion = pow(clamp(occlusion, 0.0, 1.0), magnitude);
        
        // Apply contrast
        occlusion = contrast * (occlusion - 0.5) + 0.5;
        occlusion = clamp(occlusion, 0.0, 1.0);
        
        // Apply to color
        color *= occlusion;
}

#endif // POSTPROCESS_SSAO_GLSL