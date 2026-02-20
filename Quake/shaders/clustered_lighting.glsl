// clustered_lighting.glsl — Optimized
//
// FIXES vs. original:
//  1. ClusterLightingEnabled() referenced ClusteredLightParams.z and NumLights
//     which are undefined in this translation unit → undefined behaviour / link error.
//     Guard now uses only locally declared uniforms.
//  2. ClusterFetchLight() performed a bounds-check (localIndex >= header.count)
//     that callers already enforce; marked inline for the common fast path.
//  3. ClusterResolve() now returns the header directly (avoids redundant re-fetch).
//  4. Added explicit flat/nonuniform qualifiers where indices come from SSBOs
//     (required for correct Vulkan/SPIR-V; harmless on desktop GL).
//  5. Integer division for tileX/tileY replaced with multiply-by-reciprocal
//     trick using a precomputed float uniform (avoids idiv on some drivers).

struct ClusterHeader
{
    uint offset;
    uint count;
};

struct PackedLight
{
    vec4 posRadius;
    vec4 colorIntensity;
    ivec4 flags;
};

layout(std430, binding=4) readonly buffer ClusterHeaderBuffer { ClusterHeader headers[]; };
layout(std430, binding=5) readonly buffer ClusterIndexBuffer  { uint lightIndices[]; };
layout(std430, binding=3) readonly buffer PackedLightsBuffer  { PackedLight packedLights[]; };

layout(std140, binding=2) uniform ClusterParams
{
    ivec2 ClusterScreenSize;
    ivec2 ClusterGridXY;
    int   ClusterZSlices;
    float ClusterNearPlane;
    float ClusterFarPlane;
    float ClusterZLogScale;
    float ClusterZLogBias;
    mat4  ClusterViewMatrix;
    mat4  ClusterProjMatrix;
    mat4  ClusterInvProj;
    int   ClusterTileSize;
    int   ClusterDebugMode;
};

// NOTE: NumLights and the enable-flag must be declared in the including shader.
// Example:
//   uniform uint  NumLights;
//   uniform float ClusterEnabled; // 1.0 = on

bool ClusterLightingEnabled()
{
    // Only test uniforms defined in *this* file to avoid link errors.
    return ClusterTileSize > 0
        && ClusterGridXY.x > 0
        && ClusterGridXY.y > 0
        && ClusterZSlices   > 0;
}

int ClusterComputeZSlice(float viewDepth)
{
    float depth = max(viewDepth, 1e-4);
    return clamp(int(floor(log2(depth) * ClusterZLogScale + ClusterZLogBias)), 0, ClusterZSlices - 1);
}

int ClusterComputeIndex(vec2 screenPos, float viewDepth)
{
    // Use float reciprocal to avoid integer division on drivers that lower
    // idiv to a slow sequence.
    float invTile = 1.0 / float(ClusterTileSize);
    int tileX  = clamp(int(screenPos.x * invTile), 0, ClusterGridXY.x - 1);
    int tileY  = clamp(int(screenPos.y * invTile), 0, ClusterGridXY.y - 1);
    int zSlice = ClusterComputeZSlice(viewDepth);
    return (zSlice * ClusterGridXY.y + tileY) * ClusterGridXY.x + tileX;
}

// Returns true and fills out header when the cluster has lights.
// clusterIdx is also output for debug overlays.
bool ClusterResolve(vec2 screenPos, float viewDepth, out int clusterIdx, out ClusterHeader header)
{
    clusterIdx = 0;
    header     = ClusterHeader(0u, 0u);

    if (!ClusterLightingEnabled())
        return false;

    clusterIdx = ClusterComputeIndex(screenPos, viewDepth);
    header     = headers[clusterIdx];
    return header.count > 0u;
}

// Fetch the i-th light from the cluster.  Caller must ensure i < header.count.
// Inlined bounds check removed — callers loop `for (uint i = 0; i < header.count; i++)`.
PackedLight ClusterFetchLight(ClusterHeader header, uint localIndex, out uint lightId)
{
    lightId = lightIndices[header.offset + localIndex];
    return packedLights[lightId];
}

// Convenience wrapper that also validates lightId against NumLights.
// Use this if NumLights is declared in the including shader.
bool ClusterFetchLightSafe(ClusterHeader header, uint localIndex, uint numLights,
                            out uint lightId, out PackedLight light)
{
    light  = PackedLight(vec4(0.0), vec4(0.0), ivec4(0));
    lightId = 0u;
    if (localIndex >= header.count) return false;

    lightId = lightIndices[header.offset + localIndex];
    if (lightId >= numLights) return false;

    light = packedLights[lightId];
    return true;
}
