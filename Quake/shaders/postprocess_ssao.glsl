#ifndef POSTPROCESS_SSAO_GLSL
#define POSTPROCESS_SSAO_GLSL

const int SSAO_MAX_SAMPLES = 64;
const float SSAO_EPSILON = 1e-6;

vec3 GetHemisphereSample(int index, int totalSamples)
{
        float i = float(index);
        float n = max(float(totalSamples), 1.0);
        float angle = 6.28318530718 * fract(i * 0.61803398875);
        float radius = sqrt((i + 0.5) / n);
        float z = sqrt(max(0.0, 1.0 - radius * radius));
        vec3 hemiSample = vec3(cos(angle) * radius, sin(angle) * radius, z);
        float scale = (i + 0.5) / n;
        scale = mix(0.1, 1.0, scale * scale);
        return hemiSample * scale;
}

bool IsFiniteVec3(vec3 value)
{
        const float kLimit = 1e20;
        vec3 clamped = clamp(value, vec3(-kLimit), vec3(kLimit));
        return all(equal(value, clamped));
}

vec2 NormalizeViewUV(vec2 uv, vec2 viewMin, vec2 viewMax)
{
        vec2 viewSize = max(viewMax - viewMin, vec2(1e-6));
        return clamp((uv - viewMin) / viewSize, vec2(0.0), vec2(1.0));
}

vec2 DenormalizeViewUV(vec2 viewUV, vec2 viewMin, vec2 viewMax)
{
        return viewMin + viewUV * (viewMax - viewMin);
}

bool ReconstructViewPosition(vec2 fragPx, vec2 uv, vec2 viewMin, vec2 viewMax,
        DepthSamplingInfo info, out vec3 position, out float linearDepth)
{
        float rawDepth;
        if (!SampleDepth(fragPx, info, rawDepth, linearDepth))
        {
                position = vec3(-1.0, 0.0, 0.0);
                return false;
        }

        if (rawDepth < 0.0 || rawDepth > 1.0)
        {
                position = vec3(-2.0, 0.0, 0.0);
                return false;
        }

        vec2 viewUV = NormalizeViewUV(uv, viewMin, viewMax);
        
        // KRITISCHER FIX: Probiere reversed-Z depth interpretation
        // Wenn rawDepth groß ist (nahe), muss ndcZ auch groß sein
        float ndcZ;
        if (DoFParams1.z > 0.5)
        {
                // Reversed-Z: [1,0] -> [1,-1]
                ndcZ = rawDepth * 2.0 - 1.0;
        }
        else
        {
                // Standard: [0,1] -> [-1,1], aber für diese Engine invertiert
                ndcZ = 1.0 - rawDepth * 2.0;
        }
        
        vec4 clipPos = vec4(viewUV * 2.0 - 1.0, ndcZ, 1.0);
        vec4 viewPos4 = InverseProjectionMatrix * clipPos;
        
        if (abs(viewPos4.w) < SSAO_EPSILON)
        {
                position = vec3(-3.0, 0.0, 0.0);
                return false;
        }

        // WICHTIGER FIX: Erlaube auch negative w-Werte und nehme Absolutwert
        position = viewPos4.xyz / viewPos4.w;
        
        if (!IsFiniteVec3(position))
        {
                position = vec3(-4.0, 0.0, 0.0);
                return false;
        }
        
        // Entferne die viewPos4.w <= 0.0 Prüfung - sie war zu restriktiv
        
        linearDepth = abs(position.z);  // Absolutwert!
        if (linearDepth <= SSAO_EPSILON || linearDepth != linearDepth || abs(linearDepth) > 1e10)
        {
                position = vec3(-6.0, 0.0, 0.0);
                return false;
        }
        
        return true;
}

vec3 ReconstructNormal(vec2 fragPx, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax,
        DepthSamplingInfo info, vec3 centerPos)
{
        vec2 offsets[4] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
        vec3 samples[4] = vec3[](centerPos, centerPos, centerPos, centerPos);
        int validSamples = 0;
        
        for (int i = 0; i < 4; ++i)
        {
                vec2 sampleFrag = fragPx + offsets[i];
                vec2 sampleUV = uv + offsets[i] * invTexSize;
                vec3 samplePos;
                float sampleDepth;
                if (ReconstructViewPosition(sampleFrag, sampleUV, viewMin, viewMax, info, samplePos, sampleDepth))
                {
                        samples[i] = samplePos;
                        validSamples++;
                }
        }

        if (validSamples < 3)
                return vec3(0.0, 0.0, -1.0);

        vec3 dx = samples[1] - samples[0];
        vec3 dy = samples[3] - samples[2];
        vec3 normal = cross(dx, dy);
        
        float normLength = length(normal);
        if (normLength < SSAO_EPSILON || !IsFiniteVec3(normal))
                return vec3(0.0, 0.0, -1.0);
                
        normal = normal / normLength;
        
        if (dot(normal, vec3(0.0, 0.0, -1.0)) < 0.0)
                normal = -normal;
                
        return normal;
}

