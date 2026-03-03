#include "r_godrays.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

extern cvar_t r_godrays;
extern cvar_t r_godrays_emit_emissive;
extern cvar_t r_godrays_emit_lighttex;
extern cvar_t r_godray_sky_enable;
extern cvar_t r_godray_sky_threshold;
extern cvar_t r_godray_sky_intensity;
extern cvar_t r_godray_sky_blur;
extern cvar_t r_godrays_sky_enable;
extern cvar_t r_godrays_sky_threshold;
extern cvar_t r_godrays_sky_intensity;
extern cvar_t r_godrays_sky_tint;
extern cvar_t r_godrays_emissive_intensity;
extern cvar_t r_godrays_lighttex_intensity;
extern cvar_t r_godrays_emissive_threshold;
extern cvar_t r_godrays_light_threshold;
extern cvar_t r_godrays_mask_knee;
extern cvar_t r_godrays_blur;
extern cvar_t r_godrays_lighttex_name_match;
extern cvar_t r_godrays_samples;
extern cvar_t r_godrays_density;
extern cvar_t r_godrays_weight;
extern cvar_t r_godrays_decay;
extern cvar_t r_godrays_exposure;
extern cvar_t r_godrays_threshold;
extern cvar_t r_godrays_sky_softness;
extern cvar_t r_godrays_light_sharpness;
extern cvar_t r_godrays_max_radius;
extern cvar_t r_godrays_light_x;
extern cvar_t r_godrays_light_y;
extern cvar_t r_godrays_stabilize;
extern cvar_t r_godrays_stabilize_strength;
extern cvar_t r_godrays_stabilize_max_px;
extern cvar_t r_godrays_smooth_rate;
extern cvar_t r_godrays_max_shift;
extern cvar_t r_godrays_reset_on_teleport;
extern cvar_t r_godrays_debug;
extern cvar_t r_godrays_debug_source;
extern cvar_t r_godrays_volumetric;
extern cvar_t r_godrays_vol_pow;

static void R_GodraysMigrateLegacySkyToCanonical (cvar_t *unused)
{
	(void)unused;

	Cvar_SetQuick (&r_godrays_sky_enable, r_godray_sky_enable.string);
	Cvar_SetQuick (&r_godrays_sky_threshold, r_godray_sky_threshold.string);
	Cvar_SetQuick (&r_godrays_sky_intensity, r_godray_sky_intensity.string);
	Cvar_SetQuick (&r_godrays_sky_softness, r_godray_sky_blur.string);
}

void R_Godrays_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_godrays);
	Cvar_RegisterVariable (&r_godrays_emit_emissive);
	Cvar_RegisterVariable (&r_godrays_emit_lighttex);
	Cvar_RegisterVariable (&r_godray_sky_enable);
	Cvar_RegisterVariable (&r_godray_sky_threshold);
	Cvar_RegisterVariable (&r_godray_sky_intensity);
	Cvar_RegisterVariable (&r_godray_sky_blur);
	Cvar_RegisterVariable (&r_godrays_sky_enable);
	Cvar_RegisterVariable (&r_godrays_sky_threshold);
	Cvar_RegisterVariable (&r_godrays_sky_intensity);
	Cvar_RegisterVariable (&r_godrays_sky_tint);
	Cvar_RegisterVariable (&r_godrays_emissive_intensity);
	Cvar_RegisterVariable (&r_godrays_lighttex_intensity);
	Cvar_RegisterVariable (&r_godrays_emissive_threshold);
	Cvar_RegisterVariable (&r_godrays_light_threshold);
	Cvar_RegisterVariable (&r_godrays_mask_knee);
	Cvar_RegisterVariable (&r_godrays_blur);
	Cvar_RegisterVariable (&r_godrays_lighttex_name_match);
	Cvar_RegisterVariable (&r_godrays_samples);
	Cvar_RegisterVariable (&r_godrays_density);
	Cvar_RegisterVariable (&r_godrays_weight);
	Cvar_RegisterVariable (&r_godrays_decay);
	Cvar_RegisterVariable (&r_godrays_exposure);
	Cvar_RegisterVariable (&r_godrays_threshold);
	Cvar_RegisterVariable (&r_godrays_sky_softness);
	/* Legacy r_godray_sky_* aliases are parse/set shims that write canonical r_godrays_sky_* values. */
	Cvar_SetCallback (&r_godray_sky_enable, R_GodraysMigrateLegacySkyToCanonical);
	Cvar_SetCallback (&r_godray_sky_threshold, R_GodraysMigrateLegacySkyToCanonical);
	Cvar_SetCallback (&r_godray_sky_intensity, R_GodraysMigrateLegacySkyToCanonical);
	Cvar_SetCallback (&r_godray_sky_blur, R_GodraysMigrateLegacySkyToCanonical);
	/* Startup migration: values restored into legacy names seed canonical cvars once. */
	R_GodraysMigrateLegacySkyToCanonical (NULL);
	Cvar_RegisterVariable (&r_godrays_light_sharpness);
	Cvar_RegisterVariable (&r_godrays_max_radius);
	Cvar_RegisterVariable (&r_godrays_light_x);
	Cvar_RegisterVariable (&r_godrays_light_y);
	Cvar_RegisterVariable (&r_godrays_stabilize);
	Cvar_RegisterVariable (&r_godrays_stabilize_strength);
	Cvar_RegisterVariable (&r_godrays_stabilize_max_px);
	Cvar_RegisterVariable (&r_godrays_smooth_rate);
	Cvar_RegisterVariable (&r_godrays_max_shift);
	Cvar_RegisterVariable (&r_godrays_reset_on_teleport);
	Cvar_RegisterVariable (&r_godrays_debug);
	Cvar_RegisterVariable (&r_godrays_debug_source);
	Cvar_RegisterVariable (&r_godrays_volumetric);
	Cvar_RegisterVariable (&r_godrays_vol_pow);
}


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
