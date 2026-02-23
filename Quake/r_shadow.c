/*
Copyright (C) 2024

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#include "quakedef.h"
#include <float.h>
#include <time.h>

extern cvar_t gl_farclip;
extern cvar_t r_shadows;
extern cvar_t r_shadow_bias;
extern cvar_t r_shadow_normalbias;
extern cvar_t r_shadow_pcf;
extern cvar_t r_shadow_pcf_taps;
extern cvar_t r_shadow_debug;
extern cvar_t r_shadow_debug_dummytex;
extern cvar_t r_shadow_debug_source;
extern cvar_t r_shadow_dlights;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_dlight_distance;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_dlight_pcf_taps;
extern dlight_t *r_dlight_sources[DLIGHT_GPU_MAX];

static GLuint shadow_dlight_fbo;
static GLuint shadow_dlight_depth_tex;
static GLuint shadow_dlight_dummy_tex;
static GLuint shadow_dlight_debug_color_tex;
static int shadow_dlight_atlas_size;
static int shadow_dlight_tile_size;
static int shadow_dlight_tile_count;
static int shadow_dlight_selected_count;
static int shadow_dlight_light_indices[SHADOW_DLIGHT_MAX];
static GLuint shadow_receiver_tex;
static float shadow_receiver_viewproj[16];
static qboolean shadow_dlight_shadows_active_this_frame;
static int shadow_dlight_shadow_caster_count;
static int shadow_dlight_atlas_valid_frame_id = -1;
static int shadow_dlight_selected_slot = -1;

extern cvar_t r_shadow_log;
extern cvar_t r_shadow_log_rate;
extern cvar_t r_shadow_log_gl;
extern cvar_t r_shadow_log_dump;
extern cvar_t r_shadow_log_file;

#define SHDLOG_PREFIX "SHDLOG: "

typedef struct shadow_log_state_s {
	int frame;
	qboolean active;
	qboolean dump;
	qboolean file_enabled;
	FILE *file;
	int last_rate_frame;
	GLuint last_shadow_tex;
	GLenum last_shadow_compare_mode;
	GLint last_shadow_sampler_unit;
	GLint last_program;
} shadow_log_state_t;

static shadow_log_state_t shdlog;

static unsigned int R_Shadow_MatrixHash (const float matrix[16]);

static const char *R_Shadow_LogFBOStatusString (GLenum status)
{
	switch (status)
	{
	case GL_FRAMEBUFFER_COMPLETE: return "COMPLETE";
	case GL_FRAMEBUFFER_UNDEFINED: return "UNDEFINED";
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "INCOMPLETE_ATTACHMENT";
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "MISSING_ATTACHMENT";
	case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: return "INCOMPLETE_DRAW_BUFFER";
	case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: return "INCOMPLETE_READ_BUFFER";
	case GL_FRAMEBUFFER_UNSUPPORTED: return "UNSUPPORTED";
	case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: return "INCOMPLETE_MULTISAMPLE";
#ifdef GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS
	case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: return "INCOMPLETE_LAYER_TARGETS";
#endif
	default: return "UNKNOWN";
	}
}

static const char *R_Shadow_LogGLErrorString (GLenum err)
{
	switch (err)
	{
	case GL_NO_ERROR: return "GL_NO_ERROR";
	case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
	case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
	case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
	case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
	case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
	case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
	case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
	default: return "GL_UNKNOWN_ERROR";
	}
}

static void R_Shadow_LogWrite (const char *fmt, ...)
{
	va_list argptr;
	char msg[2048];
	if (!shdlog.active)
		return;
	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof (msg), fmt, argptr);
	va_end (argptr);
	Con_Printf (SHDLOG_PREFIX "%s", msg);
	if (shdlog.file_enabled && shdlog.file)
	{
		time_t now = time (NULL);
		struct tm *tmv = localtime (&now);
		if (tmv)
			fprintf (shdlog.file, "%04d-%02d-%02d %02d:%02d:%02d " SHDLOG_PREFIX "%s", tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min, tmv->tm_sec, msg);
		else
			fprintf (shdlog.file, SHDLOG_PREFIX "%s", msg);
		fflush (shdlog.file);
	}
}

static void R_Shadow_LogEnsureFile (void)
{
	char path[MAX_OSPATH];
	if (r_shadow_log_file.value <= 0.f)
	{
		if (shdlog.file)
		{
			fclose (shdlog.file);
			shdlog.file = NULL;
		}
		shdlog.file_enabled = false;
		return;
	}
	if (!shdlog.file)
	{
		q_snprintf (path, sizeof (path), "%s/%s", com_gamedir, "shadow_debug.log");
		shdlog.file = Sys_fopen (path, "at");
		if (!shdlog.file)
		{
			Con_Printf (SHDLOG_PREFIX "WARN could not open %s for append; using console only\n", path);
			shdlog.file_enabled = false;
			return;
		}
		setvbuf (shdlog.file, NULL, _IOLBF, 0);
	}
	shdlog.file_enabled = true;
}

static qboolean R_Shadow_LogMatrixHasBadValues (const float m[16], float *out_det)
{
	float det;
	float col_scale;
	int i;
	for (i = 0; i < 16; ++i)
		if (!isfinite (m[i]))
			return true;
	det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
	if (out_det)
		*out_det = det;
	// FIX: Use scale-relative threshold; the old absolute 1e-8 is a false positive
	// for large-world ortho shadow matrices covering thousands of Quake units.
	col_scale = sqrtf (m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
	if (col_scale < 1e-30f)
		return true;
	return fabsf (det) < (col_scale * col_scale * col_scale * 1e-6f);
}

static void R_Shadow_LogTextureParams (const char *tag, GLuint tex)
{
	GLint width, height, internal, maxlevel;
	GLint minf, magf, wraps, wrapt, cmode, cfunc;
	GLfloat border[4];
	// FIX Bug 4: GL_BindNative(TEXTURE0, tex) poisonierte den Unit-Cache und
	// lies die aktive GL-Unit auf TEXTURE0 haengen. Nachfolgende
	// glTexParameteri-Calls (z.B. SetTextureCompareStateForMode) landeten
	// dann auf der falschen Unit → shadow_dlight_depth_tex behielt GL_NONE.
	// Ausserdem: glGetTexParameteriv liest von der aktiven Unit, nicht TEXTURE0,
	// falls GL_BindNative einen Cache-Hit hatte → Log-Werte waren falsch.
	// FIX: Raw GL ohne Cache, aktive Unit sichern und restaurieren.
	GLint prev_active;
	GLint prev_binding_tex0;
	if (!tex)
	{
		R_Shadow_LogWrite ("%s tex=0 (unbound)\n", tag);
		return;
	}
	glGetIntegerv (GL_ACTIVE_TEXTURE, &prev_active);
	GL_ActiveTextureFunc (GL_TEXTURE0);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev_binding_tex0);
	glBindTexture (GL_TEXTURE_2D, tex);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wraps);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapt);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &cmode);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, &cfunc);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxlevel);
	glGetTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	// Restore TEXTURE0 binding and active unit (raw GL, no cache touch)
	glBindTexture (GL_TEXTURE_2D, (GLuint)prev_binding_tex0);
	GL_ActiveTextureFunc ((GLenum)prev_active);
	R_Shadow_LogWrite ("%s tex=%u size=%dx%d ifmt=0x%X min=0x%X mag=0x%X wrap=(0x%X,0x%X) compare=(0x%X,0x%X) maxlevel=%d border=(%.2f %.2f %.2f %.2f)\n",
		tag, tex, width, height, (unsigned)internal, (unsigned)minf, (unsigned)magf, (unsigned)wraps, (unsigned)wrapt, (unsigned)cmode, (unsigned)cfunc, maxlevel,
		border[0], border[1], border[2], border[3]);
	if (width <= 0 || height <= 0)
		R_Shadow_LogWrite ("WARN shadow texture has zero size\n");
	if (internal != GL_DEPTH_COMPONENT24 && internal != GL_DEPTH_COMPONENT32F && internal != GL_DEPTH_COMPONENT16)
		R_Shadow_LogWrite ("WARN unexpected depth internal format 0x%X\n", (unsigned)internal);
	if (cmode != GL_COMPARE_REF_TO_TEXTURE)
	{
		// GL_NONE compare mode is intentional when r_shadow_debug >= 3 (raw-depth visualisation).
		// Only warn if it's unexpected outside that debug mode.
		if (r_shadow_debug.value < 3.f)
			R_Shadow_LogWrite ("WARN unexpected texture compare mode 0x%X\n", (unsigned)cmode);
	}
	if (cfunc != (gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL))
		R_Shadow_LogWrite ("WARN unexpected texture compare func 0x%X expected=0x%X\n", (unsigned)cfunc, (unsigned)(gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL));
	shdlog.last_shadow_compare_mode = (GLenum)cmode;
}

static void R_Shadow_SetTextureCompareStateForMode (GLenum compare_mode)
{
	const GLenum compare_func = gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL;

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, compare_mode);
	if (compare_mode == GL_COMPARE_REF_TO_TEXTURE)
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, compare_func);
}

static void R_Shadow_SetTextureCompareState (void)
{
	R_Shadow_SetTextureCompareStateForMode (GL_COMPARE_REF_TO_TEXTURE);
}

static void R_Shadow_CreateDummyTexture (void)
{
	float depth_data[8 * 8];
	int x, y;

	if (shadow_dlight_dummy_tex)
		return;

	for (y = 0; y < 8; ++y)
	for (x = 0; x < 8; ++x)
		depth_data[y * 8 + x] = (((x ^ y) & 1) != 0) ? 0.85f : 0.15f;

	glGenTextures (1, &shadow_dlight_dummy_tex);
	GL_ActiveTextureFunc (GL_TEXTURE0);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_dlight_dummy_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 8, 8, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth_data);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	R_Shadow_SetTextureCompareState ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

void R_EnsureShadowSamplerState (GLuint texture)
{
	GLint previous_active;
	GLint previous_binding;

	if (!texture)
		return;

	glGetIntegerv (GL_ACTIVE_TEXTURE, &previous_active);
	GL_ActiveTextureFunc (GL_TEXTURE0);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous_binding);
	glBindTexture (GL_TEXTURE_2D, texture);
	R_Shadow_SetTextureCompareStateForMode (r_shadow_debug.value >= 3.f ? GL_NONE : GL_COMPARE_REF_TO_TEXTURE);
	glBindTexture (GL_TEXTURE_2D, (GLuint)previous_binding);
	GL_ActiveTextureFunc ((GLenum)previous_active);
}

static void R_Shadow_DebugValidateProgramSampler (const char *tag, const char *uniform_name, GLint expected_unit)
{
	GLint prog = 0;
	GLint loc;
	GLint value = -1;
	if ((r_shadow_log.value <= 0.f && r_shadow_log_dump.value <= 0.f))
		return;
	glGetIntegerv (GL_CURRENT_PROGRAM, &prog);
	if (prog <= 0)
		return;
	loc = GL_GetUniformLocationFunc ((GLuint)prog, uniform_name);
	if (loc < 0)
		return;
	GL_GetUniformivFunc ((GLuint)prog, loc, &value);
	if (value != expected_unit)
		R_Shadow_LogWrite ("WARN %s sampler uniform %s=%d expected=%d (prog=%d)\n", tag, uniform_name, value, expected_unit, prog);
}

void R_Shadow_DebugValidateBinding (const char *tag, GLenum texunit, GLuint expected_tex)
{
	GLint previous_active, previous_binding;
	GLint bound_tex, cmode, cfunc;
	if ((r_shadow_log.value <= 0.f && r_shadow_log_dump.value <= 0.f) || !expected_tex)
		return;

	glGetIntegerv (GL_ACTIVE_TEXTURE, &previous_active);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous_binding);
	GL_ActiveTextureFunc (texunit);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &bound_tex);
	R_Shadow_DebugValidateProgramSampler (tag, "ShadowMap", 5);
	if ((GLuint)bound_tex == expected_tex)
	{
		glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &cmode);
		glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, &cfunc);
		if (cmode != GL_COMPARE_REF_TO_TEXTURE || cfunc != (gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL))
			R_Shadow_LogWrite ("WARN %s sampler validate texunit=%d tex=%u compare=(0x%X,0x%X) expected=(0x%X,0x%X)\n",
				tag, (int)(texunit - GL_TEXTURE0), expected_tex, (unsigned)cmode, (unsigned)cfunc,
				(unsigned)GL_COMPARE_REF_TO_TEXTURE, (unsigned)(gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL));
	}
	else
	{
		R_Shadow_LogWrite ("WARN %s sampler validate texunit=%d expected_tex=%u bound_tex=%d\n",
			tag, (int)(texunit - GL_TEXTURE0), expected_tex, bound_tex);
	}
	GL_ActiveTextureFunc ((GLenum)previous_active);
	GL_BindNative ((GLenum)previous_active, GL_TEXTURE_2D, (GLuint)previous_binding);
}

static void R_Shadow_LogGLStage (const char *stage)
{
	GLenum err;
	if (r_shadow_log_gl.value <= 0.f)
		return;
	err = glGetError ();
	if (err != GL_NO_ERROR)
		R_Shadow_LogWrite ("GL %s error=0x%X (%s)\n", stage, (unsigned)err, R_Shadow_LogGLErrorString (err));
	else
		R_Shadow_LogWrite ("GL %s error=GL_NO_ERROR\n", stage);
}

static void R_Shadow_LogTextureUnitBindings (const char *tag)
{
	GLint prev_active, max_units;
	int i;

	glGetIntegerv (GL_ACTIVE_TEXTURE, &prev_active);
	glGetIntegerv (GL_MAX_TEXTURE_IMAGE_UNITS, &max_units);
	if (max_units < 1)
		max_units = 1;
	if (max_units > 16)
		max_units = 16;

	for (i = 0; i < max_units; ++i)
	{
		GLint tex2d = 0;
		GLint texcube = 0;
		GLint sampler = 0;
		GL_ActiveTextureFunc (GL_TEXTURE0 + i);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d);
		glGetIntegerv (GL_TEXTURE_BINDING_CUBE_MAP, &texcube);
#ifdef GL_SAMPLER_BINDING
		glGetIntegerv (GL_SAMPLER_BINDING, &sampler);
#endif
		if (tex2d != 0 || texcube != 0 || sampler != 0 || i == 5)
			R_Shadow_LogWrite ("%s texunit[%d] tex2d=%d texcube=%d sampler=%d\n", tag, i, tex2d, texcube, sampler);
	}

	GL_ActiveTextureFunc ((GLenum)prev_active);
}

static void R_Shadow_LogFramebufferAttachment (const char *tag, GLenum target, GLenum attachment, const char *label)
{
	GLint type = GL_NONE;
	GLint name = 0;
	GLenum err;

	GL_GetFramebufferAttachmentParameterivFunc (target, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	err = glGetError ();
	if (err != GL_NO_ERROR)
	{
		R_Shadow_LogWrite ("%s %s target=0x%X query failed err=0x%X (%s)\n", tag, label, (unsigned)target, (unsigned)err, R_Shadow_LogGLErrorString (err));
		return;
	}

	if (type == GL_NONE)
	{
		R_Shadow_LogWrite ("%s %s target=0x%X attachment=none\n", tag, label, (unsigned)target);
		return;
	}

	GL_GetFramebufferAttachmentParameterivFunc (target, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	R_Shadow_LogWrite ("%s %s target=0x%X type=0x%X object=%d\n", tag, label, (unsigned)target, (unsigned)type, name);
}

static GLenum R_Shadow_LogDepthAttachmentEnum (void)
{
	/*
	 * Shadow atlas FBOs are depth-only.
	 * Querying GL_DEPTH_STENCIL_ATTACHMENT on a depth-only FBO can raise
	 * GL_INVALID_OPERATION on some drivers, so keep this pinned to
	 * GL_DEPTH_ATTACHMENT for draw/read framebuffer attachment probes.
	 */
	return GL_DEPTH_ATTACHMENT;
}

