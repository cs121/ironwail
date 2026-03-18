#include "quakedef.h"

#include "r_fogvol.h"
#include "r_sanitize.h"
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
	return R_SanitizeFloatRange (value, fallback, minval, maxval);
}

void R_SSAO_CaptureFogState (const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state)
{
	vec3_t color;
	float density;

	(void)framedata;

	if (!out_state)
		return;

	if (!R_FogVol_GetGlobalFogState (color, &density))
	{
		VectorClear (out_state->color);
		out_state->density = 0.f;
		return;
	}

	out_state->color[0] = color[0];
	out_state->color[1] = color[1];
	out_state->color[2] = color[2];
	out_state->density = density;
}
