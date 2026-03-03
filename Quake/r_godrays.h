#ifndef R_GODRAYS_H
#define R_GODRAYS_H

#include "quakedef.h"

typedef struct godrays_sky_params_s
{
	qboolean enabled;
	float threshold;
	float intensity;
	float softness;
	vec3_t tint;
} godrays_sky_params_t;

typedef struct godrays_stabilization_s
{
	qboolean valid;
	vec2_t smoothed_pos;
	double last_time;
	vec3_t last_viewangles;
} godrays_stabilization_t;

typedef struct r_godrays_stabilize_input_s
{
	int width;
	int height;
	float raw_x;
	float raw_y;
	float viewangles[3];
	double time;
	float stabilize;
	float smooth_rate;
	float stabilize_strength;
	float stabilize_max_px;
	float max_shift_per_sec;
	qboolean reset_on_teleport;
} r_godrays_stabilize_input_t;

float R_Godrays_SanitizeValue (float value, float fallback, float minval, float maxval);
void R_Godrays_GetSkyParams (float sky_enable, float sky_threshold, float sky_intensity, float sky_softness, const char *sky_tint_string, godrays_sky_params_t *params);
qboolean R_Godrays_IsReady (const qmodel_t *worldmodel, int framecount);
void R_Godrays_ResetStabilization (godrays_stabilization_t *stabilization);
void R_Godrays_ComputeLightPos (godrays_stabilization_t *stabilization, const r_godrays_stabilize_input_t *input, float *out_x, float *out_y);

#endif
