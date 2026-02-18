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
	int ClusterZSlices;
	float ClusterNearPlane;
	float ClusterFarPlane;
	float ClusterZLogScale;
	float ClusterZLogBias;
	mat4 ClusterViewMatrix;
	mat4 ClusterProjMatrix;
	mat4 ClusterInvProj;
	int ClusterTileSize;
	int ClusterDebugMode;
};

bool ClusterLightingEnabled()
{
	return ClusteredLightParams.z > 0.5
		&& NumLights > 0u
		&& ClusterTileSize > 0
		&& ClusterGridXY.x > 0
		&& ClusterGridXY.y > 0
		&& ClusterZSlices > 0;
}

int ClusterComputeZSlice(float viewDepth)
{
	float depth = max(viewDepth, 1e-4);
	float z = floor(log2(depth) * ClusterZLogScale + ClusterZLogBias);
	return clamp(int(z), 0, ClusterZSlices - 1);
}

int ClusterComputeIndex(vec2 screenPos, float viewDepth)
{
	int tileX = clamp(int(screenPos.x) / ClusterTileSize, 0, ClusterGridXY.x - 1);
	int tileY = clamp(int(screenPos.y) / ClusterTileSize, 0, ClusterGridXY.y - 1);
	int zSlice = ClusterComputeZSlice(viewDepth);
	return (zSlice * ClusterGridXY.y + tileY) * ClusterGridXY.x + tileX;
}

bool ClusterResolve(vec2 screenPos, float viewDepth, out int clusterIdx, out ClusterHeader header, out uint clusterCount)
{
	clusterIdx = 0;
	header = ClusterHeader(0u, 0u);
	clusterCount = 0u;
	if (!ClusterLightingEnabled())
		return false;

	clusterIdx = ClusterComputeIndex(screenPos, viewDepth);
	header = headers[clusterIdx];
	clusterCount = min(header.count, NumLights);
	return clusterCount > 0u;
}

bool ClusterFetchLight(ClusterHeader header, uint localIndex, out uint lightId, out PackedLight light)
{
	lightId = 0u;
	light = PackedLight(vec4(0.0), vec4(0.0), ivec4(0));
	if (localIndex >= header.count)
		return false;

	lightId = lightIndices[header.offset + localIndex];
	if (lightId >= NumLights)
		return false;

	light = packedLights[lightId];
	return true;
}