static void R_Shadow_LogGlobalGLState (const char *tag)
{
	GLint draw_fbo, read_fbo, draw_buf, read_buf, rbo, vao, prog;
	GLint array_buf, elem_buf;
	GLint viewport[4], scissor[4], blend_src_rgb, blend_dst_rgb, blend_eq_rgb;
	GLint depth_func, cull_mode, front_face, polygon_mode[2];
	GLint stencil_func, stencil_ref, stencil_value_mask;
	GLint stencil_fail, stencil_zfail, stencil_zpass, stencil_write_mask;
	GLboolean colormask[4], depthmask;
	GLboolean depthtest, blend, cull, scissortest, stencilen;

	if (!shdlog.active)
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buf);
	glGetIntegerv (GL_READ_BUFFER, &read_buf);
	glGetIntegerv (GL_RENDERBUFFER_BINDING, &rbo);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vao);
	glGetIntegerv (GL_CURRENT_PROGRAM, &prog);
	glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &array_buf);
	glGetIntegerv (GL_ELEMENT_ARRAY_BUFFER_BINDING, &elem_buf);
	glGetIntegerv (GL_VIEWPORT, viewport);
	glGetIntegerv (GL_SCISSOR_BOX, scissor);
	glGetIntegerv (GL_BLEND_SRC_RGB, &blend_src_rgb);
	glGetIntegerv (GL_BLEND_DST_RGB, &blend_dst_rgb);
	glGetIntegerv (GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
	glGetIntegerv (GL_DEPTH_FUNC, &depth_func);
	glGetIntegerv (GL_CULL_FACE_MODE, &cull_mode);
	glGetIntegerv (GL_FRONT_FACE, &front_face);
	glGetIntegerv (GL_POLYGON_MODE, polygon_mode);
	glGetIntegerv (GL_STENCIL_FUNC, &stencil_func);
	glGetIntegerv (GL_STENCIL_REF, &stencil_ref);
	glGetIntegerv (GL_STENCIL_VALUE_MASK, &stencil_value_mask);
	glGetIntegerv (GL_STENCIL_FAIL, &stencil_fail);
	glGetIntegerv (GL_STENCIL_PASS_DEPTH_FAIL, &stencil_zfail);
	glGetIntegerv (GL_STENCIL_PASS_DEPTH_PASS, &stencil_zpass);
	glGetIntegerv (GL_STENCIL_WRITEMASK, &stencil_write_mask);
	glGetBooleanv (GL_COLOR_WRITEMASK, colormask);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &depthmask);
	depthtest = glIsEnabled (GL_DEPTH_TEST);
	blend = glIsEnabled (GL_BLEND);
	cull = glIsEnabled (GL_CULL_FACE);
	scissortest = glIsEnabled (GL_SCISSOR_TEST);
	stencilen = glIsEnabled (GL_STENCIL_TEST);

	R_Shadow_LogWrite ("%s glstate prog=%d vao=%d vbo=%d ebo=%d draw_fbo=%d read_fbo=%d draw_buf=0x%X read_buf=0x%X rbo=%d viewport=(%d %d %d %d) scissor=(%d %d %d %d)\n",
		tag, prog, vao, array_buf, elem_buf, draw_fbo, read_fbo, (unsigned)draw_buf, (unsigned)read_buf, rbo,
		viewport[0], viewport[1], viewport[2], viewport[3], scissor[0], scissor[1], scissor[2], scissor[3]);
	R_Shadow_LogWrite ("%s glflags depth=(test:%d write:%d func:0x%X) blend=(en:%d src:0x%X dst:0x%X eq:0x%X) cull=(en:%d mode:0x%X front:0x%X) scissor=%d stencil=%d\n",
		tag, depthtest, depthmask, (unsigned)depth_func, blend, (unsigned)blend_src_rgb, (unsigned)blend_dst_rgb, (unsigned)blend_eq_rgb,
		cull, (unsigned)cull_mode, (unsigned)front_face, scissortest, stencilen);
	R_Shadow_LogWrite ("%s glmasks color=(%d %d %d %d) polygon_mode=(0x%X,0x%X) stencil=(func:0x%X ref:%d mask:0x%X fail:0x%X zfail:0x%X zpass:0x%X writemask:0x%X)\n",
		tag, colormask[0], colormask[1], colormask[2], colormask[3],
		(unsigned)polygon_mode[0], (unsigned)polygon_mode[1],
		(unsigned)stencil_func, stencil_ref, (unsigned)stencil_value_mask,
		(unsigned)stencil_fail, (unsigned)stencil_zfail, (unsigned)stencil_zpass, (unsigned)stencil_write_mask);

	R_Shadow_LogFramebufferAttachment (tag, GL_DRAW_FRAMEBUFFER, R_Shadow_LogDepthAttachmentEnum (), "draw_depth_attachment");
	R_Shadow_LogFramebufferAttachment (tag, GL_READ_FRAMEBUFFER, R_Shadow_LogDepthAttachmentEnum (), "read_depth_attachment");
	R_Shadow_LogTextureUnitBindings (tag);
}

