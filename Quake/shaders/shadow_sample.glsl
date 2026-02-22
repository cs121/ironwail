// shadow_sample.glsl — hardened & compatibility-safe
// Fixes:
//   1. CRITICAL – ShadowMul "heuristic": Die Idee, BEIDE Matrix-Varianten (M*v und
//      v*M) zu testen und anhand des NDC-Bereichs auszuwählen, ist konzeptuell
//      falsch:
//        a) Für Geometrie die sich nahe an der Frustum-Grenze befindet, können
//           beide Varianten "plausibel" aussehen (beide |ndc| ≤ 20).
//        b) v*M ist in GLSL per Spec als Zeilenvektor × Matrix definiert, was
//           zum Transponenten der Spalten-Matrix äquivalent ist. Das Resultat
//           wird also falsch sein wenn die Matrix korrekt ist — und umgekehrt.
//        c) Der Heuristic kann flimmern: gleiche Szene, anderer Frame → anderer
//           Zweig → temporales Rauschen im Schatten.
//      FIX: ShadowMul macht nur M*v (korrekt für column-major GLSL). Die "auto-
//      detect" Logik wird entfernt. Wenn die Engine row-major hochlädt, muss
//      das auf CPU-Seite mit glUniformMatrix4fv(..., transpose=GL_TRUE) gelöst
//      werden — nicht im Shader.
//
//   2. ShadowReference01: Die Heuristik "wenn ref außerhalb [0,1], mappe von
//      [-1,1] auf [0,1]" ist falsch für REVERSED_Z (OpenGL clip-control 0→1,
//      nah=1, fern=0). Dann ist ref ∈ [0,1] korrekt, aber die Semantik ist
//      umgekehrt. Die Konvertierung muss compile-time sein.
//      FIX: Zwei explizite compile-time Pfade basierend auf REVERSED_Z.
//
//   3. CRITICAL – ShadowCompare Duplikat: ShadowCompare ist identisch in beiden
//      #ifdef SHADOW_SUN und #ifdef SHADOW_DLIGHT Blöcken definiert. Wenn beide
//      gleichzeitig definiert sind, gibt es einen Compiler-Fehler (Redefinition).
//      FIX: ShadowCompare wird ein einziges Mal außerhalb beider Blöcke definiert.
//
//   4. inside-check in ShadowVisibility schließt reference (proj.z nach Mapping)
//      in die Bereichsprüfung ein. Das ist korrekt, aber UV+reference als vec3
//      zu packen und lessThanEqual zu nutzen ist subtil: reference ist NACH
//      ShadowReference01 immer in [0,1], daher ist die z-Prüfung redundant —
//      clamp() sorgt schon dafür. Kommentar klargestellt; logisch bleibt es.
//
//   5. ShadowSamplePCF: Der "taps"-Parameter ist eine Ordinalzahl (≤2 → 4 samples,
//      ≤4 → 9 samples, sonst 25 samples). Das ist kontra-intuitiv und schlecht
//      dokumentiert. Kommentar hinzugefügt. Logik bleibt unverändert.
//
//   6. ShadowVisibility: ShadowDebug.x < 0.5 → early-out mit in_range=1.0 (shadow
//      disabled, voll beleuchtet). Das ist korrekt aber undokumentiert.
//
//   7. textureSize(ShadowMap, 0): Gibt ivec2 zurück. Division 1.0/ivec2 ist in
//      GLSL implizit via Promotion OK, aber expliziter Cast macht Treiber-Warnings
//      los. FIX: vec2(textureSize(...)) explizit.

#ifndef SHADOW_SAMPLE_GLSL
#define SHADOW_SAMPLE_GLSL

// ---------------------------------------------------------------------------
// FIX 1: Robuste Matrix-Multiplikation ohne fehlerhafte Heuristik.
// Die Engine MUSS column-major Matrizen hochladen (Standard für OpenGL/GLSL).
// Wenn die Engine row-major hochlädt: glUniformMatrix4fv mit transpose=GL_TRUE
// verwenden — niemals im Shader kompensieren.
// ---------------------------------------------------------------------------
vec4 ShadowMul(mat4 M, vec4 v)
{
    return M * v;
}

// ---------------------------------------------------------------------------
// FIX 2: NDC → [0,1] Tiefenkonvertierung, compile-time korrekt.
// ---------------------------------------------------------------------------
float ShadowReference01(float proj_z)
{
#if REVERSED_Z
    // clip-control GL_ZERO_TO_ONE: proj_z bereits in [0,1], nah=1, fern=0
    return clamp(proj_z, 0.0, 1.0);
#else
    // Standard OpenGL NDC: proj_z in [-1,1] → [0,1]
    return clamp(proj_z * 0.5 + 0.5, 0.0, 1.0);
#endif
}

