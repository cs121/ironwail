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

/*
 * Maximum fraction of entity ambient that dlights are allowed to displace.
 * Prevents LightColor - DLightColor from clipping to zero when dlights
 * are bright. Range [0, 1]. 0 = no protection (original behaviour).
 * 1 = ambient is never reduced by dlights (full additive).
 * 0.5 is a reasonable conservative default.
 *
 * Background: alias shaders compute
 *   ambient = max(LightColor - DLightColor, 0) * lighting
 * which can zero-out the ambient contribution entirely when a dlight is
 * close and bright. This cap preserves at least (1 - dlight_ambient_cap)
 * of the lightgrid ambient even under strong dlights.
 */
cvar_t	r_envlight_dlight_ambient_cap = { "r_envlight_dlight_ambient_cap", "0.5", CVAR_ARCHIVE };

/*
 * When non-zero, the world-fill baseline (lightgrid_params.w) is scaled down
 * by the local dlight luminance so that well-lit clusters don't double-up on
 * ambient fill. Range [0, 1]. 0 = disabled (original behaviour).
 */
cvar_t	r_envlight_dlight_fill_suppress = { "r_envlight_dlight_fill_suppress", "0.6", CVAR_ARCHIVE };

static qboolean r_envlight_compat_busy;

/* ------------------------------------------------------------------ */
/* Compat sync                                                          */
/* ------------------------------------------------------------------ */

static void R_EnvLight_CompatPushLegacyFromMaster (void)
{
	r_envlight_compat_busy = true;

	Cvar_SetValueQuick (&r_model_lightgrid,    r_envlight.value     > 0.f ? 1.f : 0.f);
	Cvar_SetValueQuick (&r_reflection_probes,  r_envlight_envmap.value > 0.f ? 1.f : 0.f);
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

/* ------------------------------------------------------------------ */
/* Registration                                                         */
/* ------------------------------------------------------------------ */

void R_EnvLight_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_envlight);
	Cvar_RegisterVariable (&r_envlight_entity_intensity);
	Cvar_RegisterVariable (&r_envlight_world_fill);
	Cvar_RegisterVariable (&r_envlight_sky_sh);
	Cvar_RegisterVariable (&r_envlight_envmap);
	Cvar_RegisterVariable (&r_envlight_indoor_dampen);
	Cvar_RegisterVariable (&r_envlight_dlight_ambient_cap);
	Cvar_RegisterVariable (&r_envlight_dlight_fill_suppress);

	Cvar_SetCallback (&r_envlight,                  R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_entity_intensity,  R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_world_fill,        R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_sky_sh,            R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_envmap,            R_EnvLight_MasterCallback);
	Cvar_SetCallback (&r_envlight_indoor_dampen,     R_EnvLight_MasterCallback);
	/* dlight interaction cvars don't need to propagate to legacy aliases */

	/* Legacy toggles remain available as compatibility aliases. */
	Cvar_RegisterVariable (&r_reflection_probes);
	Cvar_RegisterVariable (&r_lightgrid_directional);

	Cvar_SetCallback (&r_reflection_probes,    R_EnvLight_LegacyAliasCallback);
	Cvar_SetCallback (&r_model_lightgrid,      R_EnvLight_LegacyAliasCallback);
	Cvar_SetCallback (&r_lightgrid_directional, R_EnvLight_LegacyAliasCallback);

	R_EnvLight_CompatPushLegacyFromMaster ();
}

/* ------------------------------------------------------------------ */
/* Frame uniforms                                                       */
/* ------------------------------------------------------------------ */

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

/*
 * R_EnvLight_BuildFrameUniformsDLight
 *
 * Extended variant that factors in the per-frame aggregate dlight luminance
 * (sum of dlight contributions visible this frame, caller-supplied as
 * dlight_luma in [0, Overbright]).  Two effects:
 *
 *  1. world_fill suppression: lightgrid_params.w is reduced proportionally
 *     to dlight_luma so that bright clusters don't double-up on ambient fill.
 *     Controlled by r_envlight_dlight_fill_suppress.
 *
 *  2. lighting_params.w gets the dlight_ambient_cap baked in as the upper
 *     byte (bits 8-15 of a fp16 pair are not available here; instead we
 *     repurpose the unused high half of lighting_params[3] by packing the
 *     cap as a sub-value offset – see comment in shader).  Because Ironwail
 *     shaders currently only use lighting_params[3] for debug levels 0-6, we
 *     pack dlight_ambient_cap * 10 into the fractional part so the shader can
 *     recover it with fract(lighting_params[3]) * 10.
 *
 * If you don't have a per-frame dlight luma available, call the original
 * R_EnvLight_BuildFrameUniforms() instead.
 */
