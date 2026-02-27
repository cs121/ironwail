#include "quakedef.h"
#include "rb_gl.h"

#if !RB_GL_PASSTHROUGH_ONLY
#error "RB wrappers must remain pass-through during this migration stage"
#endif

typedef struct rb_pass_info_s
{
	const char *debug_name;
	unsigned baseline_state;
} rb_pass_info_t;

/*
 * Render-backend pass baseline matrix (pass -> expected start state).
 *
 * | Pass               | Blend  | Depth test | Depth write | Cull | Program | Texture units 0..2 |
 * |--------------------|--------|------------|-------------|------|---------|--------------------|
 * | PASS_WORLD_OPAQUE  | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_DLIGHT        | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_SKY           | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_WATER_OPAQUE  | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_WATER_ALPHA   | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_ENTS_OPAQUE   | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_ENTS_ALPHA    | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_PARTICLES     | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_POSTFX        | Opaque | Off        | Off         | None | 0       | Unbound            |
 * | PASS_FOGVOL        | Opaque | Off        | Off         | None | 0       | Unbound            |
 * | PASS_UI2D          | Opaque | Off        | Off         | None | 0       | Unbound            |
 */
static const rb_pass_info_t rb_pass_info[PASS_COUNT] = {
	[PASS_WORLD_OPAQUE] = {"PASS_WORLD_OPAQUE", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_DLIGHT] = {"PASS_DLIGHT", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_SKY] = {"PASS_SKY", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_WATER_OPAQUE] = {"PASS_WATER_OPAQUE", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_WATER_ALPHA] = {"PASS_WATER_ALPHA", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_ENTS_OPAQUE] = {"PASS_ENTS_OPAQUE", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_ENTS_ALPHA] = {"PASS_ENTS_ALPHA", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_PARTICLES] = {"PASS_PARTICLES", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0)},
	[PASS_POSTFX] = {"PASS_POSTFX", GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0)},
	[PASS_FOGVOL] = {"PASS_FOGVOL", GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0)},
	[PASS_UI2D] = {"PASS_UI2D", GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0)},
};

static rb_pass_setup_hook_t rb_pass_setup_hook;
static qboolean rb_pass_active;

static void RB_ResetMinimalTextureBindings (void)
{
	GLint previous_active_tex;
	int i;

	glGetIntegerv (GL_ACTIVE_TEXTURE, &previous_active_tex);

	for (i = 0; i < 3; ++i)
	{
		GL_ActiveTextureFunc (GL_TEXTURE0 + i);
		glBindTexture (GL_TEXTURE_2D, 0);
	}
	GL_ActiveTextureFunc (previous_active_tex);
}

void RB_SetState (unsigned mask)
{
	GL_SetState (mask);
}

void RB_UseProgram (GLuint program)
{
	GL_UseProgram (program);
}

qboolean RB_BindTexture (GLenum texunit, gltexture_t *texture)
{
	return GL_Bind (texunit, texture);
}

void RB_BindFramebuffer (GLenum target, GLuint framebuffer)
{
	GL_BindFramebufferFunc (target, framebuffer);
}

void RB_DrawArrays (GLenum mode, GLint first, GLsizei count)
{
	glDrawArrays (mode, first, count);
}

void RB_Clear (GLbitfield mask)
{
	glClear (mask);
}

void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	glViewport (x, y, width, height);
}

void RB_SetPassSetupHook (rb_pass_setup_hook_t hook)
{
	rb_pass_setup_hook = hook;
}

void RB_BeginPass (rb_pass_t pass)
{
	if (pass < 0 || pass >= PASS_COUNT)
		pass = PASS_WORLD_OPAQUE;

	if (rb_pass_active)
		RB_EndPass ();

	GL_BeginGroup (rb_pass_info[pass].debug_name);
	RB_SetState (rb_pass_info[pass].baseline_state);
	RB_UseProgram (0);
	RB_ResetMinimalTextureBindings ();

	if (rb_pass_setup_hook)
		rb_pass_setup_hook (pass);

	rb_pass_active = true;
}

void RB_EndPass (void)
{
	if (!rb_pass_active)
		return;

	GL_EndGroup ();
	rb_pass_active = false;
}
