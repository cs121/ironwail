#include "quakedef.h"

#include "gl_dlight.h"
#include "r_realtimelight.h"

extern cvar_t r_lighting_debug_view;
cvar_t r_world_dlight_source = { "r_world_dlight_source", "1", CVAR_ARCHIVE };
cvar_t r_world_dlight_legacy_fallback = { "r_world_dlight_legacy_fallback", "1", CVAR_ARCHIVE };

static qboolean R_WorldDLightUseSharedPPPath (qboolean *out_allow_legacy_fallback)
{
	const int mode = CLAMP (0, (int)Q_rint (r_world_dlight_source.value), 2);
	const qboolean pp_collect_enabled = (r_ppdlights.value > 0.f);
	qboolean use_shared = false;
	qboolean allow_legacy_fallback = false;

	/* 0 = preserve legacy behavior (use dedicated world toggle),
	 * 1 = prefer PP frame lights for world/brush dlight pass,
	 * 2 = force legacy source list. */
	if (mode == 0)
		use_shared = R_PPdlights_WorldPathEnabled ();
	else if (mode == 1)
	{
		use_shared = pp_collect_enabled;
		allow_legacy_fallback = (r_world_dlight_legacy_fallback.value > 0.f);
	}
	else
		use_shared = false;

	if (out_allow_legacy_fallback)
		*out_allow_legacy_fallback = allow_legacy_fallback;

	return use_shared;
}

qboolean R_WorldDLightUsingPPPath (void)
{
	return R_WorldDLightUseSharedPPPath (NULL);
}

qboolean R_WorldDLightAllowLegacyFallback (void)
{
	qboolean allow_legacy_fallback = false;
	(void)R_WorldDLightUseSharedPPPath (&allow_legacy_fallback);
	return allow_legacy_fallback;
}

static void R_SetDlightConfig (GLuint program, float scale, float luma_clamp, float soft_knee)
{
	if (!program)
		return;

	GL_UseProgram (program);
	GL_Uniform1fFunc (0, scale);
	GL_Uniform1fFunc (1, luma_clamp);
	GL_Uniform1fFunc (2, soft_knee);
}

void R_DrawDLightPass (void)
{
	int count = 0;
	float pp_debug_mode = 0.f;
	entity_t **ents;
	qboolean allow_legacy_fallback = false;
	qboolean use_shared_world_lights = R_WorldDLightUseSharedPPPath (&allow_legacy_fallback);
	unsigned int saved_numlights = r_framedata.numlights;
	gpulightbuffer_t saved_lightbuffer = {0};
	dlight_t *saved_sources[DLIGHT_GPU_MAX] = {0};

	/*
	 * Shared-light architecture:
	 * - R_PPdlights_CollectFrame builds one frame list (dynamic + emissive).
	 * - This pass consumes the world/surface subset and repacks it into the
	 *   standard GPU light buffer expected by forward world shaders.
	 * - Alias/model and froxel passes read the same collected list separately.
	 */

	if (r_framedata.numlights == 0 || !r_drawworld_cheatsafe)
	{
		/* Optional per-pixel world path can still feed lights even if legacy list is empty. */
		if (!use_shared_world_lights)
			return;
	}
	if (CLAMP (0.f, r_ppdlights_world_scale.value, 4.f) <= 0.f)
		return;
	if (CLAMP (0.f, r_lighting_debug_view.value, 9.f) > 0.f)
		return;

	ents = R_GetVisEntities (mod_brush, false, &count);
	if (count <= 0)
		return;

	if (use_shared_world_lights)
	{
		int pp_count;
		rl_consumer_stats_t consumer_stats;
		memcpy (&saved_lightbuffer, &r_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (saved_sources, r_dlight_sources, sizeof (saved_sources));
		pp_count = R_PPdlights_BuildWorldGpuLights (&r_lightbuffer, r_dlight_sources, DLIGHT_GPU_MAX);
		r_framedata.numlights = (unsigned int)pp_count;
		if (pp_count <= 0)
		{
			r_framedata.numlights = saved_numlights;
			memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
			memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
			if (allow_legacy_fallback && saved_numlights > 0)
				use_shared_world_lights = false;
			else
				return;
		}
		if (use_shared_world_lights)
		{
			if (r_ppdlights_debug.value >= 1.f && (r_framecount % 60) == 0)
				Con_DPrintf ("r_ppdlights_world: active (lights=%d scale=%.3f)\n", pp_count,
					CLAMP (0.f, r_ppdlights_world_scale.value, 4.f));
			if (r_ppdlights_debug.value >= 2.f && (r_framecount % 60) == 0
				&& R_PPdlights_GetConsumerStats (RL_CONSUMER_WORLD, &consumer_stats))
			{
				Con_DPrintf ("r_ppdlights_world: considered=%d accepted=%d energy=%.3f reject(non_contrib=%d local_budget=%d)\n",
					consumer_stats.considered,
					consumer_stats.accepted,
					consumer_stats.accepted_energy,
					consumer_stats.rejected[RL_REJECT_NON_CONTRIB],
					consumer_stats.rejected[RL_REJECT_LOCAL_BUDGET]);
			}

			pp_debug_mode = CLAMP (0.f, r_ppdlights_debug_mode.value, 6.f);
			if (pp_debug_mode > 0.f && r_ppdlights_debug.value >= 1.f && (r_framecount % 60) == 0)
				Con_DPrintf ("r_ppdlights_world: debug mode %.0f active\n", pp_debug_mode);
		}
	}

	GL_BeginGroup ("Dynamic lights (additive)");

	r_framedata.dlight_params[2] = 1.f;
	r_framedata.dlight_params[3] = pp_debug_mode;
	R_UploadFrameData ();

	{
		const float world_scale = CLAMP (0.f, r_ppdlights_world_scale.value, 4.f);
		const float luma_clamp = CLAMP (0.f, r_ppdlights_world_luma_clamp.value, 16.f);
		const float soft_knee = CLAMP (0.05f, r_ppdlights_world_soft_knee.value, 8.f);
		R_SetDlightConfig (glprogs.world_dlight[0], world_scale, luma_clamp, soft_knee);
		R_SetDlightConfig (glprogs.world_dlight[1], world_scale, luma_clamp, soft_knee);
	}

	R_DrawBrushModels_DLights (ents, count);

	r_framedata.dlight_params[2] = 0.f;
	r_framedata.dlight_params[3] = 0.f;
	R_UploadFrameData ();

	GL_EndGroup ();

	if (use_shared_world_lights)
	{
		r_framedata.numlights = saved_numlights;
		memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
	}
}
