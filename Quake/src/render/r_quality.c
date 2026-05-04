#include "quakedef.h"

#include "r_quality.h"
#include "cl_postfx.h"

#include <math.h>
#include <stdlib.h>

extern cvar_t r_quality;

extern cvar_t r_shadow;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadow_dlight;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_sun_size;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_sun_distance;
extern cvar_t r_shadow_sun_pcf;
extern cvar_t r_shadow_dlight_pcf;
extern cvar_t r_shadow_receiver_bias;
extern cvar_t r_shadow_sun_snap;
extern cvar_t r_shadow_sun_cascades;
extern cvar_t r_shadow_sun_split1;
extern cvar_t r_shadow_sun_split2;
extern cvar_t r_shadow_sun_split3;
extern cvar_t r_shadow_sun_split_mode;
extern cvar_t r_shadow_sun_split_lambda;
extern cvar_t r_rimlight;
extern cvar_t r_rimlight_shadow;
extern cvar_t r_rimlight_intensity;
extern cvar_t r_rimlight_power;

extern cvar_t r_ssao;
extern cvar_t r_ssao_radius;
extern cvar_t r_ssao_intensity;
extern cvar_t r_ssao_bias;
extern cvar_t r_ssao_power;
extern cvar_t r_ssao_min;
extern cvar_t r_ssao_samples;
extern cvar_t r_ssao_halfres;
extern cvar_t r_ssao_blur;
extern cvar_t r_ssao_blur_radius;
extern cvar_t r_ssao_blur_sigma;
extern cvar_t r_ssao_blur_bilateral;
extern cvar_t r_ssao_noise;
extern cvar_t r_ssao_noise_scale;
extern cvar_t r_ssao_max_distance;
extern cvar_t r_ssao_fog_strength;
extern cvar_t r_ssao_fog_power;


extern cvar_t r_bloom;
extern cvar_t r_bloom_threshold;
extern cvar_t r_bloom_knee;
extern cvar_t r_bloom_quality;
extern cvar_t r_dof;
extern cvar_t r_dof_focus;
extern cvar_t r_dof_range;
extern cvar_t r_dof_strength;
extern cvar_t r_dof_autofocus;
extern cvar_t r_motionblur;
extern cvar_t r_motionblur_shutter;
extern cvar_t r_motionblur_maxradiuspixels;
extern cvar_t r_motionblur_maxsamples;
extern cvar_t r_tonemap;
extern cvar_t r_tonemap_exposure;
extern cvar_t r_autoexposure;
extern cvar_t r_exposure_bias;
extern cvar_t r_exposure_speed_up;
extern cvar_t r_exposure_speed_down;
extern cvar_t r_postfx;
extern cvar_t r_postfx_lut;
extern cvar_t r_postfx_underwater;
extern cvar_t r_postfx_damage;
extern cvar_t r_postfx_powerup;
extern cvar_t r_postfx_bloom_mode;
extern cvar_t r_oit;
extern cvar_t r_dither;
extern cvar_t r_lightmap16f;
extern cvar_t r_rgblighting_enable;
extern cvar_t r_lightingdir;
extern cvar_t r_srgb_framebuffer;

typedef enum r_quality_preset_e
{
	R_QUALITY_LOW = 0,
	R_QUALITY_MEDIUM,
	R_QUALITY_HIGH,
	R_QUALITY_ULTRA,
	R_QUALITY_CUSTOM,
	R_QUALITY_INVALID = -1
} r_quality_preset_t;

typedef struct quality_binding_s
{
	cvar_t *var;
	const char *values[4]; /* low, medium, high, ultra */
} quality_binding_t;

