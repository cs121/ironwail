#include "quakedef.h"

#include "gl_dlight.h"

static void R_SetDlightConfig (GLuint program, float scale)
{
	if (!program)
		return;

	GL_UseProgram (program);
	GL_Uniform1fFunc (0, scale);
}

void R_DrawDLightPass (void)
{
	int count = 0;
	entity_t **ents;

	if (r_framedata.numlights == 0 || !r_drawworld_cheatsafe)
		return;

	ents = R_GetVisEntities (mod_brush, false, &count);
	if (count <= 0)
		return;

	GL_BeginGroup ("Dynamic lights (additive)");

	r_framedata.dlight_params[2] = 1.f;
	{
		GLuint buf;
		GLbyte *ofs;
		GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 0, buf, (GLintptr)ofs, sizeof (r_framedata));
	}

	R_SetDlightConfig (glprogs.world_dlight[0], 1.f);
	R_SetDlightConfig (glprogs.world_dlight[1], 1.f);

	R_DrawBrushModels_DLights (ents, count);

	r_framedata.dlight_params[2] = 0.f;
	{
		GLuint buf;
		GLbyte *ofs;
		GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 0, buf, (GLintptr)ofs, sizeof (r_framedata));
	}

	GL_EndGroup ();
}