vec3 GetNoiseVector(vec2 fragCoord)
{
        float seed = whitenoise01(fragCoord * 0.25);
        float angle = seed * 6.28318530718;
        vec2 rand = vec2(cos(angle), sin(angle));
        return normalize(vec3(rand, 0.0));
}

void ApplySSAO(inout vec3 color, vec2 uv, vec2 invTexSize, vec2 viewMin, vec2 viewMax,
        bool inView, bool centerOpaque, DepthSamplingInfo depthInfo, float viewModelMask)
{
        // ====== DEBUG MODE ======
        // Setze DEBUG_MODE auf 1-6 um verschiedene Informationen anzuzeigen:
        // 1 = Raw Depth
        // 2 = Linear Depth 
        // 3 = Normals
        // 4 = Occlusion nur
        // 5 = viewModelMask
        // 6 = SSAO Bedingungen Check
        const int DEBUG_MODE = 0;  // Teste warum SSAO nicht läuft!
        
        vec2 fragCoord = gl_FragCoord.xy;
        
        if (DEBUG_MODE == 1)
        {
                // Zeige raw depth (OHNE Invertierung)
                float rawDepth, linearDepth;
                if (SampleDepth(fragCoord, depthInfo, rawDepth, linearDepth))
                {
                        color = vec3(rawDepth);
                }
                else
                {
                        color = vec3(1.0, 0.0, 0.0); // Rot = kein Depth
                }
                return;
        }
        
        if (DEBUG_MODE == 2)
        {
                // Zeige linear depth (normalisiert) UND wo es fehlschlägt
                vec3 centerPos;
                float centerDepth;
                if (ReconstructViewPosition(fragCoord, uv, viewMin, viewMax, depthInfo, centerPos, centerDepth))
                {
                        // Normalisiere auf sichtbaren Bereich (z.B. 0-100 units)
                        float normalizedDepth = clamp(centerDepth / 100.0, 0.0, 1.0);
                        color = vec3(normalizedDepth);
                }
                else
                {
                        // Zeige verschiedene Fehler als verschiedene Farben
                        if (centerPos.x < -0.5)
                        {
                                // Fehlercode in centerPos.x gespeichert
                                if (centerPos.x > -1.5) color = vec3(1.0, 0.0, 0.0); // SampleDepth failed
                                else if (centerPos.x > -2.5) color = vec3(0.0, 1.0, 0.0); // rawDepth out of range
                                else if (centerPos.x > -3.5) color = vec3(0.0, 0.0, 1.0); // viewPos4.w too small
                                else if (centerPos.x > -4.5) color = vec3(1.0, 1.0, 0.0); // not finite
                                else if (centerPos.x > -5.5) color = vec3(1.0, 0.0, 1.0); // viewPos4.w negative
                                else color = vec3(0.0, 1.0, 1.0); // linearDepth invalid
                        }
                        else
                        {
                                color = vec3(1.0, 1.0, 1.0); // Unbekannter Fehler
                        }
                }
                return;
        }
        
        if (DEBUG_MODE == 3)
        {
                // Zeige Normals
                vec3 centerPos;
                float centerDepth;
                if (ReconstructViewPosition(fragCoord, uv, viewMin, viewMax, depthInfo, centerPos, centerDepth))
                {
                        vec3 normal = ReconstructNormal(fragCoord, uv, invTexSize, viewMin, viewMax, depthInfo, centerPos);
                        color = normal * 0.5 + 0.5; // Konvertiere von [-1,1] zu [0,1]
                }
                else
                {
                        color = vec3(1.0, 0.0, 0.0);
                }
                return;
        }
        
        if (DEBUG_MODE == 5)
        {
                // Zeige viewModelMask (sollte für Waffe hell sein)
                color = vec3(viewModelMask);
                return;
        }
        
        if (DEBUG_MODE == 6)
        {
                // Zeige alle Bedingungen für SSAO
                bool cond1 = SSAOParams0.x > 0.5;
                bool cond2 = inView;
                bool cond3 = depthInfo.valid;
                bool cond4 = centerOpaque;
                bool cond5 = viewModelMask < 0.5;
                
                if (!cond1) color = vec3(1.0, 0.0, 0.0); // Rot = SSAO disabled
                else if (!cond2) color = vec3(0.0, 1.0, 0.0); // Grün = not in view
                else if (!cond3) color = vec3(0.0, 0.0, 1.0); // Blau = depth invalid
                else if (!cond4) color = vec3(1.0, 1.0, 0.0); // Gelb = not opaque
                else if (!cond5) color = vec3(1.0, 0.0, 1.0); // Magenta = viewmodel
                else color = vec3(1.0, 1.0, 1.0); // Weiß = alle Bedingungen OK
                return;
        }
        // ====== END DEBUG MODE ======
        
        if (!(SSAOParams0.x > 0.5 && inView && depthInfo.valid && centerOpaque && viewModelMask < 0.5))
                return;

        float radius = max(SSAOParams0.y, 0.0) * 0.01;
        float bias = max(SSAOParams0.z, 0.0);
        float magnitude = max(SSAOParams0.w, 1.0);
        int sampleCount = clamp(int(SSAOParams1.x + 0.5), 1, SSAO_MAX_SAMPLES);
        float contrast = max(SSAOParams1.y, 1.0);

        if (!(radius > 0.0 && sampleCount > 0))
                return;

        vec3 centerPos;
        float centerDepth;
        
        if (!ReconstructViewPosition(fragCoord, uv, viewMin, viewMax, depthInfo, centerPos, centerDepth))
                return;

        vec3 normal = ReconstructNormal(fragCoord, uv, invTexSize, viewMin, viewMax, depthInfo, centerPos);
        
        if (length(normal) < SSAO_EPSILON)
                return;

        vec3 randomVec = GetNoiseVector(fragCoord);
        float ndotRandom = dot(randomVec, normal);
        if (abs(ndotRandom) > 0.999)
                randomVec = normalize(vec3(normal.y, normal.z, normal.x));
        vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
        vec3 bitangent = normalize(cross(normal, tangent));
        mat3 tbn = mat3(tangent, bitangent, normal);

        float occlusionAccum = 0.0;
        int validOcclusionSamples = 0;

        for (int i = 0; i < SSAO_MAX_SAMPLES; ++i)
        {
                if (i >= sampleCount)
                        break;

                vec3 sampleVec = GetHemisphereSample(i, sampleCount);
                vec3 samplePos = centerPos + tbn * sampleVec * radius;

                vec4 clip = ProjectionMatrix * vec4(samplePos, 1.0);
                if (clip.w <= SSAO_EPSILON)
                        continue;
                        
                vec3 ndc = clip.xyz / clip.w;
                vec2 viewUV = ndc.xy * 0.5 + 0.5;
                
                if (any(lessThan(viewUV, vec2(0.0))) || any(greaterThan(viewUV, vec2(1.0))))
                        continue;

                vec2 sampleUV = DenormalizeViewUV(viewUV, viewMin, viewMax);
                vec2 sampleFrag = sampleUV / invTexSize;

                vec3 fetchedPos;
                float fetchedDepth;
                if (!ReconstructViewPosition(sampleFrag, sampleUV, viewMin, viewMax, depthInfo, fetchedPos, fetchedDepth))
                        continue;

                vec3 dir = fetchedPos - centerPos;
                float dist = length(dir);
                if (dist < SSAO_EPSILON)
                        continue;
                        
                float ndotDir = dot(normal, dir / dist);
                if (ndotDir <= 0.0)
                        continue;

                float expectedDepth = -samplePos.z;
                float actualDepth = fetchedDepth;
                float depthDelta = expectedDepth - actualDepth - bias;
                
                if (depthDelta <= 0.0)
                        continue;

                float rangeCheck = smoothstep(0.0, 1.0, radius / (abs(centerDepth - actualDepth) + 1e-4));
                occlusionAccum += rangeCheck;
                validOcclusionSamples++;
        }

        if (validOcclusionSamples <= 0)
                return;

        float occlusion = 1.0 - (occlusionAccum / float(validOcclusionSamples));
        occlusion = pow(clamp(occlusion, 0.0, 1.0), magnitude);
        occlusion = contrast * (occlusion - 0.5) + 0.5;
        occlusion = clamp(occlusion, 0.0, 1.0);

        // DEBUG MODE 4: Zeige nur Occlusion
        if (DEBUG_MODE == 4)
        {
                color = vec3(occlusion);
                return;
        }

        color *= occlusion;
}

#endif // POSTPROCESS_SSAO_GLSL