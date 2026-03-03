#include "quakedef.h"

#include "r_ssao.h"

#include <math.h>

extern cvar_t r_ssao;
extern cvar_t r_ssao_radius;
extern cvar_t r_ssao_intensity;
extern cvar_t r_ssao_bias;
extern cvar_t r_ssao_power;
extern cvar_t r_ssao_min;
extern cvar_t r_ssao_samples;
extern cvar_t r_ssao_blur;
extern cvar_t r_ssao_blur_radius;
extern cvar_t r_ssao_blur_sigma;
extern cvar_t r_ssao_blur_bilateral;
extern cvar_t r_ssao_halfres;
extern cvar_t r_ssao_debug;
extern cvar_t r_ssao_debug_far;
extern cvar_t r_ssao_reversedz_mode;
extern cvar_t r_ssao_noise;
extern cvar_t r_ssao_noise_mode;
extern cvar_t r_ssao_noise_scale;
extern cvar_t r_ssao_normalsource;
extern cvar_t r_ssao_freeze_noise;
extern cvar_t r_ssao_force_fullres;
extern cvar_t r_ssao_format;
extern cvar_t r_ssao_upscale_nearest;
extern cvar_t r_ssao_fog_strength;
extern cvar_t r_ssao_fog_power;
extern cvar_t r_ssao_max_distance;
extern cvar_t r_ssao_validate;

void R_SSAO_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_ssao);
	Cvar_RegisterVariable (&r_ssao_radius);
	Cvar_RegisterVariable (&r_ssao_intensity);
	Cvar_RegisterVariable (&r_ssao_bias);
	Cvar_RegisterVariable (&r_ssao_power);
	Cvar_RegisterVariable (&r_ssao_min);
	Cvar_RegisterVariable (&r_ssao_samples);
	Cvar_RegisterVariable (&r_ssao_blur);
	Cvar_RegisterVariable (&r_ssao_blur_radius);
	Cvar_RegisterVariable (&r_ssao_blur_sigma);
	Cvar_RegisterVariable (&r_ssao_blur_bilateral);
	Cvar_RegisterVariable (&r_ssao_halfres);
	Cvar_RegisterVariable (&r_ssao_debug);
	Cvar_RegisterVariable (&r_ssao_debug_far);
	Cvar_RegisterVariable (&r_ssao_reversedz_mode);
	Cvar_RegisterVariable (&r_ssao_noise);
	Cvar_RegisterVariable (&r_ssao_noise_mode);
	Cvar_RegisterVariable (&r_ssao_noise_scale);
	Cvar_RegisterVariable (&r_ssao_normalsource);
	Cvar_RegisterVariable (&r_ssao_freeze_noise);
	Cvar_RegisterVariable (&r_ssao_force_fullres);
	Cvar_RegisterVariable (&r_ssao_format);
	Cvar_RegisterVariable (&r_ssao_upscale_nearest);
	Cvar_RegisterVariable (&r_ssao_fog_strength);
	Cvar_RegisterVariable (&r_ssao_fog_power);
	Cvar_RegisterVariable (&r_ssao_max_distance);
	Cvar_RegisterVariable (&r_ssao_validate);
}


float R_SSAO_SanitizeValue (float value, float fallback, float minval, float maxval)
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

void R_SSAO_CaptureFogState (const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state)
{
	if (!out_state)
		return;

	if (!framedata)
	{
		VectorClear (out_state->color);
		out_state->density = 0.f;
		return;
	}

	out_state->color[0] = framedata->fogdata[0];
	out_state->color[1] = framedata->fogdata[1];
	out_state->color[2] = framedata->fogdata[2];
	out_state->density = framedata->fogdata[3];
}