/* Table-driven renderer quality policy. Add new quality-managed cvars here. */
static quality_binding_t r_quality_bindings[] = {
	{ &r_shadow, { "0", "1", "1", "1" } },
	{ &r_shadow_sun, { "0", "1", "1", "1" } },
	{ &r_shadow_dlight, { "0", "0", "1", "1" } },
	{ &r_shadow_dlight_max, { "0", "2", "4", "4" } },
	{ &r_shadow_sun_size, { "0", "1024", "2048", "4096" } },
	{ &r_shadow_dlight_size, { "0", "256", "512", "1024" } },
	{ &r_shadow_sun_distance, { "900", "1200", "1400", "1600" } },
	{ &r_shadow_sun_pcf, { "0.0", "1.0", "1.5", "2.0" } },
	{ &r_shadow_dlight_pcf, { "0.0", "0.5", "0.75", "1.0" } },
	{ &r_shadow_receiver_bias, { "1.0", "1.5", "2.0", "2.5" } },
	{ &r_shadow_sun_snap, { "1", "1", "1", "1" } },
	{ &r_shadow_sun_cascades, { "1", "2", "3", "4" } },
	{ &r_shadow_sun_split1, { "0.25", "0.18", "0.12", "0.08" } },
	{ &r_shadow_sun_split2, { "0.60", "0.42", "0.35", "0.28" } },
	{ &r_shadow_sun_split3, { "0.90", "0.78", "0.70", "0.60" } },
	{ &r_shadow_sun_split_mode, { "1", "1", "3", "3" } },
	{ &r_shadow_sun_split_lambda, { "0.0", "0.25", "0.65", "0.9" } },
	{ &r_rimlight, { "0", "1", "1", "1" } },
	{ &r_rimlight_shadow, { "0", "0", "1", "1" } },
	{ &r_rimlight_intensity, { "0.3", "0.42", "0.48", "0.56" } },
	{ &r_rimlight_power, { "2.2", "2.8", "3.2", "3.8" } },
	{ &r_ssao, { "0", "1", "1", "1" } },
	{ &r_ssao_radius, { "16", "20", "24", "28" } },
	{ &r_ssao_intensity, { "0.9", "1.2", "1.5", "1.8" } },
	{ &r_ssao_bias, { "0.03", "0.025", "0.02", "0.015" } },
	{ &r_ssao_power, { "1.0", "1.2", "1.5", "1.8" } },
	{ &r_ssao_min, { "0.70", "0.62", "0.55", "0.50" } },
	{ &r_ssao_samples, { "6", "8", "12", "16" } },
	{ &r_ssao_halfres, { "1", "1", "1", "0" } },
	{ &r_ssao_blur, { "0", "1", "1", "1" } },
	{ &r_ssao_blur_radius, { "1", "1", "2", "3" } },
	{ &r_ssao_blur_sigma, { "1.2", "1.6", "2.0", "2.4" } },
	{ &r_ssao_blur_bilateral, { "1", "1", "1", "1" } },
	{ &r_ssao_noise, { "1", "1", "1", "1" } },
	{ &r_ssao_noise_scale, { "0.5", "0.75", "1.0", "1.25" } },
	{ &r_ssao_max_distance, { "768", "1024", "1280", "1536" } },
	{ &r_ssao_fog_strength, { "0.75", "0.9", "1.0", "1.1" } },
	{ &r_ssao_fog_power, { "0.8", "0.9", "1.0", "1.1" } },
	{ &r_bloom, { "0.75", "1.5", "3.0", "3.5" } },
	{ &r_bloom_threshold, { "1.25", "1.1", "1.0", "0.95" } },
	{ &r_bloom_knee, { "0.15", "0.25", "0.30", "0.40" } },
	{ &r_bloom_quality, { "0", "1", "1", "2" } },
	{ &r_dof, { "0", "0", "1", "1" } },
	{ &r_dof_focus, { "48", "56", "64", "72" } },
	{ &r_dof_range, { "192", "224", "255", "300" } },
	{ &r_dof_strength, { "1.5", "2.2", "3.0", "3.8" } },
	{ &r_dof_autofocus, { "1", "1", "1", "1" } },
	{ &r_motionblur, { "0", "0", "0", "1" } },
	{ &r_motionblur_shutter, { "0.55", "0.65", "0.75", "0.85" } },
	{ &r_motionblur_maxradiuspixels, { "16", "24", "32", "40" } },
	{ &r_motionblur_maxsamples, { "8", "12", "16", "20" } },
	{ &r_tonemap, { "1", "2", "2", "2" } },
	{ &r_tonemap_exposure, { "1.0", "1.0", "1.0", "1.05" } },
	{ &r_autoexposure, { "0", "1", "1", "1" } },
	{ &r_exposure_bias, { "0.95", "1.0", "1.0", "1.05" } },
	{ &r_exposure_speed_up, { "0.4", "0.5", "0.6", "0.7" } },
	{ &r_exposure_speed_down, { "0.2", "0.25", "0.3", "0.35" } },
	{ &r_postfx, { "1", "1", "1", "1" } },
	{ &r_postfx_lut, { "0", "1", "1", "1" } },
	{ &r_postfx_underwater, { "0", "1", "1", "1" } },
	{ &r_postfx_damage, { "1", "1", "1", "1" } },
	{ &r_postfx_powerup, { "1", "1", "1", "1" } },
	{ &r_postfx_bloom_mode, { "1", "1", "1", "1" } },
	{ &r_oit, { "0", "0", "1", "1" } },
	{ &r_dither, { "0.5", "0.75", "1.0", "1.0" } },
	{ &r_lightmap16f, { "0", "1", "1", "1" } },
	{ &r_rgblighting_enable, { "1", "1", "1", "1" } },
	{ &r_lightingdir, { "0", "0", "0", "1" } },
	{ &r_srgb_framebuffer, { "1", "1", "1", "1" } }
};

