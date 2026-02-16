#ifndef ENVLIGHT_GLSL
#define ENVLIGHT_GLSL

float EnvLightLuma(vec3 rgb)
{
	return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

float DeriveIndoorFactor(float lightgridLuma, float aoHint, float visibilityHint)
{
	float luma = clamp(lightgridLuma, 0.0, 1.0);
	float ao = clamp(aoHint, 0.0, 1.0);
	float visibility = clamp(visibilityHint, 0.0, 1.0);
	float openness = luma * mix(0.35, 1.0, ao) * visibility;
	return clamp(1.0 - openness, 0.0, 1.0);
}

float EnvIndoorSkyAttenuation(float indoorFactor)
{
	return mix(1.0, 0.3, clamp(indoorFactor, 0.0, 1.0));
}

vec3 EvaluateWorldEnvFill(
	vec3 staticLight,
	vec3 ambientLightgrid,
	float indoorFactor,
	float fillStrength)
{
	if (fillStrength <= 0.0)
		return vec3(0.0);

	vec3 baseStatic = clamp(staticLight, 0.0, 1.0);
	float staticLuma = EnvLightLuma(baseStatic);
	float darkness = 1.0 - staticLuma;
	float fillWeight = darkness * darkness;

	// Conservative defaults: keep fill subtle and mostly limited to dark regions.
	const float FILL_ADD_MAX = 0.06;
	const float FILL_MUL_MAX = 0.10;
	float indoorAtten = EnvIndoorSkyAttenuation(indoorFactor);
	vec3 fillColor = clamp(ambientLightgrid, 0.0, 1.0);
	vec3 additive = fillColor * (FILL_ADD_MAX * fillWeight);
	vec3 multiplicative = baseStatic * (FILL_MUL_MAX * fillWeight);

	return (additive + multiplicative) * (fillStrength * indoorAtten);
}

vec3 EvaluateReflectionProbe(
	samplerCube reflectionTex,
	float probesEnabled,
	vec3 worldPos,
	vec3 normal,
	vec3 viewDir,
	float glossOrSpecMask,
	float indoorFactor,
	float intensity)
{
	if (probesEnabled <= 0.5 || intensity <= 0.0 || glossOrSpecMask <= 0.0)
		return vec3(0.0);

	vec3 N = normalize(normal);
	vec3 V = normalize(viewDir);
	vec3 refl = reflect(-V, N);
	float maxMip = 6.0;
	float lod = clamp(1.0 - glossOrSpecMask, 0.0, 1.0) * maxMip;
	vec3 reflColor = textureLod(reflectionTex, refl, lod).rgb;

	float indoorAtten = EnvIndoorSkyAttenuation(indoorFactor);
	float fresnel = pow(1.0 - max(dot(N, V), 0.0), 5.0);
	float fresnelTerm = mix(0.04, 1.0, fresnel);
	return reflColor * (intensity * glossOrSpecMask * indoorAtten * fresnelTerm);
}

#endif // ENVLIGHT_GLSL
