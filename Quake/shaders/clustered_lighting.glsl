// clustered_lighting.glsl
// Fragment-Shader-Include für Clustered Lighting.
// Wird per #include oder shader-stage-Verkettung eingebunden.

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
	int   ClusteredEnabled;      // 0 = aus, 1 = an
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

// NumLights muss vom aufrufenden Shader als Uniform bereitgestellt werden,
// z.B.: uniform uint NumLights;
// Alternativ kann es aus einem weiteren Uniform-Block kommen.
// Deklaration hier als extern (wird vom Linker aufgelöst):
// uniform uint NumLights;  ← auskommentiert, da der Host-Shader es bereitstellt.

// -------------------------------------------------------------------------
// Prüft, ob Clustered Lighting für diesen Frame aktiv ist.
// BUG-FIX: Originalcode referenzierte 'ClusteredLightParams.z' (undefiniert).
//          Korrekt ist die Abfrage von 'ClusteredEnabled' aus dem Uniform-Block.
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
// Berechnet den Z-Slice für eine gegebene positive View-Space-Tiefe.
// viewDepth muss positiv sein (OpenGL: -v.z).
// -------------------------------------------------------------------------
int ClusterComputeZSlice(float viewDepth)
{
	// BUG-FIX: max(viewDepth, 1e-4) schützt log2 vor 0 und negativen Werten.
	float z = floor(log2(max(viewDepth, 1e-4)) * ClusterZLogScale + ClusterZLogBias);
	return clamp(int(z), 0, ClusterZSlices - 1);
}

// -------------------------------------------------------------------------
// Berechnet den linearen Cluster-Index für eine Bildschirmposition und Tiefe.
// screenPos: Fragmentposition in Pixel [0, screenSize).
// viewDepth:  positive View-Space-Tiefe (-fragPosView.z).
// -------------------------------------------------------------------------
int ClusterComputeIndex(vec2 screenPos, float viewDepth)
{
	// OPTIMIERUNG: Integer-Division direkt, kein float-cast nötig.
	int tileX  = clamp(int(screenPos.x) / ClusterTileSize, 0, ClusterGridXY.x - 1);
	int tileY  = clamp(int(screenPos.y) / ClusterTileSize, 0, ClusterGridXY.y - 1);
	int zSlice = ClusterComputeZSlice(viewDepth);
	// Index-Layout: [z][y][x] (z-major für Cache-Lokalität bei konstanter Tiefe).
	return (zSlice * ClusterGridXY.y + tileY) * ClusterGridXY.x + tileX;
}

// -------------------------------------------------------------------------
// Löst den Cluster für einen Fragmentpunkt auf.
// Gibt false zurück, wenn Clustered Lighting deaktiviert ist oder
// der Cluster leer ist.
//
// BUG-FIX: 'clusterCount = min(header.count, NumLights)' verwendete die
//          undefinierte Variable NumLights. Stattdessen: header.count
//          ist bereits durch cluster_prefix.comp auf clusterAlloc gecappt.
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
// Liest ein einzelnes Licht aus dem Cluster-Index-Buffer.
// localIndex: Index innerhalb des Clusters [0, header.count).
//
// OPTIMIERUNG: Kein redundanter Bounds-Check auf NumLights (undefiniert/
//              potenziell falsch); lightIndices enthält nur valide IDs,
//              die vom Fill-Pass korrekt begrenzt wurden.
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
// Convenience-Makro für den typischen Render-Loop.
// Verwendung im Fragment-Shader:
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