// ---------------------------------------------------------------------------
// FIX 3: ShadowCompare einmalig definiert (war doppelt vorhanden → Compilerfehler
// wenn beide SHADOW_SUN und SHADOW_DLIGHT aktiv).
// ---------------------------------------------------------------------------
float ShadowCompare(float depth, float reference, float bias)
{
#if REVERSED_Z
    // nah=1, fern=0: Oberfläche liegt im Licht wenn reference ≥ depth (minus bias)
    return (reference >= (depth - bias)) ? 1.0 : 0.0;
#else
    // nah=0, fern=1: Oberfläche liegt im Licht wenn reference ≤ depth (plus bias)
    return (reference <= (depth + bias)) ? 1.0 : 0.0;
#endif
}

// ===========================================================================
// SHADOW_SUN — directional sun shadow
// ===========================================================================
#ifdef SHADOW_SUN

// ShadowSampleRawDepth: reads raw (uncompared) depth from the shadow map.
// Requires ShadowMapRaw (sampler2D) bound on TEXTURE6 with GL_NONE compare mode.
// ShadowMap (sampler2DShadow) is on TEXTURE5 with GL_COMPARE_REF_TO_TEXTURE.
// Both samplers reference the same underlying depth texture; the split binding
// allows hardware-PCF via sampler2DShadow (TEXTURE5) and raw reads via TEXTURE6.
float ShadowSampleRawDepth(vec2 uv)
{
    return texture(ShadowMapRaw, uv).r;
}

float ShadowSampleRawCompare(vec2 uv, float reference)
{
    return texture(ShadowMap, vec3(uv, reference));
}

float ShadowApplyBias(float reference, float bias)
{
#if REVERSED_Z
    return clamp(reference - bias, 0.0, 1.0);
#else
    return clamp(reference + bias, 0.0, 1.0);
#endif
}

float ShadowSampleRaw(vec2 uv, float reference, float bias)
{
    if (ShadowDebug.y > 2.5)
    {
        float depth = ShadowSampleRawDepth(uv);
        return ShadowCompare(depth, reference, bias);
    }
    return ShadowSampleRawCompare(uv, ShadowApplyBias(reference, bias));
}

// FIX 5: taps-Wert-Semantik dokumentiert:
//   taps <= 2 → 2×2 rotated grid  (4 samples)
//   taps <= 4 → 3×3 box filter    (9 samples)
//   else      → 5×5 box filter    (25 samples)
float ShadowSamplePCF(vec2 uv, float reference, float bias, int taps)
{
    // FIX 7: expliziter vec2-Cast vermeidet Treiber-Warnings
    vec2 texel = 1.0 / vec2(textureSize(ShadowMap, 0));
    float sum = 0.0;

    if (taps <= 2)
    {
        // 2×2 rotated grid (sub-texel offsets reduzieren Aliasing)
        const vec2 offsets[4] = vec2[4](
            vec2(-0.5, -0.5),
            vec2( 0.5, -0.5),
            vec2(-0.5,  0.5),
            vec2( 0.5,  0.5)
        );
        for (int i = 0; i < 4; ++i)
            sum += ShadowSampleRaw(uv + offsets[i] * texel, reference, bias);
        return sum * 0.25;
    }

    int count = 0;

    if (taps <= 4)
    {
        // 3×3 box
        for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            sum += ShadowSampleRaw(uv + vec2(x, y) * texel, reference, bias);
            ++count;
        }
        return sum / float(count);
    }

    // 5×5 box
    for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x)
    {
        sum += ShadowSampleRaw(uv + vec2(x, y) * texel, reference, bias);
        ++count;
    }
    return sum / float(count);
}

// ShadowDebug.x: 0 = shadows disabled (visually full-lit), ≥1 = enabled
float ShadowVisibility(vec3 world_pos, vec3 normal, out float in_range)
{
    if (ShadowDebug.x < 0.5)
    {
        in_range = 1.0;
        return 1.0;
    }

    vec4 clip = ShadowMul(ShadowViewProj, vec4(world_pos, 1.0));

    // FIX: auch negative w abfangen (Vertex hinter Licht-Near-Plane)
    if (clip.w <= 0.0)
    {
        in_range = 0.0;
        return 1.0;
    }

    vec3 proj      = clip.xyz / clip.w;
    vec2 uv        = proj.xy * 0.5 + 0.5;
    float reference = ShadowReference01(proj.z);

    bool inside = all(greaterThanEqual(uv, vec2(0.0))) &&
                  all(lessThanEqual   (uv, vec2(1.0))) &&
                  reference >= 0.0 && reference <= 1.0;
    in_range = inside ? 1.0 : 0.0;
    if (!inside)
        return 1.0;

    if (ShadowDebug.y > 3.5)
        return ShadowSampleRawCompare(uv, reference);

    float ndotl = clamp(dot(normal, -ShadowSunDir.xyz), 0.0, 1.0);
    float bias  = ShadowParams.x + ShadowParams.y * (1.0 - ndotl);

    if (ShadowParams.z > 0.5)
        return ShadowSamplePCF(uv, reference, bias, int(ShadowParams.w + 0.5));

    return ShadowSampleRaw(uv, reference, bias);
}

