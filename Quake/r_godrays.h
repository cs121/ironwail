#ifndef R_GODRAYS_H
#define R_GODRAYS_H

#include "quakedef.h"

typedef struct godrays_sky_params_s
{
	qboolean enabled;
	float threshold;
	float intensity;
	float softness;
	float tint[3];
} godrays_sky_params_t;

typedef struct godrays_stabilization_s
{
	float prev_x;
	float prev_y;
	qboolean initialized;
} godrays_stabilization_t;

typedef struct r_godrays_stabilize_input_s
{
	int width;
	int height;
	float raw_x;
	float raw_y;
	vec3_t viewangles;
	double time;
	float stabilize;
	float smooth_rate;
	float stabilize_strength;
	float stabilize_max_px;
	float max_shift_per_sec;
	qboolean reset_on_teleport;
} r_godrays_stabilize_input_t;

static inline void R_Godrays_RegisterCvars (void)
{
}

static inline void R_Godrays_ResetStabilization (godrays_stabilization_t *stabilization)
{
	if (!stabilization)
		return;
	stabilization->prev_x = 0.5f;
	stabilization->prev_y = 0.5f;
	stabilization->initialized = false;
}

static inline void R_Godrays_GetSkyParams (float enabled_value, float threshold, float intensity,
	float softness, const char *tint_string, godrays_sky_params_t *params)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (!params)
		return;
	params->enabled = (enabled_value > 0.f);
	params->threshold = q_max (0.f, threshold);
	params->intensity = q_max (0.f, intensity);
	params->softness = q_max (0.f, softness);
	if (tint_string
#ifdef RENDERER_PLUGIN_BUILD
		&& sscanf_s (tint_string, "%f %f %f", &r, &g, &b) == 3
#else
		&& q_sscanf (tint_string, "%f %f %f", &r, &g, &b) == 3
#endif
	)
	{
		params->tint[0] = CLAMP (0.f, r, 8.f);
		params->tint[1] = CLAMP (0.f, g, 8.f);
		params->tint[2] = CLAMP (0.f, b, 8.f);
	}
	else
	{
		params->tint[0] = 1.f;
		params->tint[1] = 1.f;
		params->tint[2] = 1.f;
	}
}

static inline qboolean R_Godrays_IsReady (const qmodel_t *worldmodel, int framecount)
{
	(void)worldmodel;
	(void)framecount;
	return false;
}

static inline float R_Godrays_SanitizeValue (float value, float fallback, float min_value, float max_value)
{
	if (isnan (value) || isinf (value))
		value = fallback;
	return CLAMP (min_value, value, max_value);
}

static inline void R_Godrays_ComputeLightPos (godrays_stabilization_t *stabilization,
	const r_godrays_stabilize_input_t *input, float *out_x, float *out_y)
{
	float x = 0.5f;
	float y = 0.5f;
	if (input)
	{
		x = CLAMP (0.f, input->raw_x, 1.f);
		y = CLAMP (0.f, input->raw_y, 1.f);
	}
	if (out_x)
		*out_x = x;
	if (out_y)
		*out_y = y;
	if (stabilization)
	{
		stabilization->prev_x = x;
		stabilization->prev_y = y;
		stabilization->initialized = true;
	}
}

#endif
