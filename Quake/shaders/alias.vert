struct InstanceData
{
	vec4	WorldMatrix[3];
	vec4	PrevWorldMatrix[3];
	vec4	LightColor; // xyz=LightColor w=Alpha
	vec4	DLightColor; // xyz=DLightColor
	int		Pose1;
	int		Pose2;
	float	Blend;
	int		Flags;
};

layout(std430, binding=1) restrict readonly buffer AliasFrameBlock
{
	mat4	ViewProj;
	mat4	PrevViewProj;
	vec3	EyePos;
	float	_Pad0;
	vec4	Fog;
	float	ScreenDither;
	float	Overbright;
	float	ModelHalfLambert;
	float	DLightDebugModels;
	InstanceData instances[];
} AliasFrameBuffer;

struct PoseVertex
{
	vec3 pos;
	vec3 nor;
};

#if MD5
	layout(location=0) in vec3 in_pos;
	layout(location=1) in vec4 in_nor;
	layout(location=2) in vec2 in_uv;
	layout(location=3) in vec4 in_weights;
	layout(location=4) in ivec4 in_indices;

	layout(std430, binding=2) restrict readonly buffer PoseBuffer
	{
		mat3x4 BonePoses[];
	};

	PoseVertex GetPoseVertex(uint pose)
	{
		mat3x4 blendmat = BonePoses[pose + in_indices.x] * in_weights.x;
		blendmat += BonePoses[pose + in_indices.y] * in_weights.y;
		if (in_weights.z + in_weights.w > 0.0)
		{
			blendmat += BonePoses[pose + in_indices.z] * in_weights.z;
			blendmat += BonePoses[pose + in_indices.w] * in_weights.w;
		}
		mat4x3 anim = transpose(blendmat);
		return PoseVertex((anim * vec4(in_pos, 1.0)).xyz, (anim * vec4(in_nor.xyz, 0.0)).xyz);
	}

#else
	layout(location=0) in vec2 in_uv;

	layout(std430, binding=2) restrict readonly buffer BlendShapeBuffer
	{
		uvec2 PackedPosNor[];
	};

	PoseVertex GetPoseVertex(uint pose)
	{
		uvec2 data = PackedPosNor[pose + gl_VertexID];
		return PoseVertex(vec3((data.xxx >> uvec3(0, 8, 16)) & 255), unpackSnorm4x8(data.y).xyz);
	}

#endif // MD5

float r_avertexnormal_dot(vec3 vertexnormal, vec3 dir) // from MH, blended with half-lambert
{
        float d = dot(vertexnormal, dir);
        float halfLambert = max(d, 0.0) * 0.5 + 0.5;
        // wtf - this reproduces anorm_dots within as reasonable a degree of tolerance as the >= 0 case
        float quakeShade = d < 0.0 ? 1.0 + d * (13.0 / 44.0) : 1.0 + d;
        float halfLambertMix = clamp(AliasFrameBuffer.ModelHalfLambert, 0.0, 1.0);
        return mix(quakeShade, halfLambert, halfLambertMix);
}

#if MODE == 2
	layout(location=0) noperspective out vec2 out_texcoord;
#else
	layout(location=0) out vec2 out_texcoord;
#endif
layout(location=1) out vec4 out_color;
layout(location=2) out vec3 out_pos;
layout(location=3) noperspective out vec4 out_curr_clip;
layout(location=4) noperspective out vec4 out_prev_clip;
layout(location=5) flat out int out_flags;
layout(location=6) out vec3 out_normal;
layout(location=7) out float out_dlight_vis;

const int ALIAS_FLAG_VIEWMODEL = 2;

