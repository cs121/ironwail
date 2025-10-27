#if MODE == 2
	layout(location=0) noperspective in vec2 in_texcoord;
#else
	layout(location=0) in vec2 in_texcoord;
#endif
layout(location=1) in vec4 in_color;
layout(location=2) in vec3 in_pos;
layout(location=3) noperspective in vec4 in_curr_clip;
layout(location=4) noperspective in vec4 in_prev_clip;
layout(location=5) flat in int in_flags;
layout(location=6) in vec3 in_world_pos;
layout(location=7) in vec3 in_world_normal;
layout(location=8) in vec3 in_local_normal;
layout(location=9) flat in vec3 in_shadevector;
layout(location=10) in vec3 in_tangent;
layout(location=11) in vec3 in_bitangent;
layout(location=12) in float in_linear_depth;
layout(location=13) in vec3 in_view_dir_tangent;
layout(location=14) in vec3 in_light_dir_tangent;
layout(location=15) in float in_curvature;
layout(location=16) in vec3 in_view_dir_world;
layout(location=17) in float in_thickness_hint;

// ============================================================================
// UNIFORMS
// ============================================================================
layout(binding=0) uniform sampler2D u_albedo_map;
layout(binding=1) uniform sampler2D u_normal_map;
layout(binding=2) uniform sampler2D u_height_map;      // For parallax
layout(binding=3) uniform sampler2D u_roughness_map;
layout(binding=4) uniform sampler2D u_metallic_map;
layout(binding=5) uniform sampler2D u_ao_map;
layout(binding=6) uniform sampler2D u_sss_thickness_map; // Thickness for SSS

uniform vec3 u_light_color = vec3(1.0);
uniform vec3 u_ambient_light = vec3(0.03);
uniform float u_parallax_scale = 0.04;
uniform int u_parallax_steps = 32;
uniform float u_sss_strength = 0.5;
uniform vec3 u_sss_color = vec3(1.0, 0.3, 0.3); // Reddish for skin
uniform float u_sss_distortion = 0.2;
uniform float u_fog_density = 0.001;

// ============================================================================
// OUTPUTS
// ============================================================================
layout(location=0) out vec4 out_color;
layout(location=1) out vec4 out_normal;
layout(location=2) out vec2 out_motion;

// ============================================================================
// PARALLAX OCCLUSION MAPPING
// ============================================================================
vec2 ParallaxOcclusionMapping(vec2 texCoords, vec3 viewDir)
{
	// Number of layers
	const float minLayers = 8.0;
	const float maxLayers = float(u_parallax_steps);
	float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDir)));
	
	// Calculate the size of each layer
	float layerDepth = 1.0 / numLayers;
	float currentLayerDepth = 0.0;
	
	// Amount to shift texture coordinates per layer
	vec2 P = viewDir.xy * u_parallax_scale;
	vec2 deltaTexCoords = P / numLayers;
	
	// Initial values
	vec2 currentTexCoords = texCoords;
	float currentDepthMapValue = texture(u_height_map, currentTexCoords).r;
	
	// Steep parallax mapping
	while(currentLayerDepth < currentDepthMapValue)
	{
		currentTexCoords -= deltaTexCoords;
		currentDepthMapValue = texture(u_height_map, currentTexCoords).r;
		currentLayerDepth += layerDepth;
	}
	
	// Parallax occlusion mapping (relief mapping)
	vec2 prevTexCoords = currentTexCoords + deltaTexCoords;
	
	float afterDepth = currentDepthMapValue - currentLayerDepth;
	float beforeDepth = texture(u_height_map, prevTexCoords).r - currentLayerDepth + layerDepth;
	
	float weight = afterDepth / (afterDepth - beforeDepth);
	vec2 finalTexCoords = prevTexCoords * weight + currentTexCoords * (1.0 - weight);
	
	return finalTexCoords;
}

// ============================================================================
// SUBSURFACE SCATTERING (TRANSLUCENCY APPROXIMATION)
// ============================================================================
vec3 SubsurfaceScattering(vec3 lightDir, vec3 viewDir, vec3 normal, float thickness)
{
	// Distorted light vector for back-face light transmission
	vec3 lightVecDistorted = lightDir + normal * u_sss_distortion;
	float sssIntensity = pow(clamp(dot(viewDir, -lightVecDistorted), 0.0, 1.0), 4.0);
	
	// Modulate by thickness
	sssIntensity *= thickness;
	
	// Add curvature influence for more detail
	sssIntensity *= (1.0 + in_curvature * 0.5);
	
	return u_sss_color * sssIntensity * u_sss_strength;
}

