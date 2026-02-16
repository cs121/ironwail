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

	float indoorAtten = mix(1.0, 0.3, clamp(indoorFactor, 0.0, 1.0));
	float fresnel = pow(1.0 - max(dot(N, V), 0.0), 5.0);
	float fresnelTerm = mix(0.04, 1.0, fresnel);
	return reflColor * (intensity * glossOrSpecMask * indoorAtten * fresnelTerm);
}

#endif // ENVLIGHT_GLSL