void main()
{
	InstanceData inst = AliasFrameBuffer.instances[gl_InstanceID];
	out_texcoord = in_uv;
	PoseVertex pose1 = GetPoseVertex(inst.Pose1);
	PoseVertex pose2 = GetPoseVertex(inst.Pose2);
	vec3 local_vert = mix(pose1.pos, pose2.pos, inst.Blend);
	mat4x3 worldmatrix = transpose(mat3x4(inst.WorldMatrix[0], inst.WorldMatrix[1], inst.WorldMatrix[2]));
	mat4x3 prev_worldmatrix = transpose(mat3x4(inst.PrevWorldMatrix[0], inst.PrevWorldMatrix[1], inst.PrevWorldMatrix[2]));
	vec3 world_vert = (worldmatrix * vec4(local_vert, 1.0)).xyz;
	vec3 prev_world_vert = (prev_worldmatrix * vec4(local_vert, 1.0)).xyz;
	vec4 curr_clip = AliasFrameBuffer.ViewProj * vec4(world_vert, 1.0);
	vec4 prev_clip = AliasFrameBuffer.PrevViewProj * vec4(prev_world_vert, 1.0);
	gl_Position = curr_clip;
	out_curr_clip = curr_clip;
	out_prev_clip = prev_clip;
	out_flags = inst.Flags;
	out_pos = world_vert - AliasFrameBuffer.EyePos;
	// transform world X and Z axes to local space
        mat3 orientation = mat3(normalize(worldmatrix[0].xyz), normalize(worldmatrix[1].xyz), normalize(worldmatrix[2].xyz));
        orientation = transpose(orientation);
        // BUG FIX #2: shadevector sollte die Weltkoordinaten-Beleuchtungsrichtung
        // (typisch leicht von oben, Quake-Standard ~(0, 0.5, 1) normalisiert) in
        // den Modell-Lokalraum transformieren. Die alte Formel (orientation[0]+orientation[2])
        // war eine Diagonale aus X+Z, die bei rotierten Modellen willkürlich falsch zeigt.
        // Korrekt: einen festen Weltvektor per Inverse-Transpose (= transpose(orientation))
        // in Lokalraum bringen. orientation ist bereits transponiert, also direkt multiplizieren.
        vec3 world_shade_dir = normalize(vec3(0.0, 0.5, 1.0));
        vec3 shadevector = normalize(orientation * world_shade_dir);
        float dot1 = r_avertexnormal_dot(pose1.nor, shadevector);
        float dot2 = r_avertexnormal_dot(pose2.nor, shadevector);
        float lighting = clamp(mix(dot1, dot2, inst.Blend), 0.0, 1.0);
        vec3 blended_normal = normalize(mix(pose1.nor, pose2.nor, inst.Blend));
        // BP FIX #8: Normalentransformation erfordert die inverse-transpose der
        // Weltmatrix. worldmatrix[i].xyz sind die Spaltenvektoren der 4x3-Matrix,
        // die bei nicht-uniform-Skalierung die Normalen verzerren. Korrekt:
        // normalize() auf den einzelnen Basisvektoren vor mat3-Konstruktion
        // entspricht der inversen-transponierten für orthogonale Matrizen.
        // Für korrekte Unterstützung nicht-uniformer Skalierung wäre die echte
        // Inverse-Transpose nötig (CPU-seitig berechnen und übergeben).
        mat3 world_orientation = mat3(
                normalize(worldmatrix[0].xyz),
                normalize(worldmatrix[1].xyz),
                normalize(worldmatrix[2].xyz));
        vec3 world_normal = normalize(world_orientation * blended_normal);
        vec3 view_dir = normalize(-out_pos);
        float rim = pow(max(1.0 - dot(world_normal, view_dir), 0.0), 3.0) * 0.3;

        // BUG FIX #1a: DLightColor aus dem InstanceBuffer ist bereits der
        // CPU-integrierte Gesamtbeitrag aller DLights an diesem Modell-Ursprung
        // (Radius/Abstand schon eingerechnet). Den Wert NOCHMALS mit `lighting`
        // (Vertex-NdotL) zu multiplizieren doppelt die Abschwächung → zu dunkle
        // oder bei fast-tangentialen Normalen grau-helle Fragmente.
        // Korrektur: DLightColor direkt übernehmen, NdotL gilt nur für ambient.
        //
        // BUG FIX #1b: max(LightColor - DLightColor, 0) subtrahiert per-Kanal.
        // Wenn ein DLight-Kanal > LightColor-Kanal ist (z.B. reines Blau-DLight
        // über weißem Ambient), wird der Ambient-Kanal auf 0 geclampt, aber die
        // anderen Kanäle bleiben unverändert, was Farbverschiebungen erzeugt.
        // Besser: den vollen LightColor-Wert als Ambient-Basis verwenden und
        // DLight additiv dazurechnen statt subtraktiv zu trennen.
        vec3 litAmbient = inst.LightColor.rgb * (mix(0.35, 1.0, lighting) * AliasFrameBuffer.Overbright);
        // DLightColor direkt (kein weiteres lighting-Weighting) – CPU hat bereits
        // Distanz/Radius berücksichtigt. NdotL-Faktor hier wäre Doppelmodulation.
        vec3 litDlight = inst.DLightColor.rgb;
        vec3 base_color = litAmbient + litDlight;
        bool is_viewmodel = (inst.Flags & ALIAS_FLAG_VIEWMODEL) != 0;
        vec3 final_color = is_viewmodel ? base_color + vec3(rim) : base_color;
        out_color = clamp(vec4(final_color, inst.LightColor.a), 0.0, AliasFrameBuffer.Overbright);
	out_dlight_vis = clamp(dot(litDlight, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
	out_normal = world_normal;
}
