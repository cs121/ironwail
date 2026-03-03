#include "r_godrays.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

float R_Godrays_SanitizeValue (float value, float fallback, float minval, float maxval)
{
#if defined(_MSC_VER)
	if (_finite (value) == 0)
		return fallback;
#else
	if (!isfinite (value))
		return fallback;
#endif

	if (value < minval)
		return minval;
	if (value > maxval)
		return maxval;
	return value;
}

void R_Godrays_GetSkyParams (float sky_enable, float sky_threshold, float sky_intensity, float sky_softness, const char *sky_tint_string, godrays_sky_params_t *params)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (!params)
		return;

	if (sky_tint_string && sky_tint_string[0])
	{
		if (sscanf (sky_tint_string, "%f %f %f", &r, &g, &b) != 3)
			r = g = b = 1.f;
	}

	params->enabled = (sky_enable > 0.f);
	params->threshold = R_Godrays_SanitizeValue (sky_threshold, 0.05f, 0.f, 1.f);
	params->intensity = q_max (0.f, sky_intensity);
	params->softness = R_Godrays_SanitizeValue (sky_softness, 1.5f, 0.f, FLT_MAX);
	params->tint[0] = q_max (0.f, r);
	params->tint[1] = q_max (0.f, g);
	params->tint[2] = q_max (0.f, b);
}

qboolean R_Godrays_IsReady (const qmodel_t *worldmodel, int framecount)
{
	if (!worldmodel || framecount <= 1)
		return false;
	if (!worldmodel->textures || !worldmodel->usedtextures || worldmodel->numtextures <= 0)
		return false;
	return true;
}

void R_Godrays_ResetStabilization (godrays_stabilization_t *stabilization)
{
	if (!stabilization)
		return;
	stabilization->valid = false;
	stabilization->smoothed_pos[0] = 0.f;
	stabilization->smoothed_pos[1] = 0.f;
	stabilization->last_time = 0.0;
	VectorClear (stabilization->last_viewangles);
}

void R_Godrays_ComputeLightPos (godrays_stabilization_t *stabilization, const r_godrays_stabilize_input_t *input, float *out_x, float *out_y)
{
	float raw_x, raw_y, max_shift;
	qboolean should_reset;
	if (!stabilization || !input || !out_x || !out_y)
		return;

	raw_x = R_Godrays_SanitizeValue (input->raw_x, 0.5f, 0.f, 1.f);
	raw_y = R_Godrays_SanitizeValue (input->raw_y, 0.5f, 0.f, 1.f);
	max_shift = R_Godrays_SanitizeValue (input->stabilize_max_px, 0.f, 0.f, FLT_MAX);
	should_reset = !stabilization->valid;

	if (!should_reset && input->reset_on_teleport)
	{
		float yaw_delta = fabsf (AngleDifference (input->viewangles[YAW], stabilization->last_viewangles[YAW]));
		float pitch_delta = fabsf (AngleDifference (input->viewangles[PITCH], stabilization->last_viewangles[PITCH]));
		vec2_t delta = { raw_x - stabilization->smoothed_pos[0], raw_y - stabilization->smoothed_pos[1] };
		float delta_len = sqrtf (delta[0] * delta[0] + delta[1] * delta[1]);
		if (yaw_delta > 45.f || pitch_delta > 35.f || delta_len > 0.35f)
			should_reset = true;
	}

	if (should_reset)
	{
		stabilization->smoothed_pos[0] = raw_x;
		stabilization->smoothed_pos[1] = raw_y;
	}
	else
	{
		float dt = (float)(input->time - stabilization->last_time);
		vec2_t delta, step;
		float alpha = 1.f;
		if (dt < 0.f)
			dt = 0.f;
		delta[0] = raw_x - stabilization->smoothed_pos[0];
		delta[1] = raw_y - stabilization->smoothed_pos[1];
		if (input->stabilize_strength > 0.f)
		{
			float t = q_max (0.f, dt) * 60.f;
			alpha = 1.f - powf (1.f - input->stabilize_strength, t);
		}
		else if (input->smooth_rate > 0.f && dt > 0.f)
			alpha = 1.f - expf (-dt * input->smooth_rate);

		step[0] = delta[0] * alpha;
		step[1] = delta[1] * alpha;

		if (max_shift <= 0.f && dt > 0.f)
			max_shift = R_Godrays_SanitizeValue (input->max_shift_per_sec, 0.f, 0.f, FLT_MAX) * dt;

		if (max_shift > 0.f && input->width > 0 && input->height > 0)
		{
			vec2_t step_px = { step[0] * (float)input->width, step[1] * (float)input->height };
			float step_len = sqrtf (step_px[0] * step_px[0] + step_px[1] * step_px[1]);
			if (step_len > max_shift && step_len > 0.f)
			{
				float scale = max_shift / step_len;
				step[0] = (step_px[0] * scale) / (float)input->width;
				step[1] = (step_px[1] * scale) / (float)input->height;
			}
		}
		stabilization->smoothed_pos[0] += step[0];
		stabilization->smoothed_pos[1] += step[1];
	}

	stabilization->last_time = input->time;
	VectorCopy (input->viewangles, stabilization->last_viewangles);
	stabilization->valid = true;
	*out_x = CLAMP (0.f, raw_x + (stabilization->smoothed_pos[0] - raw_x) * input->stabilize, 1.f);
	*out_y = CLAMP (0.f, raw_y + (stabilization->smoothed_pos[1] - raw_y) * input->stabilize, 1.f);
}
