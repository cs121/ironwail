// clustered_lighting.glsl
// Fragment-shader include for Clustered Lighting.
// Included via #include or shader-stage concatenation.

struct ClusterHeader
{
	uint offset;
	uint count;
};

struct PackedLight
{
	vec4 posRadius;      // xyz=worldPos, w=radius
	vec4 colorIntensity; // xyz=color, w=intensity
	ivec4 flags;
};

layout(std430, binding=4) readonly buffer ClusterHeaderBuffer
{
	ClusterHeader headers[];
};

layout(std430, binding=5) readonly buffer ClusterIndexBuffer
{
	uint lightIndices[];
};

layout(std430, binding=3) readonly buffer PackedLightsBuffer
{
	PackedLight packedLights[];
};

layout(std140, binding=2) uniform ClusterParams
{
	ivec2 ClusterScreenSize;
	ivec2 ClusterGridXY;
	int   ClusterZSlices;
	int   ClusteredEnabled;      // 0 = off, 1 = on
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

// -------------------------------------------------------------------------
// Returns true if Clustered Lighting is active for this frame.
// -------------------------------------------------------------------------
bool ClusterLightingEnabled()
{
	return ClusteredEnabled > 0
		&& ClusterTileSize > 0
		&& ClusterGridXY.x > 0
		&& ClusterGridXY.y > 0
		&& ClusterZSlices > 0;
}

// -------------------------------------------------------------------------
// Converts a positive view-space depth to a Z-slice index.
// viewDepth must be positive (OpenGL: -v.z).
// -------------------------------------------------------------------------
int ClusterComputeZSlice(float viewDepth)
{
	// max(viewDepth, 1e-4) guards log2 against zero and negative values.
	float z = floor(log2(max(viewDepth, 1e-4)) * ClusterZLogScale + ClusterZLogBias);
	return clamp(int(z), 0, ClusterZSlices - 1);
}

// -------------------------------------------------------------------------
// Computes the linear cluster index for a screen position and depth.
// screenPos: fragment position in pixels [0, screenSize).
// viewDepth: positive view-space depth (-fragPosView.z).
// -------------------------------------------------------------------------
int ClusterComputeIndex(vec2 screenPos, float viewDepth)
{
	int tileX  = clamp(int(screenPos.x) / ClusterTileSize, 0, ClusterGridXY.x - 1);
	int tileY  = clamp(int(screenPos.y) / ClusterTileSize, 0, ClusterGridXY.y - 1);
	int zSlice = ClusterComputeZSlice(viewDepth);
	// Index layout: [z][y][x] (z-major for cache locality at constant depth).
	return (zSlice * ClusterGridXY.y + tileY) * ClusterGridXY.x + tileX;
}

// -------------------------------------------------------------------------
// Resolves the cluster for a fragment point.
// Returns false if Clustered Lighting is disabled or the cluster is empty.
// -------------------------------------------------------------------------
bool ClusterResolve(vec2 screenPos, float viewDepth,
                    out int clusterIdx,
                    out ClusterHeader header)
{
	clusterIdx = 0;
	header     = ClusterHeader(0u, 0u);

	if (!ClusterLightingEnabled())
		return false;

	clusterIdx = ClusterComputeIndex(screenPos, viewDepth);
	header     = headers[clusterIdx];

	return header.count > 0u;
}

// -------------------------------------------------------------------------
// Fetches a single light from the cluster index buffer.
// localIndex: index within the cluster [0, header.count).
// -------------------------------------------------------------------------
bool ClusterFetchLight(ClusterHeader header, uint localIndex,
                       out uint lightId, out PackedLight light)
{
	lightId = 0u;
	light   = PackedLight(vec4(0.0), vec4(0.0), ivec4(0));

	if (localIndex >= header.count)
		return false;

	lightId = lightIndices[header.offset + localIndex];
	light   = packedLights[lightId];
	return true;
}

// -------------------------------------------------------------------------
// Typical usage in a fragment shader:
//
//   vec3 Lo = vec3(0.0);
//   int clusterIdx;
//   ClusterHeader header;
//   if (ClusterResolve(gl_FragCoord.xy, -fragPosView.z, clusterIdx, header))
//   {
//       for (uint i = 0u; i < header.count; ++i)
//       {
//           uint lightId; PackedLight light;
//           if (!ClusterFetchLight(header, i, lightId, light)) break;
//           Lo += EvaluateLight(light, ...);
//       }
//   }
//
// -------------------------------------------------------------------------
