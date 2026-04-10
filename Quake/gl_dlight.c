#include "quakedef.h"
#include "glquake.h"

#include "gl_dlight.h"
#include "r_realtimelight.h"

extern cvar_t r_lighting_debug_view;

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
	entity_t **ents;
	unsigned int saved_numlights = r_framedata.numlights;
	gpulightbuffer_t saved_lightbuffer = {0};
	dlight_t *saved_sources[DLIGHT_GPU_MAX] = {0};
	int pp_count;
	const float world_scale = 1.f;
	const float luma_clamp = 1.f;
	const float soft_knee = 1.f;

	/*
	 * Single dynamic-lighting architecture:
	 * - Per-pixel world dlight pass always consumes shared frame lights.
	 * - No alternate source path.
	 */
	if (!r_drawworld_cheatsafe)
		return;
	if (CLAMP (0.f, r_lighting_debug_view.value, 9.f) > 0.f)
		return;

	ents = R_GetVisEntities (mod_brush, false, &count);
	if (count <= 0)
		return;

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

	GL_BeginGroup ("Dynamic lights (additive)");

	r_framedata.dlight_params[2] = 1.f;
	r_framedata.dlight_params[3] = 0.f;
	R_UploadFrameData ();

	R_SetDlightConfig (glprogs.world_dlight[0], world_scale, luma_clamp, soft_knee);
	R_SetDlightConfig (glprogs.world_dlight[1], world_scale, luma_clamp, soft_knee);

	R_DrawBrushModels_DLights (ents, count);

	r_framedata.dlight_params[2] = 0.f;
	r_framedata.dlight_params[3] = 0.f;
	R_UploadFrameData ();

	GL_EndGroup ();

	r_framedata.numlights = saved_numlights;
	memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
	memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
}
