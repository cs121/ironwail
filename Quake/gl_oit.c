#include "quakedef.h"
#include "glquake.h"

#include "gl_oit.h"

framesetup_t framesetup;

void R_BeginTranslucency (void)
{
	static const float zeroes[4] = { 0.f, 0.f, 0.f, 0.f };
	static const float ones[4] = { 1.f, 1.f, 1.f, 1.f };

	GL_BeginGroup ("Translucent objects");

	if (R_GetEffectiveAlphaMode () == ALPHAMODE_OIT)
	{
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framesetup.oit_fbo);
		GL_ClearBufferfvFunc (GL_COLOR, 0, zeroes);
		GL_ClearBufferfvFunc (GL_COLOR, 1, ones);

		glEnable (GL_STENCIL_TEST);
		glStencilMask (2);
		glStencilFunc (GL_ALWAYS, 2, 2);
		glStencilOp (GL_KEEP, GL_KEEP, GL_REPLACE);
	}
}

void R_EndTranslucency (void)
{
	if (R_GetEffectiveAlphaMode () == ALPHAMODE_OIT)
	{
		GL_BeginGroup ("OIT resolve");

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framesetup.scene_fbo);
		{
			GLuint buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			GL_DrawBuffersFunc (2, buffers);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
		}

		glStencilFunc (GL_EQUAL, 2, 2);
		glStencilOp (GL_KEEP, GL_KEEP, GL_KEEP);

		GL_UseProgram (glprogs.oit_resolve[framebufs.scene.samples > 1]);
		{
			const unsigned state = GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0);
			RenderBackendPipelineDesc pipeline_desc;
			RenderBackendDynamicState dynamic_state;
			memset (&pipeline_desc, 0, sizeof (pipeline_desc));
			memset (&dynamic_state, 0, sizeof (dynamic_state));
			pipeline_desc.state_bits = state;
			dynamic_state.blend_state = state;
			dynamic_state.depth_state = state;
			dynamic_state.raster_state = state;
			R_Backend_BindPipeline (&pipeline_desc);
			R_Backend_SetDynamicState (&dynamic_state);
		}
		// Keep color attachment alpha-blended, but overwrite velocity/material mask.
		if (GL_BlendFunciFunc)
			GL_BlendFunciFunc (1, GL_ONE, GL_ZERO);
		GL_BindNative (GL_TEXTURE0, framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, framebufs.oit.accum_tex);
		GL_BindNative (GL_TEXTURE1, framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, framebufs.oit.revealage_tex);

		R_Backend_Draw (R_BACKEND_PRIMITIVE_TRIANGLES, 0, 3);

		glDisable (GL_STENCIL_TEST);

		GL_EndGroup ();
	}

	GL_EndGroup ();
}