void R_Shadow_Log_BeginFrame (void)
{
	qboolean enabled;
	int rate;
	if (shdlog.frame == r_framecount)
		return;
	shdlog.frame = r_framecount;
	enabled = r_shadow_log.value > 0.f;
	shdlog.dump = r_shadow_log_dump.value > 0.f;
	shdlog.active = false;
	if (!enabled && !shdlog.dump)
	{
		R_Shadow_LogEnsureFile ();
		return;
	}
	rate = (int)r_shadow_log_rate.value;
	if (rate < 1)
		rate = 1;
	if (shdlog.dump || (r_framecount - shdlog.last_rate_frame) >= rate)
	{
		shdlog.active = true;
		shdlog.last_rate_frame = r_framecount;
	}
	R_Shadow_LogEnsureFile ();

	// FIX: Flush any stale GL errors accumulated during the previous frame's
	// postprocessing (depth blits, composite passes, etc.) so they do not
	// misleadingly appear as errors in the first shadow pass of this frame.
	// glGetError() is called in a loop because GL may queue multiple errors.
	if (r_shadow_log_gl.value > 0.f)
	{
		GLenum flush_err;
		int flush_count = 0;
		while ((flush_err = glGetError ()) != GL_NO_ERROR && flush_count++ < 32)
		{
			if (shdlog.active)
				R_Shadow_LogWrite ("frame-start flushed stale GL error 0x%X (%s)\n",
					(unsigned)flush_err, R_Shadow_LogGLErrorString (flush_err));
		}
	}

	if (shdlog.active)
	{
		R_Shadow_LogWrite ("----- frame=%d map=%s vieworg=(%.1f %.1f %.1f) viewangles=(%.1f %.1f %.1f) -----\n",
			r_framecount,
			cl.mapname[0] ? cl.mapname : "(nomap)",
			r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2],
			r_refdef.viewangles[0], r_refdef.viewangles[1], r_refdef.viewangles[2]);
		R_Shadow_LogGlobalGLState ("FRAME_BEGIN");
	}
}

