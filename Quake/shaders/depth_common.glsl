// Shared depth helpers for both normal-Z and reversed-Z paths.
//
// Canonical depth convention used here:
//   0.0 = near plane, 1.0 = far plane (independent of runtime depth mode).

float DepthRawToCanonical(float depth, float reversedZ)
{
        return (reversedZ > 0.5) ? (1.0 - depth) : depth;
}

float DepthRawToNdc(float depth, float reversedZ)
{
        if (reversedZ > 0.5)
                return depth;
        return depth * 2.0 - 1.0;
}

float DepthRawToNdcDebug(float depth, float reversedZ, int mode)
{
        float raw = depth;
        if (mode == 1)
                raw = 1.0 - raw;
        if (reversedZ > 0.5)
        {
                if (mode == 2)
                        raw = 1.0 - raw;
                return raw;
        }
        float ndc = raw * 2.0 - 1.0;
        if (mode == 2)
                ndc = -ndc;
        return ndc;
}

bool DepthIsSkyDepth(float depth, float reversedZ, float skyCutoff)
{
        if (reversedZ > 0.5)
                return depth <= skyCutoff;
        return depth >= skyCutoff;
}

float ShadowReferenceFromProjZ(float projZ)
{
#if REVERSED_Z
        // clip-control path stores depth in [0..1]
        return clamp(projZ, 0.0, 1.0);
#else
        // legacy OpenGL projection stores NDC Z in [-1..1]
        return clamp(projZ * 0.5 + 0.5, 0.0, 1.0);
#endif
}
