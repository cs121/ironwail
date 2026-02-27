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
static rb_pass_t rb_current_pass = PASS_WORLD_OPAQUE;

#if !defined(NDEBUG) || defined(_DEBUG) || defined(DEBUG)
#define RB_STATE_TRACKER_ENABLED 1
#else
#define RB_STATE_TRACKER_ENABLED 0
#endif

#if RB_STATE_TRACKER_ENABLED
typedef struct rb_state_debug_s
{
	const char *last_blend_owner;
	const char *last_depth_owner;
	const char *last_cull_owner;
	const char *last_program_owner;
	const char *last_texture_owner[3];
} rb_state_debug_t;

static rb_state_debug_t rb_state_debug;

static const char *RB_DebugOwnerOrUnknown (const char *owner)
{
	return owner ? owner : "<unknown>";
}

static const char *RB_BlendName (unsigned state)
{
	switch (state & GLS_MASK_BLEND)
	{
	case GLS_BLEND_OPAQUE: return "opaque";
	case GLS_BLEND_ALPHA: return "alpha";
	case GLS_BLEND_ALPHA_OIT: return "alpha_oit";
	case GLS_BLEND_MULTIPLY: return "multiply";
	case GLS_BLEND_ADD: return "add";
	default: return "invalid";
	}
}

static const char *RB_CullName (unsigned state)
{
	switch (state & GLS_MASK_CULL)
	{
	case GLS_CULL_BACK: return "back";
	case GLS_CULL_NONE: return "none";
	case GLS_CULL_FRONT: return "front";
	default: return "invalid";
	}
}

static void RB_LogIncomingStateDiff (rb_pass_t pass, unsigned expected_state, unsigned current_state,
	GLint current_program, GLint tex2d_0, GLint tex2d_1, GLint tex2d_2)
{
	qboolean mismatch = false;

	if ((current_state & GLS_MASK_BLEND) != (expected_state & GLS_MASK_BLEND))
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  blend: expected=%s actual=%s last_owner=%s\n",
			RB_BlendName (expected_state),
			RB_BlendName (current_state),
			RB_DebugOwnerOrUnknown (rb_state_debug.last_blend_owner));
		mismatch = true;
	}

	if ((current_state & (GLS_NO_ZTEST | GLS_NO_ZWRITE)) != (expected_state & (GLS_NO_ZTEST | GLS_NO_ZWRITE)))
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  depth: expected=(ztest:%d zwrite:%d) actual=(ztest:%d zwrite:%d) last_owner=%s\n",
			(expected_state & GLS_NO_ZTEST) == 0,
			(expected_state & GLS_NO_ZWRITE) == 0,
			(current_state & GLS_NO_ZTEST) == 0,
			(current_state & GLS_NO_ZWRITE) == 0,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_depth_owner));
		mismatch = true;
	}

	if ((current_state & GLS_MASK_CULL) != (expected_state & GLS_MASK_CULL))
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  cull: expected=%s actual=%s last_owner=%s\n",
			RB_CullName (expected_state),
			RB_CullName (current_state),
			RB_DebugOwnerOrUnknown (rb_state_debug.last_cull_owner));
		mismatch = true;
	}

	if (current_program != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  program: expected=0 actual=%d last_owner=%s\n",
			(int)current_program,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_program_owner));
		mismatch = true;
	}

	if (tex2d_0 != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  texture[0]: expected=0 actual=%d last_owner=%s\n",
			(int)tex2d_0,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[0]));
		mismatch = true;
	}
	if (tex2d_1 != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  texture[1]: expected=0 actual=%d last_owner=%s\n",
			(int)tex2d_1,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[1]));
		mismatch = true;
	}
	if (tex2d_2 != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name);
		Con_Warning ("  texture[2]: expected=0 actual=%d last_owner=%s\n",
			(int)tex2d_2,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[2]));
		mismatch = true;
	}

	if (mismatch && r_rb_assert_state.value > 0.f)
		Sys_Error ("RB_BeginPass(%s): incoming state mismatch", rb_pass_info[pass].debug_name);
}
#endif

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