static qboolean r_quality_initialized = false;
static qboolean r_quality_applying = false;
static r_quality_preset_t r_quality_active = R_QUALITY_INVALID;

static void R_Quality_PostApplySync (void)
{
	/* Keep postfx/frame-dependent state deterministic after preset transitions.
	 * Users observed stale postfx artifacts that disappear after manual
	 * r_quality toggles; perform the same reset explicitly on apply. */
	CL_PostFX_Reset ();
	vid.recalc_refdef = true;
}

static const char *R_Quality_PresetName (r_quality_preset_t preset)
{
	switch (preset)
	{
	default:
	case R_QUALITY_CUSTOM: return "custom";
	case R_QUALITY_LOW: return "low";
	case R_QUALITY_MEDIUM: return "medium";
	case R_QUALITY_HIGH: return "high";
	case R_QUALITY_ULTRA: return "ultra";
	}
}

static r_quality_preset_t R_Quality_ParsePreset (const char *name)
{
	if (!name || !name[0])
		return R_QUALITY_INVALID;
	if (!q_strcasecmp (name, "low") || !strcmp (name, "0"))
		return R_QUALITY_LOW;
	if (!q_strcasecmp (name, "medium") || !strcmp (name, "1"))
		return R_QUALITY_MEDIUM;
	if (!q_strcasecmp (name, "high") || !strcmp (name, "2"))
		return R_QUALITY_HIGH;
	if (!q_strcasecmp (name, "ultra") || !strcmp (name, "3"))
		return R_QUALITY_ULTRA;
	if (!q_strcasecmp (name, "custom"))
		return R_QUALITY_CUSTOM;
	return R_QUALITY_INVALID;
}

static qboolean R_Quality_CvarMatches (const cvar_t *var, const char *value)
{
	char *end_a = NULL;
	char *end_b = NULL;
	double a;
	double b;

	if (!var || !value)
		return false;

	a = strtod (var->string, &end_a);
	b = strtod (value, &end_b);
	if (end_a && end_b && *end_a == '\0' && *end_b == '\0')
		return fabs (a - b) < 0.0001;

	return !strcmp (var->string, value);
}

static void R_Quality_ApplyPreset (r_quality_preset_t preset)
{
	int i;

	if (preset < R_QUALITY_LOW || preset > R_QUALITY_ULTRA)
		return;

	r_quality_applying = true;
	for (i = 0; i < (int)countof (r_quality_bindings); ++i)
	{
		const quality_binding_t *binding = &r_quality_bindings[i];
		const char *target = binding->values[preset];
		if (!binding->var || !target)
			continue;
		if (!R_Quality_CvarMatches (binding->var, target))
			Cvar_SetQuick (binding->var, target);
	}
	Cvar_SetQuick (&r_quality, R_Quality_PresetName (preset));
	r_quality_applying = false;
	r_quality_active = preset;
	R_Quality_PostApplySync ();
}

static qboolean R_Quality_HasManualOverrides (r_quality_preset_t preset)
{
	int i;

	if (preset < R_QUALITY_LOW || preset > R_QUALITY_ULTRA)
		return false;

	for (i = 0; i < (int)countof (r_quality_bindings); ++i)
	{
		const quality_binding_t *binding = &r_quality_bindings[i];
		const char *target = binding->values[preset];
		if (!binding->var || !target)
			continue;
		if (!R_Quality_CvarMatches (binding->var, target))
			return true;
	}

	return false;
}

void R_Quality_Init (void)
{
	r_quality_initialized = true;
	r_quality_active = R_QUALITY_INVALID;
	R_Quality_Update ();
	R_Quality_PostApplySync ();
}

void R_Quality_Update (void)
{
	r_quality_preset_t requested;

	if (!r_quality_initialized || r_quality_applying)
		return;

	requested = R_Quality_ParsePreset (r_quality.string);
	if (requested == R_QUALITY_INVALID)
	{
		if (r_quality_active != R_QUALITY_CUSTOM)
			Con_DWarning ("r_quality '%s' is invalid, switching to custom\n", r_quality.string);
		Cvar_SetQuick (&r_quality, "custom");
		r_quality_active = R_QUALITY_CUSTOM;
		return;
	}

	if (requested >= R_QUALITY_LOW && requested <= R_QUALITY_ULTRA)
	{
		if (requested != r_quality_active)
		{
			R_Quality_ApplyPreset (requested);
			return;
		}
		if (R_Quality_HasManualOverrides (requested))
		{
			Cvar_SetQuick (&r_quality, "custom");
			r_quality_active = R_QUALITY_CUSTOM;
		}
		return;
	}

	r_quality_active = requested;
}