static void R_Shadow_LogEndFrameIfNeeded (void)
{
	if (shdlog.dump)
		Cvar_SetValueQuick (&r_shadow_log_dump, 0.f);
}

void R_Shadow_Log_DlightEarlyOut (const char *reason)
{
	R_Shadow_Log_BeginFrame ();
	if (!shdlog.active)
		return;
	R_Shadow_LogWrite ("DLIGHT skip: %s\n", reason);
}

void R_Shadow_Log_ShadowPassSnapshot (const char *tag, GLuint fbo, GLuint depth_tex, int vpw, int vph, int drawcalls, int tris, double msec)
{
	GLint viewport[4], scissor[4], draw_fbo, read_fbo, depthfunc, cullmode, prog;
	GLboolean scissoren, depthtest, depthwrite, cullen, po;
	GLfloat pof, pou, depthclear;
	GLenum status;
	R_Shadow_Log_BeginFrame ();
	if (!shdlog.active)
		return;
	status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
	glGetIntegerv (GL_VIEWPORT, viewport);
	glGetIntegerv (GL_SCISSOR_BOX, scissor);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_CURRENT_PROGRAM, &prog);
	depthtest = glIsEnabled (GL_DEPTH_TEST);
	depthwrite = 0;
	glGetBooleanv (GL_DEPTH_WRITEMASK, &depthwrite);
	glGetIntegerv (GL_DEPTH_FUNC, &depthfunc);
	cullen = glIsEnabled (GL_CULL_FACE);
	glGetIntegerv (GL_CULL_FACE_MODE, &cullmode);
	po = glIsEnabled (GL_POLYGON_OFFSET_FILL);
	glGetFloatv (GL_POLYGON_OFFSET_FACTOR, &pof);
	glGetFloatv (GL_POLYGON_OFFSET_UNITS, &pou);
	glGetFloatv (GL_DEPTH_CLEAR_VALUE, &depthclear);
	scissoren = glIsEnabled (GL_SCISSOR_TEST);
	R_Shadow_LogWrite ("%s enter/exit dt=%.3fms draw_fbo=%d read_fbo=%d fbo=%u check=0x%X(%s) viewport=(%d %d %d %d) scissor_en=%d scissor=(%d %d %d %d) shadow_vp=%dx%d prog=%d draws=%d tris=%d clearDepth=%.3f\n",
		tag, (float)msec, draw_fbo, read_fbo, (unsigned)fbo, (unsigned)status, R_Shadow_LogFBOStatusString (status),
		viewport[0], viewport[1], viewport[2], viewport[3], scissoren, scissor[0], scissor[1], scissor[2], scissor[3], vpw, vph, prog, drawcalls, tris, depthclear);
	R_Shadow_LogWrite ("%s state depth_test=%d depth_write=%d depth_func=0x%X cull=%d cull_mode=0x%X polyoffset=%d factor=%.4f units=%.4f\n",
		tag, depthtest, depthwrite, (unsigned)depthfunc, cullen, (unsigned)cullmode, po, pof, pou);
	R_Shadow_LogGlobalGLState (tag);
	R_Shadow_LogTextureParams (tag, depth_tex);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		R_Shadow_LogWrite ("WARN shadow FBO incomplete for %s\n", tag);
	R_Shadow_LogGLStage (tag);
}

void R_Shadow_Log_ReceiverPassSnapshot (const char *tag, int program, GLint cached_current_program, GLenum texunit, GLuint expected_tex, qboolean shadows_enabled, float bias, float normalbias, float pcf, float taps, const float *shadow_viewproj)
{
	GLint active_tex, bound_tex, draw_fbo, read_fbo, current_program;
	GLint vp[4], sc[4];
	float det = 0.f;
	qboolean bad;
	R_Shadow_Log_BeginFrame ();
	if (!shdlog.active)
		return;
	glGetIntegerv (GL_ACTIVE_TEXTURE, &active_tex);
	GL_ActiveTextureFunc (texunit);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &bound_tex);
	GL_ActiveTextureFunc (active_tex);
#if defined(SHDLOG)
	glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
#else
	current_program = cached_current_program;
	if (r_shadow_debug.value > 0.f)
		glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
