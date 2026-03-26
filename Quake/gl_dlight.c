#include "quakedef.h"

#include "gl_dlight.h"
#include "r_realtimelight.h"

extern cvar_t r_lighting_debug_view;

static void R_SetDlightConfig (GLuint program, float scale, float blend_mode)
{
	if (!program)
		return;

	GL_UseProgram (program);
	GL_Uniform1fFunc (0, scale);
	GL_Uniform1fFunc (1, blend_mode);
}

void R_DrawDLightPass (void)
{
	int count = 0;
	float pp_debug_mode = 0.f;
	entity_t **ents;
	qboolean use_shared_world_lights = R_PPdlights_WorldPathEnabled ();
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
		memcpy (&saved_lightbuffer, &r_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (saved_sources, r_dlight_sources, sizeof (saved_sources));
		pp_count = R_PPdlights_BuildWorldGpuLights (&r_lightbuffer, r_dlight_sources, DLIGHT_GPU_MAX);
		r_framedata.numlights = (unsigned int)pp_count;
		if (pp_count <= 0)
		{
			r_framedata.numlights = saved_numlights;
			memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
			memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
			return;
		}
		if (r_ppdlights_debug.value >= 1.f && (r_framecount % 60) == 0)
			Con_DPrintf ("r_ppdlights_world: active (lights=%d scale=%.3f)\n", pp_count,
				CLAMP (0.f, r_ppdlights_world_scale.value, 4.f));

		pp_debug_mode = CLAMP (0.f, r_ppdlights_debug_mode.value, 4.f);
		if (pp_debug_mode > 0.f && r_ppdlights_debug.value >= 1.f && (r_framecount % 60) == 0)
			Con_DPrintf ("r_ppdlights_world: debug mode %.0f active\n", pp_debug_mode);
	}

	GL_BeginGroup ("Dynamic lights (additive)");

	r_framedata.dlight_params[2] = 1.f;
	r_framedata.dlight_params[3] = pp_debug_mode;
	{
		GLuint buf;
		GLbyte *ofs;
		GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 0, buf, (GLintptr)ofs, sizeof (r_framedata));
	}

	{
		const float blend_mode = CLAMP (0.f, (float)Q_rint (r_ppdlights_world_blend.value), 1.f);
		R_SetDlightConfig (glprogs.world_dlight[0], 1.f, blend_mode);
		R_SetDlightConfig (glprogs.world_dlight[1], 1.f, blend_mode);
	}

	R_DrawBrushModels_DLights (ents, count);

	r_framedata.dlight_params[2] = 0.f;
	r_framedata.dlight_params[3] = 0.f;
	{
		GLuint buf;
		GLbyte *ofs;
		GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 0, buf, (GLintptr)ofs, sizeof (r_framedata));
	}

	GL_EndGroup ();

	if (use_shared_world_lights)
	{
		r_framedata.numlights = saved_numlights;
		memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
	}
}