void R_EnvLight_BuildFrameUniformsDLight (vec4_t lighting_params, vec4_t lightgrid_params,
                                          float dlight_luma, float dlight_overbright)
{
	R_EnvLight_BuildFrameUniforms (lighting_params, lightgrid_params);

	/* --- world fill suppression ---------------------------------------- */
	const float fill_suppress = CLAMP (0.f, r_envlight_dlight_fill_suppress.value, 1.f);
	if (fill_suppress > 0.f && dlight_overbright > 0.f)
	{
		/* normalise dlight luma to [0,1] relative to Overbright headroom */
		const float dlight_n = CLAMP (0.f, dlight_luma / dlight_overbright, 1.f);
		lightgrid_params[3] *= 1.f - fill_suppress * dlight_n;
	}

	/* --- dlight ambient cap packing ------------------------------------ */
	/*
	 * Pack cap into the fractional part of lighting_params[3].
	 * lighting_params[3] carries r_lighting_debug (integer 0-6).
	 * Encoding: stored = floor(debug) + fract where fract = cap / 10.
	 * Shader recovers: cap = fract(lighting_params[3]) * 10.
	 * This is a lossless round-trip for cap values in [0, 1] at fp32
	 * precision (step ~0.0001).
	 */
	const float cap = CLAMP (0.f, r_envlight_dlight_ambient_cap.value, 1.f);
	const float debug_floor = (float)(int)lighting_params[3]; /* strip any prior frac */
	lighting_params[3] = debug_floor + cap * 0.1f;
}

/* ------------------------------------------------------------------ */
/* Entity ambient sampling                                              */
/* ------------------------------------------------------------------ */

qboolean R_EnvLight_SampleEntityAmbient (const entity_t *e, vec3_t out_rgb_linear, float *out_ao)
{
	const lightgrid_t *lg;
	lightgrid_probe_t  probe;
	vec3_t             sample_pos;
	float              ao        = 1.f;
	float              intensity;

	/* Always initialise outputs so callers never read garbage. */
	if (out_ao)
		*out_ao = 1.f;
	if (out_rgb_linear)
		VectorSet (out_rgb_linear, 0.f, 0.f, 0.f);

	if (!e || !out_rgb_linear)
		return false;

	/* Fast-path: both master and lightgrid must be active. */
	if (!(r_envlight.value > 0.f && r_model_lightgrid.value > 0.f))
		return false;

	lg = Lightgrid_Get ();
	if (!lg || !r_lightgrid.value)
		return false;

	VectorCopy (e->origin, sample_pos);

	/*
	 * Two-attempt probe: first try entity origin, then the midpoint of the
	 * bounding box top if the first sample is dark (probe.intensity == 0).
	 * This handles entities that sit flush on the floor where the lightgrid
	 * cell may be fully occluded.
	 */
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!Lightgrid_SampleProbe (lg, sample_pos, &probe))
			break;

		VectorCopy (probe.rgb, out_rgb_linear);
		ao = CLAMP (0.f, probe.ao, 1.f);

		/* Non-zero intensity or valid AO – good enough. */
		if (probe.intensity > 0.f || ao > 0.f)
			break;

		/* Only retry once, and only when model info is available. */
		if (attempt == 1 || !e->model)
			break;

		const float ofs = e->model->maxs[2] * 0.5f;
		if (ofs <= 0.f)
			break;

		sample_pos[2] += ofs;
	}

	intensity = CLAMP (0.f, r_envlight_entity_intensity.value, 1.f);

	/*
	 * Scale with a single multiply-by-scalar instead of three separate
	 * component multiplications.  VectorScale is a macro in Ironwail so this
	 * generates identical code; written out explicitly for clarity.
	 */
	out_rgb_linear[0] *= intensity;
	out_rgb_linear[1] *= intensity;
	out_rgb_linear[2] *= intensity;

	if (out_ao)
		*out_ao = ao;

	return (out_rgb_linear[0] + out_rgb_linear[1] + out_rgb_linear[2]) > 0.f || ao > 0.f;
}

/*
 * R_EnvLight_SampleEntityAmbientDLight
 *
 * Variant of SampleEntityAmbient that also returns the fraction of lightgrid
 * ambient that should survive after dlight subtraction (the "safe floor").
 *
 * The problem: alias shaders compute
 *   ambient_color = max(LightColor - DLightColor, 0) * shading
 * This can zero-out lightgrid ambient entirely when a dlight is very bright,
 * losing all environment colour.  The caller can use out_dlight_floor to
 * clamp how much DLightColor is allowed to subtract from LightColor before
 * passing them into the shader:
 *
 *   effective_dlight = min(DLightColor, LightColor * (1 - out_dlight_floor))
 *   LightColor_shader = LightColor          (unchanged)
 *   DLightColor_shader = effective_dlight   (capped)
 *
 * out_dlight_floor is in [0, 1].  0 means dlights may fully cancel ambient
 * (legacy behaviour). 1 means ambient is fully protected.
 *
 * Returns the same qboolean as R_EnvLight_SampleEntityAmbient.
 */
qboolean R_EnvLight_SampleEntityAmbientDLight (const entity_t *e,
                                               vec3_t out_rgb_linear,
                                               float *out_ao,
                                               float *out_dlight_floor)
{
	if (out_dlight_floor)
		*out_dlight_floor = 0.f;

	const qboolean result = R_EnvLight_SampleEntityAmbient (e, out_rgb_linear, out_ao);

	if (out_dlight_floor && result)
		*out_dlight_floor = CLAMP (0.f, r_envlight_dlight_ambient_cap.value, 1.f);

	return result;
}