#endif
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, vp);
	glGetIntegerv (GL_SCISSOR_BOX, sc);
	bad = shadow_viewproj ? R_Shadow_LogMatrixHasBadValues (shadow_viewproj, &det) : true;
	if (!shdlog.dump && shdlog.last_program == program && shdlog.last_shadow_sampler_unit == (GLint)(texunit - GL_TEXTURE0) && shdlog.last_shadow_tex == expected_tex && (GLuint)bound_tex == expected_tex)
	{
		R_Shadow_LogEndFrameIfNeeded ();
		return;
	}
	if (shadow_dlight_selected_slot >= 0 && shadow_dlight_selected_slot < SHADOW_DLIGHT_MAX)
	{
		R_Shadow_LogWrite ("%s receiver_select slot=%d light=%d tile=(%.3f %.3f %.3f %.3f) matrix_hash=%08X\n",
			tag, shadow_dlight_selected_slot, shadow_dlight_light_indices[shadow_dlight_selected_slot],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][0],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][1],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][2],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][3],
			shadow_viewproj ? R_Shadow_MatrixHash (shadow_viewproj) : 0u);
	}
	shdlog.last_program = program;
	shdlog.last_shadow_sampler_unit = (GLint)(texunit - GL_TEXTURE0);
	shdlog.last_shadow_tex = expected_tex;
	R_Shadow_LogWrite ("%s receiver program=%d cached_current=%d gl_current=%d source=%s enable=%d texunit=%d expected_tex=%u bound_tex=%d draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) scissor=(%d %d %d %d) casters=%d atlas_valid_frame=%d frame=%d selected_slot=%d\n",
		tag, program, cached_current_program, current_program,
		"dlight_atlas",
		shadows_enabled, (int)(texunit - GL_TEXTURE0), expected_tex, bound_tex, draw_fbo, read_fbo, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3],
		shadow_dlight_shadow_caster_count, shadow_dlight_atlas_valid_frame_id, r_framecount, shadow_dlight_selected_slot);
	R_Shadow_LogWrite ("%s params bias=%.6f normalbias=%.6f pcf=%.1f taps=%.1f matrix_col0=(%g %g %g %g) matrix_col1=(%g %g %g %g) matrix_col2=(%g %g %g %g) matrix_col3=(%g %g %g %g) det3x3=%.6g\n",
		tag, bias, normalbias, pcf, taps,
		shadow_viewproj ? shadow_viewproj[0] : 0.f,
		shadow_viewproj ? shadow_viewproj[1] : 0.f,
		shadow_viewproj ? shadow_viewproj[2] : 0.f,
		shadow_viewproj ? shadow_viewproj[3] : 0.f,
		shadow_viewproj ? shadow_viewproj[4] : 0.f,
		shadow_viewproj ? shadow_viewproj[5] : 0.f,
		shadow_viewproj ? shadow_viewproj[6] : 0.f,
		shadow_viewproj ? shadow_viewproj[7] : 0.f,
		shadow_viewproj ? shadow_viewproj[8] : 0.f,
		shadow_viewproj ? shadow_viewproj[9] : 0.f,
		shadow_viewproj ? shadow_viewproj[10] : 0.f,
		shadow_viewproj ? shadow_viewproj[11] : 0.f,
		shadow_viewproj ? shadow_viewproj[12] : 0.f,
		shadow_viewproj ? shadow_viewproj[13] : 0.f,
		shadow_viewproj ? shadow_viewproj[14] : 0.f,
		shadow_viewproj ? shadow_viewproj[15] : 0.f,
		det);
	if (!shadows_enabled)
		R_Shadow_LogWrite ("WARN receiver shadow branch disabled\n");
	if ((GLuint)bound_tex != expected_tex)
		R_Shadow_LogWrite ("WARN receiver shadow texture mismatch: expected=%u bound=%d\n", expected_tex, bound_tex);
	if (bad)
		R_Shadow_LogWrite ("WARN shadow matrix invalid (NaN/Inf or near-singular determinant)\n");
	if (bias < 1e-7f || bias > 0.1f)
		R_Shadow_LogWrite ("WARN suspicious shadow bias %.6f\n", bias);
	R_Shadow_LogGlobalGLState (tag);
	// NOTE: GL_NONE compare mode is intentional (manual shader compare); warning removed.
	R_Shadow_LogGLStage (tag);
	R_Shadow_LogEndFrameIfNeeded ();
}

static void R_Shadow_DestroyDlightResources (void)
{
	if (shadow_dlight_fbo)
	{
		GL_DeleteFramebuffersFunc (1, &shadow_dlight_fbo);
		shadow_dlight_fbo = 0;
	}
	if (shadow_dlight_depth_tex)
	{
		GL_DeleteNativeTexture (shadow_dlight_depth_tex);
		shadow_dlight_depth_tex = 0;
	}
	if (shadow_dlight_dummy_tex)
	{
		GL_DeleteNativeTexture (shadow_dlight_dummy_tex);
		shadow_dlight_dummy_tex = 0;
	}
	if (shadow_dlight_debug_color_tex)
	{
		GL_DeleteNativeTexture (shadow_dlight_debug_color_tex);
		shadow_dlight_debug_color_tex = 0;
	}
	shadow_dlight_atlas_size = 0;
	shadow_dlight_tile_size = 0;
	shadow_dlight_tile_count = 0;
}

static void R_Shadow_OrthoMatrix (float matrix[16], float left, float right, float bottom, float top, float n, float f)
{
	float rl = right - left;
	float tb = top - bottom;
	float fn = f - n;

	memset (matrix, 0, 16 * sizeof (float));

	if (rl == 0.f || tb == 0.f || fn == 0.f)
	{
		IdentityMatrix (matrix);
		return;
	}

	matrix[0 * 4 + 0] = 2.f / rl;
	matrix[1 * 4 + 1] = 2.f / tb;
	if (gl_clipcontrol_able)
	{
		matrix[2 * 4 + 2] = 1.f / (n - f);
		matrix[3 * 4 + 2] = n / (n - f);
	}
	else
	{
		matrix[2 * 4 + 2] = -2.f / fn;
		matrix[3 * 4 + 2] = -(f + n) / fn;
	}
	matrix[3 * 4 + 0] = -(right + left) / rl;
	matrix[3 * 4 + 1] = -(top + bottom) / tb;
	matrix[3 * 4 + 3] = 1.f;
}

static void R_Shadow_BuildViewMatrixColumns (float matrix[16], const vec3_t right, const vec3_t up, const vec3_t forward, const vec3_t origin)
{
	memset (matrix, 0, 16 * sizeof (float));

	/*
	 * Matrices are column-major (OpenGL convention, vector on the right):
	 * [ right.x   up.x   forward.x   tx ]
	 * [ right.y   up.y   forward.y   ty ]
	 * [ right.z   up.z   forward.z   tz ]
	 * [   0        0        0        1 ]
	 */
	matrix[0 * 4 + 0] = right[0];
	matrix[0 * 4 + 1] = right[1];
	matrix[0 * 4 + 2] = right[2];

	matrix[1 * 4 + 0] = up[0];
	matrix[1 * 4 + 1] = up[1];
	matrix[1 * 4 + 2] = up[2];

	matrix[2 * 4 + 0] = forward[0];
	matrix[2 * 4 + 1] = forward[1];
	matrix[2 * 4 + 2] = forward[2];

	matrix[3 * 4 + 0] = -DotProduct (right, origin);
	matrix[3 * 4 + 1] = -DotProduct (up, origin);
	matrix[3 * 4 + 2] = -DotProduct (forward, origin);
	matrix[3 * 4 + 3] = 1.f;
}

static void R_Shadow_DestroyResources (void)
{
	R_Shadow_DestroyDlightResources ();
}

static void R_Shadow_PerspectiveMatrix (float matrix[16], float fovx, float fovy, float n, float f)
{
	const float w = 1.0f / tanf (fovx * 0.5f);
	const float h = 1.0f / tanf (fovy * 0.5f);

	memset (matrix, 0, 16 * sizeof (float));

	if (gl_clipcontrol_able)
	{
		matrix[0 * 4 + 2] = -n / (f - n);
		matrix[0 * 4 + 3] = 1.f;
		matrix[1 * 4 + 0] = -w;
		matrix[2 * 4 + 1] = h;
		matrix[3 * 4 + 2] = f * n / (f - n);
	}
	else
	{
		matrix[0 * 4 + 2] = (f + n) / (f - n);
		matrix[0 * 4 + 3] = 1.f;
		matrix[1 * 4 + 0] = -w;
		matrix[2 * 4 + 1] = h;
		matrix[3 * 4 + 2] = -2.f * f * n / (f - n);
	}
}

