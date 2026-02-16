#include "quakedef.h"
#include "renderer/r_envlight.h"
#include "opengl/gl_lightgrid.h"

extern cvar_t r_model_lightgrid;
extern cvar_t r_reflection_probes;
extern cvar_t r_reflection_probe_debug;
extern cvar_t r_lightgrid_directional;
extern cvar_t r_lighting_debug;
extern cvar_t r_shadow_lightgrid;
extern cvar_t r_shadow_lightgrid_mode;
extern cvar_t r_shadows;
extern cvar_t r_shadow_sun;

/* Master switch for envlight features (entity ambient, world fill, sky SH, envmap). */
cvar_t	r_envlight = { "r_envlight", "1", CVAR_ARCHIVE };
/* Scales lightgrid-driven entity ambient (conservative by default). */
cvar_t	r_envlight_entity_intensity = { "r_envlight_entity_intensity", "0.35", CVAR_ARCHIVE };
/* Low baseline world fill packed into frame lightgrid params.w. */
cvar_t	r_envlight_world_fill = { "r_envlight_world_fill", "0.12", CVAR_ARCHIVE };
/* Enables lightweight sky SH contribution path (0/1 style toggle). */
cvar_t	r_envlight_sky_sh = { "r_envlight_sky_sh", "0.35", CVAR_ARCHIVE };
/* Envmap/reflection contribution master (default off for conservative visuals). */
cvar_t	r_envlight_envmap = { "r_envlight_envmap", "0", CVAR_ARCHIVE };
/* Dampens indoor envmap response used by alias shaders. */
cvar_t	r_envlight_indoor_dampen = { "r_envlight_indoor_dampen", "0.35", CVAR_ARCHIVE };

static qboolean r_envlight_compat_busy;

static void R_EnvLight_CompatPushLegacyFromMaster (void)
{
	r_envlight_compat_busy = true;

	Cvar_SetValueQuick (&r_model_lightgrid, r_envlight.value > 0.f ? 1.f : 0.f);
	Cvar_SetValueQuick (&r_reflection_probes, r_envlight_envmap.value > 0.f ? 1.f : 0.f);
	Cvar_SetValueQuick (&r_lightgrid_directional, r_envlight_sky_sh.value > 0.f ? 1.f : 0.f);

	r_envlight_compat_busy = false;
}

static void R_EnvLight_MasterCallback (cvar_t *var)
{
	(void)var;
	if (r_envlight_compat_busy)
		return;

	R_EnvLight_CompatPushLegacyFromMaster ();
}

static void R_EnvLight_LegacyAliasCallback (cvar_t *var)
{
	if (r_envlight_compat_busy)
		return;

	r_envlight_compat_busy = true;

	if (var == &r_reflection_probes)
	{
		Cvar_SetValueQuick (&r_envlight_envmap, var->value > 0.f ? 1.f : 0.f);
		if (var->value > 0.f)
			Cvar_SetValueQuick (&r_envlight, 1.f);
	}
	else if (var == &r_model_lightgrid)
	{
		Cvar_SetValueQuick (&r_envlight, var->value > 0.f ? 1.f : 0.f);
	}
	else if (var == &r_lightgrid_directional)
	{
		Cvar_SetValueQuick (&r_envlight_sky_sh, var->value > 0.f ? 1.f : 0.f);
		if (var->value > 0.f)
			Cvar_SetValueQuick (&r_envlight, 1.f);
	}

	r_envlight_compat_busy = false;
	R_EnvLight_CompatPushLegacyFromMaster ();
}

void R_EnvLight_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_envlight);
	Cvar_RegisterVariable (&r_envlight_entity_intensity);
	Cvar_RegisterVariable (&r_envlight_world_fill);
	Cvar_RegisterVariable (&r_envlight_sky_sh);
	Cvar_RegisterVariable (&r_envlight_envmap);
	Cvar_RegisterVariable (&r_envlight_indoor_dampen);

	Cvar_SetCallback (&r_envlight, R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_entity_intensity, R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_world_fill, R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_sky_sh, R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_envmap, R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_indoor_dampen, R_EnvLight_MasterCallback);

	/* Legacy toggles remain available as compatibility aliases. */
	Cvar_RegisterVariable (&r_reflection_probes);
	Cvar_RegisterVariable (&r_lightgrid_directional);

	/* Legacy toggles are compatibility aliases into envlight master controls. */
	Cvar_SetCallback (&r_reflection_probes, R_EnvLight_LegacyAliasCallback);
	Cvar_SetCallback (&r_model_lightgrid, R_EnvLight_LegacyAliasCallback);
	Cvar_SetCallback (&r_lightgrid_directional, R_EnvLight_LegacyAliasCallback);

	R_EnvLight_CompatPushLegacyFromMaster ();
}

void R_EnvLight_BuildFrameUniforms (vec4_t lighting_params, vec4_t lightgrid_params)
{
	const qboolean enabled = r_envlight.value > 0.f;

	lightgrid_params[0] = (enabled && R_LightgridEnabled ()) ? 1.f : 0.f;
	lightgrid_params[1] = (r_lightgrid_debug.value >= 2.f) ? 1.f : 0.f;
	lightgrid_params[2] =
		(r_shadows.value > 0.f && r_shadow_sun.value > 0.f && r_shadow_lightgrid.value > 0.f)
		? CLAMP (0.f, r_shadow_lightgrid_mode.value, 2.f)
		: 0.f;
	lightgrid_params[3] = CLAMP (0.f, r_envlight_world_fill.value, 1.f);

	lighting_params[0] = (enabled && r_envlight_envmap.value > 0.f) ? 1.f : 0.f;
	lighting_params[1] = CLAMP (0.f, r_reflection_probe_debug.value, 1.f);
	lighting_params[2] = (enabled && r_envlight_sky_sh.value > 0.f) ? 1.f : 0.f;
	lighting_params[3] = CLAMP (0.f, r_lighting_debug.value, 6.f);
}

qboolean R_EnvLight_SampleEntityAmbient (const entity_t *e, vec3_t out_rgb_linear, float *out_ao)
{
	const lightgrid_t *lg;
	lightgrid_probe_t probe;
	vec3_t sample_pos;
	float ao = 1.f;
	float intensity;

	if (out_ao)
		*out_ao = 1.f;
	if (out_rgb_linear)
		VectorSet (out_rgb_linear, 0.f, 0.f, 0.f);

	if (!e || !out_rgb_linear)
		return false;

	if (!(r_envlight.value > 0.f && r_model_lightgrid.value > 0.f))
		return false;

	lg = Lightgrid_Get ();
	if (!lg || !r_lightgrid.value)
		return false;

	VectorCopy (e->origin, sample_pos);

	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!Lightgrid_SampleProbe (lg, sample_pos, &probe))
			break;

		VectorCopy (probe.rgb, out_rgb_linear);
		ao = CLAMP (0.f, probe.ao, 1.f);
		if (probe.intensity > 0.f || ao > 0.f)
			break;

		if (attempt == 1 || !e->model)
			break;

		const float ofs = e->model->maxs[2] * 0.5f;
		if (ofs <= 0.f)
			break;

		sample_pos[2] += ofs;
	}

	intensity = CLAMP (0.f, r_envlight_entity_intensity.value, 1.f);
	out_rgb_linear[0] *= intensity;
	out_rgb_linear[1] *= intensity;
	out_rgb_linear[2] *= intensity;

	if (out_ao)
		*out_ao = ao;

	return (out_rgb_linear[0] + out_rgb_linear[1] + out_rgb_linear[2]) > 0.f || ao > 0.f;
}