// ============================================================================
// PBR LIGHTING (SIMPLIFIED)
// ============================================================================
const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH * NdotH;
	
	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;
	
	return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float r = (roughness + 1.0);
	float k = (r * r) / 8.0;
	
	float nom = NdotV;
	float denom = NdotV * (1.0 - k) + k;
	
	return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	float ggx2 = GeometrySchlickGGX(NdotV, roughness);
	float ggx1 = GeometrySchlickGGX(NdotL, roughness);
	
	return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================================
// MAIN
// ============================================================================
void main()
{
	// ========================================================================
	// PARALLAX OCCLUSION MAPPING
	// ========================================================================
	vec3 viewDirTangent = normalize(in_view_dir_tangent);
	vec2 texCoords = ParallaxOcclusionMapping(in_texcoord, viewDirTangent);
	
	// Discard fragments outside texture bounds (steep parallax)
	if(texCoords.x > 1.0 || texCoords.y > 1.0 || texCoords.x < 0.0 || texCoords.y < 0.0)
		discard;
	
	// ========================================================================
	// TEXTURE SAMPLING
	// ========================================================================
	vec4 albedo = texture(u_albedo_map, texCoords);
	vec3 normalMap = texture(u_normal_map, texCoords).xyz * 2.0 - 1.0;
	float roughness = texture(u_roughness_map, texCoords).r;
	float metallic = texture(u_metallic_map, texCoords).r;
	float ao = texture(u_ao_map, texCoords).r;
	float thickness = texture(u_sss_thickness_map, texCoords).r * in_thickness_hint;
	
	// Alpha test for transparency
	if(albedo.a < 0.5)
		discard;
	
	// ========================================================================
	// NORMAL MAPPING
	// ========================================================================
	mat3 TBN = mat3(
		normalize(in_tangent),
		normalize(in_bitangent),
		normalize(in_world_normal)
	);
	vec3 N = normalize(TBN * normalMap);
	
	// ========================================================================
	// LIGHTING SETUP
	// ========================================================================
	vec3 V = normalize(in_view_dir_world);
	vec3 L = normalize(in_shadevector);
	vec3 H = normalize(V + L);
	
	float NdotL = max(dot(N, L), 0.0);
	
	// ========================================================================
	// PBR CALCULATIONS
	// ========================================================================
	vec3 F0 = vec3(0.04);
	F0 = mix(F0, albedo.rgb, metallic);
	
	// Cook-Torrance BRDF
	float NDF = DistributionGGX(N, H, roughness);
	float G = GeometrySmith(N, V, L, roughness);
	vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
	
	vec3 numerator = NDF * G * F;
	float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
	vec3 specular = numerator / denominator;
	
	vec3 kS = F;
	vec3 kD = vec3(1.0) - kS;
	kD *= 1.0 - metallic;
	
	// ========================================================================
	// SUBSURFACE SCATTERING
	// ========================================================================
	vec3 sss = SubsurfaceScattering(L, V, N, thickness);
	
	// ========================================================================
	// FINAL LIGHTING
	// ========================================================================
	vec3 radiance = u_light_color * in_color.rgb;
	vec3 Lo = (kD * albedo.rgb / PI + specular) * radiance * NdotL;
	Lo += sss * radiance;
	
	// Ambient
	vec3 ambient = u_ambient_light * albedo.rgb * ao;
	vec3 color = ambient + Lo;
	
	// ========================================================================
	// FOG
	// ========================================================================
	float fogFactor = 1.0 - exp(-u_fog_density * in_linear_depth);
	color = mix(color, vec3(0.5, 0.6, 0.7), fogFactor); // Sky blue fog
	
	// ========================================================================
	// TONE MAPPING AND GAMMA CORRECTION
	// ========================================================================
	color = color / (color + vec3(1.0)); // Reinhard
	color = pow(color, vec3(1.0/2.2)); // Gamma correction
	
	// ========================================================================
	// OUTPUT
	// ========================================================================
	out_color = vec4(color, albedo.a);
	
	// Normal output for deferred rendering
	out_normal = vec4(N * 0.5 + 0.5, 1.0);
	
	// Motion vectors for TAA/Motion Blur
	vec2 curr_ndc = in_curr_clip.xy / in_curr_clip.w;
	vec2 prev_ndc = in_prev_clip.xy / in_prev_clip.w;
	out_motion = (curr_ndc - prev_ndc) * 0.5;
}