static void R_Shadow_BuildDlightViewProj (float out_viewproj[16], const vec3_t origin, float radius)
{
	vec3_t target;
	vec3_t forward;
	vec3_t up = { 0.f, 0.f, 1.f };
	vec3_t right;
	vec3_t light_up;
	float view[16];
	float proj[16];
	float znear;
	float zfar;

	VectorCopy (r_refdef.vieworg, target);
	VectorSubtract (target, origin, forward);
	if (VectorNormalize (forward) == 0.f)
	{
		forward[0] = 0.f;
		forward[1] = 0.f;
		forward[2] = -1.f;
	}

	if (fabsf (DotProduct (forward, up)) > 0.95f)
	{
		up[0] = 0.f;
		up[1] = 1.f;
		up[2] = 0.f;
	}

	CrossProduct (up, forward, right);
	VectorNormalize (right);
	CrossProduct (forward, right, light_up);
	VectorNormalize (light_up);

	R_Shadow_BuildViewMatrixColumns (view, right, light_up, forward, origin);

	if (!isfinite (radius) || radius < 1.f)
		radius = 1.f;
	znear = 4.f;
	zfar = q_max (radius, znear + 1.f);
	if (zfar - znear < 1.f)
		zfar = znear + 1.f;
	R_Shadow_PerspectiveMatrix (proj, DEG2RAD (90.f), DEG2RAD (90.f), znear, zfar);

	memcpy (out_viewproj, proj, sizeof (proj));
	MatrixMultiply (out_viewproj, view);
}

static void R_Shadow_ResizeDlightAtlasIfNeeded (void)
{
	int max_tiles;
	int tile_size;
	int grid;
	int atlas_size;

	max_tiles = CLAMP (0, (int)r_shadow_dlight_max.value, SHADOW_DLIGHT_MAX);
	if (r_shadow_dlight_size.value <= 0.f || max_tiles <= 0)
	{
		if (shadow_dlight_depth_tex || shadow_dlight_fbo)
			R_Shadow_DestroyDlightResources ();
		return;
	}

	tile_size = (int)r_shadow_dlight_size.value;
	if (tile_size < 64)
		tile_size = 64;
	if (tile_size > gl_max_texture_size)
		tile_size = gl_max_texture_size;

	grid = 1;
	while (grid * grid < max_tiles)
		grid++;

	if (grid < 1)
		grid = 1;

	atlas_size = tile_size * grid;
	if (atlas_size > gl_max_texture_size)
	{
		tile_size = gl_max_texture_size / grid;
		if (tile_size < 64)
			tile_size = 64;
		atlas_size = tile_size * grid;
		if (atlas_size > gl_max_texture_size)
		{
			grid = 1;
			atlas_size = tile_size;
		}
	}

	if (shadow_dlight_depth_tex && shadow_dlight_fbo &&
		shadow_dlight_atlas_size == atlas_size &&
		shadow_dlight_tile_size == tile_size &&
		shadow_dlight_tile_count == grid * grid)
		return;

	if (shadow_dlight_depth_tex || shadow_dlight_fbo)
		R_Shadow_DestroyDlightResources ();

	glGenTextures (1, &shadow_dlight_depth_tex);
	GL_ActiveTextureFunc (GL_TEXTURE0);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_dlight_depth_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, shadow_dlight_depth_tex, -1, "shadowmap dlight depth");
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, atlas_size, atlas_size);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	{
		const float border[4] = { 1.f, 1.f, 1.f, 1.f };
		glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	R_Shadow_SetTextureCompareState ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	glGenTextures (1, &shadow_dlight_debug_color_tex);
	GL_ActiveTextureFunc (GL_TEXTURE0);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_dlight_debug_color_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, shadow_dlight_debug_color_tex, -1, "shadowmap dlight debug color");
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, GL_R8, atlas_size, atlas_size);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	GL_GenFramebuffersFunc (1, &shadow_dlight_fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_dlight_fbo);
	GL_ObjectLabelFunc (GL_FRAMEBUFFER, shadow_dlight_fbo, -1, "shadowmap dlight fbo");
	GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_dlight_depth_tex, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);

	{
		GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			Sys_Error ("Failed to create dlight shadowmap FBO (status code 0x%X)", status);
	}

	shadow_dlight_atlas_size = atlas_size;
	shadow_dlight_tile_size = tile_size;
	shadow_dlight_tile_count = grid * grid;
}
void R_InitShadow (void)
{
	shadow_dlight_fbo = 0;
	shadow_dlight_depth_tex = 0;
	shadow_dlight_dummy_tex = 0;
	shadow_dlight_debug_color_tex = 0;
	shadow_dlight_atlas_size = 0;
	shadow_dlight_tile_size = 0;
	shadow_dlight_tile_count = 0;
	shadow_dlight_selected_count = 0;
}

void R_ShutdownShadow (void)
{
	R_Shadow_DestroyResources ();
}

void R_ResizeShadowMapIfNeeded (void)
{
	R_Shadow_ResizeDlightAtlasIfNeeded ();
}

void R_Shadow_BindShadowMap (GLenum texunit)
{
	R_Shadow_BindDlightShadowMap (texunit);
}

GLuint R_Shadow_GetShadowMapTextureId (void)
{
	return shadow_dlight_depth_tex;
}

void R_Shadow_BindDlightShadowMap (GLenum texunit)
{
	R_Shadow_CreateDummyTexture ();
	if (r_shadow_debug_dummytex.value > 0.f && shadow_dlight_dummy_tex)
	{
		GL_ActiveTextureFunc (texunit);
		GL_BindNative (texunit, GL_TEXTURE_2D, shadow_dlight_dummy_tex);
		R_EnsureShadowSamplerState (shadow_dlight_dummy_tex);
	}
	else if (shadow_dlight_depth_tex)
	{
		GL_ActiveTextureFunc (texunit);
		GL_BindNative (texunit, GL_TEXTURE_2D, shadow_dlight_depth_tex);
		R_EnsureShadowSamplerState (shadow_dlight_depth_tex);
	}
	else
		GL_BindNative (texunit, GL_TEXTURE_2D, 0);
}

GLuint R_Shadow_GetDlightShadowMapTextureId (void)
{
	if (r_shadow_debug_dummytex.value > 0.f && shadow_dlight_dummy_tex)
		return shadow_dlight_dummy_tex;
	return shadow_dlight_depth_tex;
}

void R_Shadow_BindDebugColorAtlas (GLenum texunit)
{
	GL_ActiveTextureFunc (texunit);
	if (shadow_dlight_debug_color_tex)
		GL_BindNative (texunit, GL_TEXTURE_2D, shadow_dlight_debug_color_tex);
	else
		GL_BindNative (texunit, GL_TEXTURE_2D, 0);
}

GLuint R_Shadow_GetDebugColorAtlasTextureId (void)
{
	return shadow_dlight_debug_color_tex;
}

qboolean R_Shadow_DlightShadowsActiveThisFrame (void)
{
	return shadow_dlight_shadows_active_this_frame;
}

int R_Shadow_DlightAtlasValidFrameId (void)
{
	return shadow_dlight_atlas_valid_frame_id;
}

