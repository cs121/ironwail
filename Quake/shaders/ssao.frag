#version 330 core
in vec2 vUV;
out float FragAO; // 0..1 (1 = volle Occlusion oder umgekehrt – je nach Geschmack)

uniform sampler2D uDepth;   // non-linear depth (0..1)
uniform sampler2D uNormal;  // View-Space Normals als RGB, normalisiert
uniform sampler2D uNoise;   // 4x4 Noise (View-Space XY, Z=0), kacheln

uniform mat4 uProj;         // Projection
uniform mat4 uInvProj;      // inverse Projection

// SSAO-Parameter
uniform vec3 uSamples[64];  // Kernel (View-Space, Hemisphäre + wir normieren/skalieren)
uniform int  uKernelSize;   // z.B. 32 oder 64
uniform float uRadius;      // z.B. 0.5..1.5 (Einheiten im View-Space)
uniform float uBias;        // z.B. 0.02..0.1 gegen Acne
uniform vec2 uNoiseScale;   // = ViewportSize / 4.0, wenn Noise 4x4 ist

// Sonstige Tuning-Parameter
uniform float uPower;       // z.B. 1.0..2.0 (Kontrastkurve), 1.2 ist oft gut

// --- Hilfsfunktionen ---
float linearizeDepth(float depth) {
    // Tiefenlinearisierung aus non-linear depth. Nutze Projektion.
    // NDC z = depth * 2 - 1
    float z = depth * 2.0 - 1.0;
    // Rekonstruiere View-Space z über inverse Projection:
    vec4 v = uInvProj * vec4(0.0, 0.0, z, 1.0);
    // v = (x,y,z,w) in View-Space homogen; durch w teilen:
    return v.z / v.w; // Achtung: in View-Space ist z typischerweise negativ vor der Kamera
}

vec3 getViewPos(vec2 uv, float depth) {
    // Rekonstruiert View-Space Position aus Depth + UV
    float z = depth * 2.0 - 1.0;              // NDC z
    vec2 ndc = uv * 2.0 - 1.0;                // NDC xy
    vec4 p = uInvProj * vec4(ndc, z, 1.0);    // View-homogen
    return p.xyz / p.w;                       // View-Space pos
}

void main() {
    // Eingaben lesen
    float depth = texture(uDepth, vUV).r;
    if (depth >= 1.0) { // Hintergrund
        FragAO = 0.0;
        return;
    }

    vec3 N = texture(uNormal, vUV).xyz;
    N = normalize(N);

    vec3 P = getViewPos(vUV, depth);  // aktuelle View-Space Position

    // Rausch-/Rotationsvektor (View-Space, XY)
    vec3 rand = texture(uNoise, vUV * uNoiseScale).xyz;
    rand.z = 0.0;
    rand = normalize(rand);

    // TBN für Hemisphäre
    vec3 T = normalize(rand - N * dot(rand, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;

    float Pz = P.z; // i.d.R. negativ
    for (int i = 0; i < uKernelSize; ++i) {
        // Sample in View-Space um P herum (Hemisphäre entlang N)
        vec3 sampleVS = P + (TBN * uSamples[i]) * uRadius;

        // In Clip/NDC projizieren, um Textur-UV für Tiefenvergleich zu bekommen
        vec4 offsetClip = uProj * vec4(sampleVS, 1.0);
        offsetClip.xyz /= offsetClip.w;
        vec2 offsetUV = offsetClip.xy * 0.5 + 0.5;

        // Bildschirmrand ignorieren
        if (offsetUV.x < 0.0 || offsetUV.x > 1.0 || offsetUV.y < 0.0 || offsetUV.y > 1.0)
            continue;

        // Tiefenvergleich in View-Space
        float sampleDepthNL = texture(uDepth, offsetUV).r;
        if (sampleDepthNL >= 1.0) continue;

        float sampleZ = linearizeDepth(sampleDepthNL); // View-Space z (negativ)
        float rangeWeight = smoothstep(0.0, 1.0, uRadius / abs(Pz - sampleZ));

        // Wenn die tatsächliche Geometrie "vor" unserem Sample liegt (größer in Richtung Kamera),
        // dann ist das Sample verdeckt → Occlusion-Beitrag.
        float occluded = (sampleZ > (sampleVS.z + uBias)) ? 1.0 : 0.0;

        occlusion += occluded * rangeWeight;
    }

    occlusion = occlusion / float(uKernelSize);

    // Statt „rein linear“ ein leichtes Pow für schöneren Kontrast
    occlusion = pow(clamp(occlusion, 0.0, 1.0), uPower);

    // Je nach Compositing: AO als Stärke (0..1). Oft invertiert man später beim Multiplizieren.
    FragAO = occlusion;
}