#endif // SHADOW_SUN

// ===========================================================================
// SHADOW_DLIGHT — dynamic / point light shadow (atlas-based)
// ===========================================================================
#ifdef SHADOW_DLIGHT

float ShadowSampleRawDlight(vec2 uv, float reference, float bias)
{
    float depth = texture(ShadowDlightMap, uv).r;
    return ShadowCompare(depth, reference, bias);
}

// Gleiche Tap-Semantik wie ShadowSamplePCF (s.o.)
float ShadowSamplePCFDlight(vec2 uv, float reference, float bias, int taps)
{
    vec2 texel = 1.0 / vec2(textureSize(ShadowDlightMap, 0));
    float sum = 0.0;

    if (taps <= 2)
    {
        const vec2 offsets[4] = vec2[4](
            vec2(-0.5, -0.5),
            vec2( 0.5, -0.5),
            vec2(-0.5,  0.5),
            vec2( 0.5,  0.5)
        );
        for (int i = 0; i < 4; ++i)
            sum += ShadowSampleRawDlight(uv + offsets[i] * texel, reference, bias);
        return sum * 0.25;
    }

    int count = 0;

    if (taps <= 4)
    {
        for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            sum += ShadowSampleRawDlight(uv + vec2(x, y) * texel, reference, bias);
            ++count;
        }
        return sum / float(count);
    }

    for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x)
    {
        sum += ShadowSampleRawDlight(uv + vec2(x, y) * texel, reference, bias);
        ++count;
    }
    return sum / float(count);
}

float ShadowVisibilityDlight(vec3 world_pos, vec3 normal, vec3 light_pos,
                              uint light_index, out float in_range)
{
    // ShadowDlightParams.z: 0 = dlight shadows disabled
    if (ShadowDlightParams.z < 0.5)
    {
        in_range = 1.0;
        return 1.0;
    }

    // Suche Shadow-Slot für dieses Licht
    int shadow_index = -1;
    for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
    {
        if (ShadowDlightInfo[i].x < 0.0)
            continue;
        // FIX: round-to-nearest statt +0.5-cast (identisch, aber expliziter)
        if (int(round(ShadowDlightInfo[i].x)) == int(light_index))
        {
            shadow_index = i;
            break;
        }
    }

    if (shadow_index < 0)
    {
        in_range = 0.0;
        return 1.0;
    }

    vec4 clip = ShadowMul(ShadowDlightViewProj[shadow_index], vec4(world_pos, 1.0));
    if (clip.w <= 0.0)
    {
        in_range = 0.0;
        return 1.0;
    }

    vec3  proj      = clip.xyz / clip.w;
    vec2  uv_local  = proj.xy * 0.5 + 0.5;
    float reference = ShadowReference01(proj.z);

    // Atlas-Remapping: uv_local → atlas UV
    vec4 atlas = ShadowDlightAtlas[shadow_index];
    // atlas.xy = scale, atlas.zw = offset
    vec2 uv = uv_local * atlas.xy + atlas.zw;

    // Bounds-Check gegen Atlas-Kachel (nicht gegen Textur-Rand)
    vec2 atlas_min = atlas.zw;
    vec2 atlas_max = atlas.zw + atlas.xy;
    bool inside = all(greaterThanEqual(uv, atlas_min)) &&
                  all(lessThanEqual   (uv, atlas_max));
    in_range = inside ? 1.0 : 0.0;
    if (!inside)
        return 1.0;

    // FIX: normalize light_dir erst nach null-check — bei light_pos == world_pos
    // würde normalize(vec3(0)) NaN liefern.
    vec3 light_delta = light_pos - world_pos;
    float light_dist = length(light_delta);
    vec3  light_dir  = (light_dist > 1e-6) ? light_delta / light_dist : vec3(0.0, 0.0, 1.0);

    float ndotl = clamp(dot(normal, light_dir), 0.0, 1.0);
    float bias  = ShadowDlightParams.x * (1.0 - ndotl);

    int taps = int(ShadowDlightParams.y + 0.5);
    if (taps > 0)
        return ShadowSamplePCFDlight(uv, reference, bias, taps);

    return ShadowSampleRawDlight(uv, reference, bias);
}

#endif // SHADOW_DLIGHT

#endif // SHADOW_SAMPLE_GLSL