int R_Shadow_DlightShadowCasterCount (void)
{
	return shadow_dlight_shadow_caster_count;
}

static qboolean R_Shadow_IsDlightShadowCaster (int light_index, float *out_score)
{
	dlight_t *dl;
	const gpulight_t *glight;
	vec3_t delta;
	float dist;
	float score;

	if (light_index < 0 || (unsigned)light_index >= r_framedata.numlights)
		return false;

	dl = r_dlight_sources[light_index];
	if (!dl)
		return false;

	glight = &r_lightbuffer.lights[light_index];
	if (glight->radius <= 0.f)
		return false;

	VectorSubtract (glight->pos, r_refdef.vieworg, delta);
	dist = VectorLength (delta);
	if (r_shadow_dlight_distance.value > 0.f && dist > r_shadow_dlight_distance.value + glight->radius)
		return false;

	score = (glight->radius * (glight->color[0] + glight->color[1] + glight->color[2])) / (1.f + dist);
	if (out_score)
		*out_score = score;

	return true;
}

static unsigned int R_Shadow_MatrixHash (const float matrix[16])
{
	unsigned int h = 2166136261u;
	for (int i = 0; i < 16; ++i)
	{
		unsigned int v = (unsigned int)(fabsf (matrix[i]) * 1048576.f);
		h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
	}
	return h;
}

static void R_Shadow_SelectReceiverSource (GLuint tex, const float viewproj[16])
{
	shadow_receiver_tex = tex;
	if (viewproj)
		memcpy (shadow_receiver_viewproj, viewproj, sizeof (shadow_receiver_viewproj));
	else
		IdentityMatrix (shadow_receiver_viewproj);

#if defined(SHDLOG)
	if (r_shadow_debug.value > 0.f)
	{
		R_Shadow_LogWrite ("receiver_upload path=UBO/std140 matrix_layout=column_major transpose=GL_FALSE(implicit) matrix_hash=%08X\n",
			R_Shadow_MatrixHash (shadow_receiver_viewproj));
		R_Shadow_LogWrite ("receiver_upload m=[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]\n",
			shadow_receiver_viewproj[0], shadow_receiver_viewproj[1], shadow_receiver_viewproj[2], shadow_receiver_viewproj[3],
			shadow_receiver_viewproj[4], shadow_receiver_viewproj[5], shadow_receiver_viewproj[6], shadow_receiver_viewproj[7],
			shadow_receiver_viewproj[8], shadow_receiver_viewproj[9], shadow_receiver_viewproj[10], shadow_receiver_viewproj[11],
			shadow_receiver_viewproj[12], shadow_receiver_viewproj[13], shadow_receiver_viewproj[14], shadow_receiver_viewproj[15]);
	}
#endif

	r_framedata.shadow_debug[0] = tex ? 1.f : 0.f;
	r_framedata.shadow_params[0] = r_shadow_dlight_bias.value;
	r_framedata.shadow_params[1] = 0.f;
	r_framedata.shadow_params[2] = r_shadow_dlight_pcf_taps.value > 0.f ? 1.f : 0.f;
	r_framedata.shadow_params[3] = r_shadow_dlight_pcf_taps.value;
	r_framedata.shadow_dlight_params[2] = tex ? 1.f : 0.f;
}

void R_Shadow_BindReceiverShadowMap (GLenum texunit)
{
	if (!R_Shadow_ReceiverUsesDlight ())
	{
		GL_BindNative (texunit, GL_TEXTURE_2D, 0);
		return;
	}
	R_Shadow_BindDlightShadowMap (texunit);
}

GLuint R_Shadow_GetReceiverShadowMapTextureId (void)
{
	return shadow_receiver_tex;
}

const float *R_Shadow_GetReceiverShadowViewProj (void)
{
	return shadow_receiver_viewproj;
}

qboolean R_Shadow_ReceiverUsesDlight (void)
{
	return shadow_dlight_shadow_caster_count > 0 && shadow_receiver_tex != 0;
}