void RB_SetState_Owner (unsigned mask, const char *owner)
{
	#if RB_STATE_TRACKER_ENABLED
	unsigned changed = glstate ^ mask;

	if ((changed & GLS_MASK_BLEND) != 0)
		rb_state_debug.last_blend_owner = owner;
	if ((changed & (GLS_NO_ZTEST | GLS_NO_ZWRITE)) != 0)
		rb_state_debug.last_depth_owner = owner;
	if ((changed & GLS_MASK_CULL) != 0)
		rb_state_debug.last_cull_owner = owner;
	#endif

	GL_SetState (mask);
}

void RB_UseProgram_Owner (GLuint program, const char *owner)
{
	#if RB_STATE_TRACKER_ENABLED
	rb_state_debug.last_program_owner = owner;
	#endif

	GL_UseProgram (program);
}

qboolean RB_BindTexture_Owner (GLenum texunit, gltexture_t *texture, const char *owner)
{
	#if RB_STATE_TRACKER_ENABLED
	int texindex = (int)(texunit - GL_TEXTURE0);
	if (texindex >= 0 && texindex < (int)countof (rb_state_debug.last_texture_owner))
		rb_state_debug.last_texture_owner[texindex] = owner;
	#endif

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

void RB_DrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	glDrawElements (mode, count, type, indices);
}

void RB_Clear (GLbitfield mask)
{
	glClear (mask);
}

void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	glViewport (x, y, width, height);
}

void RB_Scissor (GLint x, GLint y, GLsizei width, GLsizei height)
{
	glScissor (x, y, width, height);
}

void RB_DepthFunc (GLenum func)
{
	glDepthFunc (func);
}

void RB_BlendFunc (GLenum sfactor, GLenum dfactor)
{
	glBlendFunc (sfactor, dfactor);
}

void RB_DrawBuffer (GLenum buf)
{
	glDrawBuffer (buf);
}

void RB_ReadBuffer (GLenum src)
{
	glReadBuffer (src);
}

void RB_SetPassSetupHook (rb_pass_setup_hook_t hook)
{
	rb_pass_setup_hook = hook;
}

void RB_BeginPass (rb_pass_t pass)
{
	GLint current_program = 0;
	GLint previous_active_tex = 0;
	GLint tex2d_0 = 0, tex2d_1 = 0, tex2d_2 = 0;

	if (pass < 0 || pass >= PASS_COUNT)
		pass = PASS_WORLD_OPAQUE;

	if (rb_pass_active)
		RB_EndPass ();

	#if RB_STATE_TRACKER_ENABLED
	if (r_gl_state_validate.value > 0.f)
	{
		glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
		glGetIntegerv (GL_ACTIVE_TEXTURE, &previous_active_tex);

		GL_ActiveTextureFunc (GL_TEXTURE0);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d_0);
		GL_ActiveTextureFunc (GL_TEXTURE1);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d_1);
		GL_ActiveTextureFunc (GL_TEXTURE2);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d_2);
		GL_ActiveTextureFunc (previous_active_tex);

		RB_LogIncomingStateDiff (pass, rb_pass_info[pass].baseline_state, glstate, current_program, tex2d_0, tex2d_1, tex2d_2);
	}
	#endif

	GL_BeginGroup (rb_pass_info[pass].debug_name);
	RB_SetState (rb_pass_info[pass].baseline_state);
	RB_UseProgram (0);
	RB_ResetMinimalTextureBindings ();

	if (rb_pass_setup_hook)
		rb_pass_setup_hook (pass);

	rb_current_pass = pass;
	rb_pass_active = true;
}

const char *RB_DebugStateOwnersString (void)
{
#if RB_STATE_TRACKER_ENABLED
	static char info[384];
	q_snprintf (info, sizeof (info),
		"pass=%s owner(blend=%s depth=%s cull=%s prog=%s tex0=%s tex1=%s tex2=%s)",
		rb_pass_info[rb_current_pass].debug_name,
		RB_DebugOwnerOrUnknown (rb_state_debug.last_blend_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_depth_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_cull_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_program_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[0]),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[1]),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[2]));
	return info;
#else
	return "pass=<tracker disabled>";
#endif
}

void RB_EndPass (void)
{
	if (!rb_pass_active)
		return;

	GL_EndGroup ();
	rb_pass_active = false;
}