void R_Shadow_DlightPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadow_dlights.value > 0.f;
	double t0, t1;
	int draws0, tris0;
	R_Shadow_Log_BeginFrame ();
	float saved_viewproj[16];
	int max_tiles;
	int grid;
	int tiles_used = 0;

	shadow_dlight_selected_count = 0;
	shadow_dlight_selected_slot = -1;
	shadow_dlight_shadow_caster_count = 0;
	shadow_dlight_shadows_active_this_frame = false;

	for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
	{
		IdentityMatrix (r_framedata.shadow_dlight_viewproj[i]);
		r_framedata.shadow_dlight_atlas[i][0] = 0.f;
		r_framedata.shadow_dlight_atlas[i][1] = 0.f;
		r_framedata.shadow_dlight_atlas[i][2] = 0.f;
		r_framedata.shadow_dlight_atlas[i][3] = 0.f;
		r_framedata.shadow_dlight_info[i][0] = -1.f;
		r_framedata.shadow_dlight_info[i][1] = 0.f;
		r_framedata.shadow_dlight_info[i][2] = 0.f;
		r_framedata.shadow_dlight_info[i][3] = 0.f;
		shadow_dlight_light_indices[i] = -1;
	}

	if (!enabled)
	{
		R_Shadow_Log_DlightEarlyOut ("DLIGHTPASS disabled by cvars");
		return;
	}
	if (!glprogs.shadow_depth)
	{
		R_Shadow_Log_DlightEarlyOut ("DLIGHTPASS missing glprogs.shadow_depth");
		return;
	}

	R_Shadow_ResizeDlightAtlasIfNeeded ();
	if (!shadow_dlight_depth_tex || !shadow_dlight_fbo)
	{
		R_Shadow_Log_DlightEarlyOut ("DLIGHTPASS resources unavailable");
		return;
	}

	max_tiles = CLAMP (0, (int)r_shadow_dlight_max.value, SHADOW_DLIGHT_MAX);
	if (shadow_dlight_tile_count > 0)
		max_tiles = q_min (max_tiles, shadow_dlight_tile_count);
	if (max_tiles <= 0)
	{
		R_Shadow_Log_DlightEarlyOut ("DLIGHTPASS max_tiles <= 0");
		return;
	}

	if (r_framedata.numlights == 0)
	{
		shadow_dlight_atlas_valid_frame_id = -1;
		R_Shadow_SelectReceiverSource (0, NULL);
		R_Shadow_Log_DlightEarlyOut ("DLIGHTPASS no gpu lights");
		return;
	}

	{
		float scores[SHADOW_DLIGHT_MAX];
		for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
			scores[i] = -FLT_MAX;

		for (unsigned int i = 0; i < r_framedata.numlights; ++i)
		{
			float score = -FLT_MAX;
			if (!R_Shadow_IsDlightShadowCaster ((int)i, &score))
				continue;

			shadow_dlight_shadow_caster_count++;
			for (int slot = 0; slot < max_tiles; ++slot)
			{
				if (score > scores[slot])
				{
					for (int move = max_tiles - 1; move > slot; --move)
					{
						scores[move] = scores[move - 1];
						shadow_dlight_light_indices[move] = shadow_dlight_light_indices[move - 1];
					}
					scores[slot] = score;
					shadow_dlight_light_indices[slot] = (int)i;
					break;
				}
			}
		}
		shadow_dlight_shadows_active_this_frame = shadow_dlight_shadow_caster_count > 0;
		R_Shadow_LogWrite ("DLIGHTPASS casters=%d frame=%d atlas_valid_frame=%d\n",
			shadow_dlight_shadow_caster_count, r_framecount, shadow_dlight_atlas_valid_frame_id);
	}

	for (int i = 0; i < max_tiles; ++i)
	{
		if (shadow_dlight_light_indices[i] >= 0)
			tiles_used++;
	}
	shadow_dlight_selected_count = tiles_used;

	if (!shadow_dlight_shadows_active_this_frame || !tiles_used)
	{
		R_Shadow_SelectReceiverSource (0, NULL);
		shadow_dlight_atlas_valid_frame_id = -1;
		R_Shadow_Log_DlightEarlyOut (shadow_dlight_shadows_active_this_frame ? "DLIGHTPASS no selected lights" : "DLIGHTPASS no shadow casters");
		return;
	}

	memcpy (saved_viewproj, r_framedata.shadow_viewproj, sizeof (saved_viewproj));

	GL_BeginGroup ("Shadow map (dlights)");
	t0 = Sys_DoubleTime ();
	// FIX 3: draws0 is now an entity-count accumulator (reset to 0 here).
	draws0 = 0;
	tris0  = rs_brushpolys + rs_aliaspolys;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_dlight_fbo);
	if (r_shadow_debug_source.value >= 1.f)
	{
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_dlight_debug_color_tex, 0);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
	}
	else
	{
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_dlight_depth_tex, 0);
		glDrawBuffer (GL_NONE);
		glReadBuffer (GL_NONE);
	}
	// FIX: Same reversed-Z depth correction as SunPass.
	if (gl_clipcontrol_able)
	{
		glDepthFunc (GL_GREATER);
		glClearDepth (0.0);
	}
	else
	{
		glDepthFunc (GL_LESS);
		glClearDepth (1.0);
	}
	GL_SetScissorEnabled (true);

	grid = shadow_dlight_atlas_size / shadow_dlight_tile_size;
	if (grid < 1)
		grid = 1;

	for (int i = 0; i < max_tiles; ++i)
	{
		int light_index = shadow_dlight_light_indices[i];
		if (light_index < 0)
			continue;
		if (shadow_dlight_selected_slot < 0)
			shadow_dlight_selected_slot = i;

		const gpulight_t *glight = &r_lightbuffer.lights[light_index];
		float viewproj[16];
		int tile_x = i % grid;
		int tile_y = i / grid;
		float scale = (float)shadow_dlight_tile_size / (float)shadow_dlight_atlas_size;
		float offset_x = tile_x * scale;
		float offset_y = tile_y * scale;

		R_Shadow_BuildDlightViewProj (viewproj, glight->pos, glight->radius);
		memcpy (r_framedata.shadow_dlight_viewproj[i], viewproj, sizeof (viewproj));
		r_framedata.shadow_dlight_atlas[i][0] = scale;
		r_framedata.shadow_dlight_atlas[i][1] = scale;
		r_framedata.shadow_dlight_atlas[i][2] = offset_x;
		r_framedata.shadow_dlight_atlas[i][3] = offset_y;
		r_framedata.shadow_dlight_info[i][0] = (float)light_index;

		memcpy (r_framedata.shadow_viewproj, viewproj, sizeof (viewproj));
		R_UploadFrameData ();

		glViewport (tile_x * shadow_dlight_tile_size, tile_y * shadow_dlight_tile_size,
			shadow_dlight_tile_size, shadow_dlight_tile_size);
		glScissor (tile_x * shadow_dlight_tile_size, tile_y * shadow_dlight_tile_size,
			shadow_dlight_tile_size, shadow_dlight_tile_size);
		if (r_shadow_debug_source.value >= 1.f)
			glClear (GL_COLOR_BUFFER_BIT);
		else
			glClear (GL_DEPTH_BUFFER_BIT);

		if (r_shadow_debug_source.value >= 1.f)
		{
			GL_UseProgram (glprogs.shadow_debug);
			GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
			glDrawArrays (GL_TRIANGLES, 0, 3);
		}
		else
		{
			GL_UseProgram (glprogs.shadow_depth);
			GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_FRONT | GLS_ATTRIBS (6));

			{
				int count = 0;
				entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
				R_DrawBrushModels_Shadow (ents, count);
				draws0 += count;
			}
			{
				int count = 0;
				entity_t **ents = R_GetVisEntities (mod_alias, false, &count);
				R_DrawAliasModels_Shadow (ents, count);
				draws0 += count;
			}
		}
	}

	GL_SetScissorEnabled (false);
	t1 = Sys_DoubleTime ();
	R_Shadow_Log_ShadowPassSnapshot ("DLIGHTPASS", shadow_dlight_fbo, shadow_dlight_depth_tex, shadow_dlight_atlas_size, shadow_dlight_atlas_size,
		draws0,
		(rs_brushpolys  + rs_aliaspolys)  - tris0,
		(t1 - t0) * 1000.0);
	R_Shadow_LogWrite ("DLIGHTPASS selected=%d casters=%d atlas=%d tile=%d tile_count=%d cvar_max=%d\n", shadow_dlight_selected_count, shadow_dlight_shadow_caster_count, shadow_dlight_atlas_size, shadow_dlight_tile_size, shadow_dlight_tile_count, max_tiles);

	memcpy (r_framedata.shadow_viewproj, saved_viewproj, sizeof (saved_viewproj));
	if (shadow_dlight_selected_slot >= 0)
	{
		float receiver_viewproj[16];
		int selected_light_index = shadow_dlight_light_indices[shadow_dlight_selected_slot];
		memcpy (receiver_viewproj, r_framedata.shadow_dlight_viewproj[shadow_dlight_selected_slot], sizeof (receiver_viewproj));
		R_Shadow_SelectReceiverSource (r_shadow_debug_source.value >= 1.f ? shadow_dlight_debug_color_tex : shadow_dlight_depth_tex, receiver_viewproj);
		memcpy (r_framedata.shadow_viewproj, receiver_viewproj, sizeof (r_framedata.shadow_viewproj));
		shadow_dlight_atlas_valid_frame_id = r_framecount;
		R_Shadow_LogWrite ("DLIGHTPASS receiver slot=%d light=%d tile=(%.3f %.3f %.3f %.3f) matrix_hash=%08X atlas_valid_frame=%d frame=%d\n",
			shadow_dlight_selected_slot, selected_light_index,
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][0],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][1],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][2],
			r_framedata.shadow_dlight_atlas[shadow_dlight_selected_slot][3],
			R_Shadow_MatrixHash (receiver_viewproj),
			shadow_dlight_atlas_valid_frame_id, r_framecount);
	}
	R_UploadFrameData ();

	GL_EndGroup ();
}

void R_Shadow_DrawDebug (void)
{
	int mode = (int)r_shadow_debug.value;
	if (mode != 5)
		return;
	if (!glprogs.shadow_debug)
		return;
	if (!shadow_dlight_depth_tex)
		return;

	GL_BeginGroup ("Shadow map debug");

	GL_UseProgram (glprogs.shadow_debug);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_dlight_depth_tex);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}
