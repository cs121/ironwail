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

static void R_Shadow_LogClearDebug (const char *tag, GLbitfield clearbits)
{
	GLint draw_fbo, read_fbo;
	GLint viewport[4], scissor_box[4];
	GLboolean scissor_test;
	GLfloat clear_color[4];

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, viewport);
	scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetFloatv (GL_COLOR_CLEAR_VALUE, clear_color);

	Con_DPrintf (
		"CLEARDBG %s draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) scissor_test=%d scissor_box=(%d %d %d %d) clear_color=(%.3f %.3f %.3f %.3f) clear_mask=0x%08x\n",
		tag,
		draw_fbo,
		read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_test,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		clear_color[0], clear_color[1], clear_color[2], clear_color[3],
		(unsigned int)clearbits);
}

extern cvar_t gl_farclip;
extern cvar_t r_shadows;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadowmap_size;
extern cvar_t r_shadow_bias;
extern cvar_t r_shadow_normalbias;
extern cvar_t r_shadow_pcf;
extern cvar_t r_shadow_pcf_taps;
extern cvar_t r_shadow_debug;
extern cvar_t r_shadow_sun_dir;
extern cvar_t r_shadow_csm_debug;
extern cvar_t r_shadow_dlights;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_dlight_distance;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_dlight_pcf_taps;
extern cvar_t r_dlight_shadows;
extern cvar_t r_dlight_max;
extern dlight_t *r_dlight_sources[DLIGHT_GPU_MAX];

static GLuint shadow_fbo;
static GLuint shadow_depth_tex;
static int shadowmap_size;
static GLuint shadow_dlight_fbo;
static GLuint shadow_dlight_depth_tex;
static int shadow_dlight_atlas_size;
static int shadow_dlight_tile_size;
static int shadow_dlight_tile_count;
static int shadow_dlight_selected_count;
static int shadow_dlight_light_indices[SHADOW_DLIGHT_MAX];

extern cvar_t r_shadow_log;
extern cvar_t r_shadow_log_rate;
extern cvar_t r_shadow_log_gl;
extern cvar_t r_shadow_log_dump;
extern cvar_t r_shadow_log_file;
extern cvar_t r_shadow_validate;
extern cvar_t r_gl_verify_program;

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
static qboolean shadow_sun_validated_once;
static qboolean shadow_dlight_validated_once;
static qboolean shadow_warned_sun_dir_sanitize;
static qboolean shadow_warned_matrix_sanitize;
static char shadow_last_empty_sunpass_map[MAX_QPATH];

typedef struct shadow_program_ubo_info_s {
	GLuint program;
	GLuint block_index;
	GLint data_size;
	qboolean has_block;
	qboolean warned_missing;
	qboolean warned_size;
	qboolean logged_bind;
} shadow_program_ubo_info_t;

static shadow_program_ubo_info_t shadow_program_ubo_info[256];
static int shadow_program_ubo_info_count;

void R_Shadow_ResetUBOBindings (void)
{
	shadow_program_ubo_info_count = 0;
	memset (shadow_program_ubo_info, 0, sizeof (shadow_program_ubo_info));
}

static void R_Shadow_LogWrite (const char *fmt, ...);

static void R_Shadow_DumpActiveUniforms (GLuint program)
{
	GLint uniform_count = 0;
	GLint max_name_len = 0;
	int i;
	char *name;

	GL_GetProgramivFunc (program, GL_ACTIVE_UNIFORMS, &uniform_count);
	GL_GetProgramivFunc (program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_name_len);
	if (max_name_len < 1)
		max_name_len = 1;
	name = (char *)malloc ((size_t)max_name_len);
	if (!name)
		return;

	R_Shadow_LogWrite ("UNIFORMS program=%u count=%d\n", (unsigned)program, (int)uniform_count);
	for (i = 0; i < uniform_count; ++i)
	{
		GLsizei length = 0;
		GLint size = 0;
		GLenum type = 0;
		GL_GetActiveUniformFunc (program, (GLuint)i, (GLsizei)max_name_len, &length, &size, &type, name);
		R_Shadow_LogWrite ("UNIFORM program=%u index=%d name=%s type=0x%X size=%d\n", (unsigned)program, i, name, (unsigned)type, (int)size);
	}
	free (name);
}

static void R_Shadow_DumpActiveUniformBlocks (GLuint program)
{
	GLint block_count = 0;
	GLint max_name_len = 0;
	int i;
	char *name;

	GL_GetProgramivFunc (program, GL_ACTIVE_UNIFORM_BLOCKS, &block_count);
	if (block_count <= 0)
	{
		R_Shadow_LogWrite ("UNIFORM_BLOCKS program=%u count=0\n", (unsigned)program);
		return;
	}

	if (!GL_GetActiveUniformBlockNameFunc)
	{
		R_Shadow_LogWrite ("UNIFORM_BLOCKS program=%u unavailable (missing GL_GetActiveUniformBlockName)\n", (unsigned)program);
		return;
	}

	GL_GetProgramivFunc (program, GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH, &max_name_len);
	if (max_name_len < 1)
		max_name_len = 1;
	name = (char *)malloc ((size_t)max_name_len);
	if (!name)
		return;

	R_Shadow_LogWrite ("UNIFORM_BLOCKS program=%u count=%d\n", (unsigned)program, (int)block_count);
	for (i = 0; i < block_count; ++i)
	{
		GLsizei length = 0;
		GL_GetActiveUniformBlockNameFunc (program, (GLuint)i, (GLsizei)max_name_len, &length, name);
		R_Shadow_LogWrite ("UNIFORM_BLOCK program=%u index=%d name=%s\n", (unsigned)program, i, name);
	}
	free (name);
}

typedef struct shadow_gl_state_s {
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint viewport[4];
	GLint scissor[4];
	GLboolean scissor_test;
} shadow_gl_state_t;

static void R_Shadow_SaveGLState (shadow_gl_state_t *state)
{
	if (!state)
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &state->draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &state->read_buffer);
	glGetIntegerv (GL_VIEWPORT, state->viewport);
	glGetIntegerv (GL_SCISSOR_BOX, state->scissor);
	state->scissor_test = glIsEnabled (GL_SCISSOR_TEST);
}

static void R_Shadow_RestoreGLState (const shadow_gl_state_t *state)
{
	if (!state)
		return;

	GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, (GLuint)state->draw_fbo);
	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, (GLuint)state->read_fbo);
	glDrawBuffer ((GLenum)state->draw_buffer);
	glReadBuffer ((GLenum)state->read_buffer);
	glViewport (state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
	glScissor (state->scissor[0], state->scissor[1], state->scissor[2], state->scissor[3]);
	if (state->scissor_test)
		glEnable (GL_SCISSOR_TEST);
	else
		glDisable (GL_SCISSOR_TEST);
}

/*
Root Cause(s) + Fix Summary:
1) Shadow caster pass used front-face culling. Quake BSP caster geometry is effectively
   single-sided for shadow rendering; culling fronts dropped most/all casters, leaving
   an empty depth map and fully lit receivers.
2) Shadow map configuration and validation were spread out and easy to regress.
   Centralized depth texture setup and added optional runtime validation to catch
   FBO/texture/state mismatches early.
3) Logging assumed hardware depth-compare PCF, but the shader path is manual PCF
   with sampler2D. Updated diagnostics to match the intended pipeline.
*/

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
	float det = 0.f;
	qboolean basis_degenerate = true;
	int i;

	for (i = 0; i < 16; ++i)
	{
		if (!isfinite (m[i]))
		{
			if (out_det)
				*out_det = 0.f;
			return true;
		}
	}

	det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
	if (out_det)
		*out_det = det;

	for (i = 0; i < 3; ++i)
	{
		const float x = m[i * 4 + 0];
		const float y = m[i * 4 + 1];
		const float z = m[i * 4 + 2];
		if (x * x + y * y + z * z > 1e-12f)
		{
			basis_degenerate = false;
			break;
		}
	}

	if (basis_degenerate)
		return true;

	if (fabsf (m[15] - 1.f) > 1e-3f)
		R_Shadow_LogWrite ("INFO shadow matrix m[15]=%.6f (expected near 1 for affine proj path)\n", m[15]);

	return false;
}

static void R_Shadow_LogMatrixDump (const char *tag, const float m[16])
{
	if (!shdlog.active || !m)
		return;
	R_Shadow_LogWrite ("%s matrix=[%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g]\n",
		tag,
		m[0], m[1], m[2], m[3],
		m[4], m[5], m[6], m[7],
		m[8], m[9], m[10], m[11],
		m[12], m[13], m[14], m[15]);
}

void R_Shadow_BindProgram (const char *tag, GLuint target_program)
{
	GLint current_program = 0;

	glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
	if ((GLuint)current_program == target_program)
		return;

	if (r_shadow_log.value > 0.f || r_shadow_validate.value > 0.f)
	{
		R_Shadow_Log_BeginFrame ();
		if (shdlog.active)
		{
			R_Shadow_LogWrite ("DEBUG %s receiver program rebind: target=%u current=%d\n",
				tag ? tag : "SHADOW", (unsigned)target_program, (int)current_program);
		}
	}

	GL_UseProgram (target_program);
}

qboolean R_Shadow_BindUBO (const char *tag, GLuint program, const char *block_name, GLuint binding_point)
{
	shadow_program_ubo_info_t *info = NULL;
	const char *name = (block_name && block_name[0]) ? block_name : FRAME_DATA_UBO_NAME;
	int i;

	if (!GL_GetUniformBlockIndexFunc || !GL_UniformBlockBindingFunc || !GL_GetActiveUniformBlockivFunc)
		return false;

	for (i = 0; i < shadow_program_ubo_info_count; ++i)
	{
		if (shadow_program_ubo_info[i].program == program)
		{
			info = &shadow_program_ubo_info[i];
			break;
		}
	}

	if (!info)
	{
		GLuint block_index;
		GLint current_binding = -1;
		GLint data_size = (GLint)sizeof (r_framedata);

		if (shadow_program_ubo_info_count >= (int)countof (shadow_program_ubo_info))
			return false;

		info = &shadow_program_ubo_info[shadow_program_ubo_info_count++];
		memset (info, 0, sizeof (*info));
		info->program = program;
		info->block_index = GL_INVALID_INDEX;

		block_index = GL_GetUniformBlockIndexFunc (program, name);
		if (block_index != GL_INVALID_INDEX)
		{
			info->has_block = true;
			info->block_index = block_index;
			GL_GetActiveUniformBlockivFunc (program, block_index, GL_UNIFORM_BLOCK_DATA_SIZE, &data_size);
			GL_GetActiveUniformBlockivFunc (program, block_index, GL_UNIFORM_BLOCK_BINDING, &current_binding);
			info->data_size = data_size;
			if (current_binding != (GLint)binding_point)
				GL_UniformBlockBindingFunc (program, block_index, binding_point);
		}
	}

	if (!info->has_block)
	{
		if (!info->warned_missing && (r_shadow_validate.value > 0.f || r_shadow_log.value > 0.f))
		{
			R_Shadow_Log_BeginFrame ();
			if (shdlog.active)
				R_Shadow_LogWrite ("WARN %s missing UBO block '%s' in program=%u\n",
					tag ? tag : "SHADOW", name, (unsigned)program);
			info->warned_missing = true;
		}
		return false;
	}

	if (info->data_size != (GLint)sizeof (r_framedata) && !info->warned_size)
	{
		R_Shadow_Log_BeginFrame ();
		if (shdlog.active)
			R_Shadow_LogWrite ("WARN %s UBO block '%s' size mismatch: got=%d expected=%d program=%u\n",
				tag ? tag : "SHADOW", name, (int)info->data_size, (int)sizeof (r_framedata), (unsigned)program);
		info->warned_size = true;
	}

	if (r_shadow_log.value > 0.f && !info->logged_bind)
	{
		GLint final_binding = -1;
		GL_GetActiveUniformBlockivFunc (program, info->block_index, GL_UNIFORM_BLOCK_BINDING, &final_binding);
		R_Shadow_Log_BeginFrame ();
		if (shdlog.active)
			R_Shadow_LogWrite ("UBO_BIND tag=%s program=%u block=%s index=%u binding=%d size=%d\n",
				tag ? tag : "SHADOW", (unsigned)program, name, (unsigned)info->block_index, (int)final_binding, (int)info->data_size);
		info->logged_bind = true;
	}

	return true;
}

void R_Shadow_LogReceiverUniformUpload (const char *tag, GLuint target_program)
{
	GLint gl_current = 0;
	GLint loc_shadow_viewproj = 0;
	GLint loc_shadow_params = 0;
	GLint loc_shadow_debug = 0;
	GLuint frame_ubo_index = GL_INVALID_INDEX;
	GLint frame_ubo_binding = 0;
	GLint frame_ubo_data_size = 0;
	qboolean frame_ubo_metadata_valid = false;
	const char *target_name = GL_GetProgramDebugName (target_program);
	const char *target_defines = GL_GetProgramDebugDefines (target_program);
	const char *target_vert = GL_GetProgramVertexShaderPath (target_program);
	const char *target_frag = GL_GetProgramFragmentShaderPath (target_program);
	const char *current_name = GL_GetProgramDebugName (GL_GetCurrentProgramCached ());

	glGetIntegerv (GL_CURRENT_PROGRAM, &gl_current);

	loc_shadow_viewproj = GL_GetUniformLocationFunc (target_program, "ShadowViewProj");
	loc_shadow_params = GL_GetUniformLocationFunc (target_program, "ShadowParams");
	loc_shadow_debug = GL_GetUniformLocationFunc (target_program, "ShadowDebug");
	frame_ubo_index = GL_GetUniformBlockIndexFunc (target_program, FRAME_DATA_UBO_NAME);
	if (frame_ubo_index != GL_INVALID_INDEX && GL_GetActiveUniformBlockivFunc)
	{
		GLint block_count = 0;
		GL_GetProgramivFunc (target_program, GL_ACTIVE_UNIFORM_BLOCKS, &block_count);
		if ((GLint)frame_ubo_index < block_count)
		{
			GL_GetActiveUniformBlockivFunc (target_program, frame_ubo_index, GL_UNIFORM_BLOCK_BINDING, &frame_ubo_binding);
			GL_GetActiveUniformBlockivFunc (target_program, frame_ubo_index, GL_UNIFORM_BLOCK_DATA_SIZE, &frame_ubo_data_size);
			frame_ubo_metadata_valid = true;
		}
	}

	R_Shadow_Log_BeginFrame ();
	if (shdlog.active)
	{
		R_Shadow_LogWrite (
			"UPLOAD shadow uniforms: tag=%s target=%u(%s) vert=%s frag=%s defines=%s gl_current=%d(%s) loc_matrix=%d loc_params=%d loc_debug=%d\n",
			tag ? tag : "SHADOW",
			(unsigned)target_program,
			target_name,
			target_vert,
			target_frag,
			(target_defines && target_defines[0]) ? target_defines : "<none>",
			(int)gl_current,
			current_name,
			(int)loc_shadow_viewproj,
			(int)loc_shadow_params,
			(int)loc_shadow_debug);
		if (frame_ubo_index != GL_INVALID_INDEX)
		{
			R_Shadow_LogWrite ("UPLOAD shadow uniforms via UBO: tag=%s block=" FRAME_DATA_UBO_NAME " index=%u binding=%d data_size=%d metadata=%s\n",
				tag ? tag : "SHADOW", (unsigned)frame_ubo_index, (int)frame_ubo_binding, (int)frame_ubo_data_size,
				frame_ubo_metadata_valid ? "ok" : "invalid");
			if (!frame_ubo_metadata_valid)
				R_Shadow_LogWrite ("WARN %s FrameDataUBO metadata invalid (binding=%d data_size=%d)\n",
					tag ? tag : "SHADOW", (int)frame_ubo_binding, (int)frame_ubo_data_size);
		}
		else if (loc_shadow_viewproj == -1 && loc_shadow_params == -1 && loc_shadow_debug == -1)
		{
			R_Shadow_LogWrite ("WARN %s has no shadow uniforms and no FrameDataUBO block (likely shadows not compiled in this receiver variant)\n",
				tag ? tag : "SHADOW");
		}
		if (((tag && !q_strcasecmp (tag, "WORLD")) || (tag && !q_strcasecmp (tag, "ALIAS"))) && loc_shadow_viewproj == -1 && frame_ubo_index == GL_INVALID_INDEX)
		{
			R_Shadow_DumpActiveUniforms (target_program);
			R_Shadow_DumpActiveUniformBlocks (target_program);
		}
		if (r_shadow_validate.value > 0.f && loc_shadow_viewproj == -1 && loc_shadow_params == -1 && loc_shadow_debug == -1 && frame_ubo_index == GL_INVALID_INDEX)
		{
			R_Shadow_LogWrite ("WARN %s receiver missing both fallback uniforms and FrameDataUBO\n", tag ? tag : "SHADOW");
		}
		if (target_program == 67 || target_program == 207 || target_program == 210)
		{
			R_Shadow_LogWrite ("PROGRAM FOCUS id=%u name=%s vert=%s frag=%s defines=%s\n",
				(unsigned)target_program,
				target_name,
				target_vert,
				target_frag,
				(target_defines && target_defines[0]) ? target_defines : "<none>");
		}
	}

	if ((GLuint)gl_current != target_program)
	{
#if defined(_DEBUG)
		if (r_gl_verify_program.value > 0.f)
			Sys_Error ("R_Shadow_LogReceiverUniformUpload: gl_current=%d target=%u (%s)", (int)gl_current, (unsigned)target_program, target_name);
#endif
	}
}

static void R_Shadow_LogTextureParams (const char *tag, GLuint tex)
{
	GLint width, height, internal, maxlevel;
	GLint minf, magf, wraps, wrapt, cmode, cfunc;
	GLfloat border[4];
	if (!tex)
	{
		R_Shadow_LogWrite ("%s tex=0 (unbound)\n", tag);
		return;
	}
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, tex);
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
	R_Shadow_LogWrite ("%s tex=%u size=%dx%d ifmt=0x%X min=0x%X mag=0x%X wrap=(0x%X,0x%X) compare=(0x%X,0x%X) maxlevel=%d border=(%.2f %.2f %.2f %.2f)\n",
		tag, tex, width, height, (unsigned)internal, (unsigned)minf, (unsigned)magf, (unsigned)wraps, (unsigned)wrapt, (unsigned)cmode, (unsigned)cfunc, maxlevel,
		border[0], border[1], border[2], border[3]);
	if (width <= 0 || height <= 0)
		R_Shadow_LogWrite ("WARN shadow texture has zero size\n");
	if (internal != GL_DEPTH_COMPONENT24 && internal != GL_DEPTH_COMPONENT32F && internal != GL_DEPTH_COMPONENT16)
		R_Shadow_LogWrite ("WARN unexpected depth internal format 0x%X\n", (unsigned)internal);
	if (cmode != GL_NONE)
		R_Shadow_LogWrite ("WARN compare mode != GL_NONE but current shaders use sampler2D/manual compare\n");
	shdlog.last_shadow_compare_mode = (GLenum)cmode;
}

static void R_Shadow_ConfigureDepthTexture (GLuint tex, qboolean hw_compare)
{
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	{
		const float border[4] = { 1.f, 1.f, 1.f, 1.f };
		glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, hw_compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

static void R_Shadow_ValidateDepthResources (const char *tag, GLuint fbo, GLuint tex, int expected_w, int expected_h, GLenum expected_compare_mode)
{
	GLint width = 0, height = 0, cmode = GL_NONE, minf = 0, magf = 0;
	GLenum status;

	if (r_shadow_validate.value <= 0.f)
		return;

	if (!fbo || !tex)
	{
		Con_DWarning ("%s: missing shadow resource(s): fbo=%u tex=%u\n", tag, (unsigned)fbo, (unsigned)tex);
		return;
	}

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, tex);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &cmode);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);

	if (width != expected_w || height != expected_h)
		Con_DWarning ("%s: shadow texture size mismatch (%dx%d, expected %dx%d)\n", tag, width, height, expected_w, expected_h);
	if ((GLenum)cmode != expected_compare_mode)
		Con_DWarning ("%s: shadow compare mode mismatch (0x%X, expected 0x%X)\n", tag, (unsigned)cmode, (unsigned)expected_compare_mode);
	if (minf != GL_NEAREST || magf != GL_NEAREST)
		Con_DWarning ("%s: unexpected filter state min=0x%X mag=0x%X (expected NEAREST)\n", tag, (unsigned)minf, (unsigned)magf);

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, fbo);
	status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		Con_DWarning ("%s: shadow FBO incomplete 0x%X (%s)\n", tag, (unsigned)status, R_Shadow_LogFBOStatusString (status));
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

static void R_Shadow_LogMainViewCullContext (const char *tag)
{
	int i;

	R_Shadow_Log_BeginFrame ();
	if (!shdlog.active)
		return;

	R_Shadow_LogWrite ("%s world_bounds mins=(%.3f %.3f %.3f) maxs=(%.3f %.3f %.3f) vieworg=(%.3f %.3f %.3f) clipctl=%d\n",
		tag,
		cl.worldmodel ? cl.worldmodel->mins[0] : 0.f, cl.worldmodel ? cl.worldmodel->mins[1] : 0.f, cl.worldmodel ? cl.worldmodel->mins[2] : 0.f,
		cl.worldmodel ? cl.worldmodel->maxs[0] : 0.f, cl.worldmodel ? cl.worldmodel->maxs[1] : 0.f, cl.worldmodel ? cl.worldmodel->maxs[2] : 0.f,
		r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2], gl_clipcontrol_able ? 1 : 0);

	for (i = 0; i < 4; ++i)
	{
		R_Shadow_LogWrite ("%s frustum[%d] n=(%.6f %.6f %.6f) d=%.6f\n",
			tag, i,
			frustum[i].normal[0], frustum[i].normal[1], frustum[i].normal[2], frustum[i].dist);
	}
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
	if (shdlog.active)
	{
		R_Shadow_LogWrite ("----- frame=%d map=%s vieworg=(%.1f %.1f %.1f) viewangles=(%.1f %.1f %.1f) -----\n",
			r_framecount,
			cl.mapname[0] ? cl.mapname : "(nomap)",
			r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2],
			r_refdef.viewangles[0], r_refdef.viewangles[1], r_refdef.viewangles[2]);
	}
}

static void R_Shadow_LogEndFrameIfNeeded (void)
{
	if (shdlog.dump)
		Cvar_SetValueQuick (&r_shadow_log_dump, 0.f);
}

void R_Shadow_Log_SunPassEarlyOut (const char *reason)
{
	R_Shadow_Log_BeginFrame ();
	if (!shdlog.active)
		return;
	R_Shadow_LogWrite ("SUNPASS skip: %s\n", reason);
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
	R_Shadow_LogTextureParams (tag, depth_tex);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		R_Shadow_LogWrite ("WARN shadow FBO incomplete for %s\n", tag);
	R_Shadow_LogGLStage (tag);
}

void R_Shadow_Log_ReceiverPassSnapshot (const char *tag, int program, GLenum texunit, GLuint expected_tex, qboolean shadows_enabled, float bias, float normalbias, float pcf, float taps, const float *shadow_viewproj)
{
	GLint active_tex, bound_tex, draw_fbo, read_fbo, current_program;
	GLint vp[4], sc[4];
	float det = 0.f;
	qboolean bad;
	qboolean finite = false;
	R_Shadow_Log_BeginFrame ();
	if (!shdlog.active)
		return;
	glGetIntegerv (GL_ACTIVE_TEXTURE, &active_tex);
	GL_ActiveTextureFunc (texunit);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &bound_tex);
	GL_ActiveTextureFunc (active_tex);
	glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, vp);
	glGetIntegerv (GL_SCISSOR_BOX, sc);
	bad = shadow_viewproj ? R_Shadow_LogMatrixHasBadValues (shadow_viewproj, &det) : true;
	finite = !bad;
	if (!shdlog.dump && shdlog.last_program == program && shdlog.last_shadow_sampler_unit == (GLint)(texunit - GL_TEXTURE0) && shdlog.last_shadow_tex == expected_tex && (GLuint)bound_tex == expected_tex)
	{
		R_Shadow_LogEndFrameIfNeeded ();
		return;
	}
	shdlog.last_program = program;
	shdlog.last_shadow_sampler_unit = (GLint)(texunit - GL_TEXTURE0);
	shdlog.last_shadow_tex = expected_tex;
	R_Shadow_LogWrite ("%s receiver program=%d current_program=%d enable=%d texunit=%d expected_tex=%u bound_tex=%d draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) scissor=(%d %d %d %d)\n",
		tag, program, current_program, shadows_enabled, (int)(texunit - GL_TEXTURE0), expected_tex, bound_tex, draw_fbo, read_fbo, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3]);
	R_Shadow_LogWrite ("%s receiver uniform-target program=%d gl_current_program=%d\n", tag, program, current_program);
	R_Shadow_LogWrite ("%s params bias=%.9g normalbias=%.9g pcf=%.3g taps=%.3g matrix_row0=(%.9g %.9g %.9g %.9g) det3x3=%.9g finite=%d\n",
		tag, bias, normalbias, pcf, taps,
		shadow_viewproj ? shadow_viewproj[0] : 0.f,
		shadow_viewproj ? shadow_viewproj[1] : 0.f,
		shadow_viewproj ? shadow_viewproj[2] : 0.f,
		shadow_viewproj ? shadow_viewproj[3] : 0.f,
		det, finite ? 1 : 0);
	if (!shadows_enabled)
		R_Shadow_LogWrite ("WARN receiver shadow branch disabled\n");
	if ((GLuint)bound_tex != expected_tex)
		R_Shadow_LogWrite ("WARN receiver shadow texture mismatch: expected=%u bound=%d\n", expected_tex, bound_tex);
	if (bad)
	{
		R_Shadow_LogWrite ("WARN shadow matrix invalid (NaN/Inf or degenerate basis)\n");
		R_Shadow_LogMatrixDump (tag, shadow_viewproj);
	}
	if (bias < 1e-7f || bias > 0.1f)
		R_Shadow_LogWrite ("WARN suspicious shadow bias %.6f\n", bias);
	if (pcf > 0.f && shdlog.last_shadow_compare_mode != GL_NONE)
		R_Shadow_LogWrite ("WARN manual PCF enabled but depth compare mode is not GL_NONE\n");
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
	shadow_dlight_atlas_size = 0;
	shadow_dlight_tile_size = 0;
	shadow_dlight_tile_count = 0;
	shadow_dlight_validated_once = false;
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

static void R_Shadow_DestroyResources (void)
{
	if (shadow_fbo)
	{
		GL_DeleteFramebuffersFunc (1, &shadow_fbo);
		shadow_fbo = 0;
	}
	if (shadow_depth_tex)
	{
		GL_DeleteNativeTexture (shadow_depth_tex);
		shadow_depth_tex = 0;
	}
	shadowmap_size = 0;
	shadow_sun_validated_once = false;
	R_Shadow_DestroyDlightResources ();
}

static void R_Shadow_GetSunDirection (vec3_t out_dir)
{
	qboolean sanitized = false;

	if (r_sun.enabled)
	{
		VectorCopy (r_sun.direction, out_dir);
		if (!isfinite (out_dir[0]) || !isfinite (out_dir[1]) || !isfinite (out_dir[2]))
		{
			VectorSet (out_dir, 0.f, 0.f, -1.f);
			sanitized = true;
		}
		if (VectorNormalize (out_dir) == 0.f)
		{
			VectorSet (out_dir, 0.f, 0.f, -1.f);
			sanitized = true;
		}
	}
	else
	{
		VectorSet (out_dir, 0.f, 0.f, -1.f);
		sanitized = true;
	}

	if (sanitized && !shadow_warned_sun_dir_sanitize)
	{
		Con_DWarning ("R_Shadow_GetSunDirection: sanitized invalid sun direction, using fallback (0 0 -1)\n");
		shadow_warned_sun_dir_sanitize = true;
	}
}

static void R_Shadow_BuildViewProj (float out_viewproj[16], vec4_t out_sun_dir)
{
	vec3_t sun_dir;
	vec3_t up = { 0.f, 0.f, 1.f };
	vec3_t right;
	vec3_t light_up;
	vec3_t corner;
	float znear;
	float zfar;
	float tanx;
	float tany;
	float wnear;
	float hnear;
	float wfar;
	float hfar;
	float min_ls[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float max_ls[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	float center_ls[3];
	float extents[3];
	vec3_t view_forward;
	vec3_t view_right;
	vec3_t view_up;
	vec3_t origin_world;
	float view[16];
	float ortho[16];
	int i;

	R_Shadow_GetSunDirection (sun_dir);
	VectorCopy (sun_dir, out_sun_dir);
	out_sun_dir[3] = r_sun.enabled ? r_sun.intensity : 0.f;

	if (fabsf (DotProduct (sun_dir, up)) > 0.95f)
	{
		up[0] = 0.f;
		up[1] = 1.f;
		up[2] = 0.f;
	}

	CrossProduct (up, sun_dir, right);
	VectorNormalize (right);
	CrossProduct (sun_dir, right, light_up);
	VectorNormalize (light_up);

	AngleVectors (r_refdef.viewangles, view_forward, view_right, view_up);

	tanx = tanf (DEG2RAD (r_fovx) * 0.5f);
	tany = tanf (DEG2RAD (r_fovy) * 0.5f);

	{
		float w = 1.f / tanx;
		float h = 1.f / tany;
		float d = 12.f * q_min (w, h);
		znear = CLAMP (0.5f, d, 4.f);
	}

	zfar = gl_farclip.value;

	wnear = tanx * znear;
	hnear = tany * znear;
	wfar = tanx * zfar;
	hfar = tany * zfar;

	for (i = 0; i < 8; ++i)
	{
		float sx = (i & 1) ? 1.f : -1.f;
		float sy = (i & 2) ? 1.f : -1.f;
		float sz = (i & 4) ? zfar : znear;
		float w = (i & 4) ? wfar : wnear;
		float h = (i & 4) ? hfar : hnear;

		VectorMA (r_refdef.vieworg, sz, view_forward, corner);
		VectorMA (corner, sx * w, view_right, corner);
		VectorMA (corner, sy * h, view_up, corner);

		{
			float lsx = DotProduct (corner, right);
			float lsy = DotProduct (corner, light_up);
			float lsz = DotProduct (corner, sun_dir);

			min_ls[0] = q_min (min_ls[0], lsx);
			min_ls[1] = q_min (min_ls[1], lsy);
			min_ls[2] = q_min (min_ls[2], lsz);
			max_ls[0] = q_max (max_ls[0], lsx);
			max_ls[1] = q_max (max_ls[1], lsy);
			max_ls[2] = q_max (max_ls[2], lsz);
		}
	}

	for (i = 0; i < 3; ++i)
	{
		center_ls[i] = 0.5f * (min_ls[i] + max_ls[i]);
		extents[i] = 0.5f * (max_ls[i] - min_ls[i]);
	}

	for (i = 0; i < 3; ++i)
	{
		if (!isfinite (extents[i]) || extents[i] < 1.f)
		{
			extents[i] = 1.f;
			if (!shadow_warned_matrix_sanitize)
				Con_DWarning ("R_Shadow_BuildViewProj: sanitized degenerate shadow extent axis=%d\n", i);
			shadow_warned_matrix_sanitize = true;
		}
	}

	if (shadowmap_size > 0)
	{
		float texel_x = (extents[0] * 2.f) / (float)shadowmap_size;
		float texel_y = (extents[1] * 2.f) / (float)shadowmap_size;
		float prev_x = center_ls[0];
		float prev_y = center_ls[1];
		if (texel_x > 0.f)
			center_ls[0] = floorf (center_ls[0] / texel_x) * texel_x;
		if (texel_y > 0.f)
			center_ls[1] = floorf (center_ls[1] / texel_y) * texel_y;
		if (r_shadow_csm_debug.value > 0.f)
		{
			Con_Printf ("CSM debug: split=[%.1f %.1f] texel=(%.5f %.5f) center=(%.3f %.3f)->(%.3f %.3f)\n",
				znear, zfar, texel_x, texel_y, prev_x, prev_y, center_ls[0], center_ls[1]);
		}
	}

	VectorScale (right, center_ls[0], origin_world);
	VectorMA (origin_world, center_ls[1], light_up, origin_world);
	VectorMA (origin_world, center_ls[2], sun_dir, origin_world);

	memset (view, 0, sizeof (view));
	view[0] = right[0];
	view[1] = right[1];
	view[2] = right[2];
	view[4] = light_up[0];
	view[5] = light_up[1];
	view[6] = light_up[2];
	view[8] = sun_dir[0];
	view[9] = sun_dir[1];
	view[10] = sun_dir[2];
	view[15] = 1.f;
	view[12] = -DotProduct (right, origin_world);
	view[13] = -DotProduct (light_up, origin_world);
	view[14] = -DotProduct (sun_dir, origin_world);

	{
		float min_z = -extents[2];
		float max_z = extents[2];
		R_Shadow_OrthoMatrix (ortho, -extents[0], extents[0], -extents[1], extents[1], min_z, max_z);
	}

	R_Shadow_Log_BeginFrame ();
	if (shdlog.active)
	{
		R_Shadow_LogWrite ("SUNPASS inputs sun_dir=(%.6f %.6f %.6f) intensity=%.6f near=%.6f far=%.6f extents=(%.6f %.6f %.6f) center_ls=(%.6f %.6f %.6f) clipctl=%d\n",
			sun_dir[0], sun_dir[1], sun_dir[2], out_sun_dir[3], znear, zfar, extents[0], extents[1], extents[2], center_ls[0], center_ls[1], center_ls[2], gl_clipcontrol_able ? 1 : 0);
	}

	memcpy (out_viewproj, ortho, sizeof (ortho));
	MatrixMultiply (out_viewproj, view);
	if (shdlog.active)
	{
		R_Shadow_LogWrite ("SUNPASS proj_sign ortho_m10=%.6f ortho_m14=%.6f clipctl=%d\n", ortho[10], ortho[14], gl_clipcontrol_able ? 1 : 0);
		R_Shadow_LogMatrixDump ("SUNPASS out_viewproj", out_viewproj);
	}
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

	memset (view, 0, sizeof (view));
	view[0] = right[0];
	view[1] = right[1];
	view[2] = right[2];
	view[4] = light_up[0];
	view[5] = light_up[1];
	view[6] = light_up[2];
	view[8] = forward[0];
	view[9] = forward[1];
	view[10] = forward[2];
	view[15] = 1.f;
	view[12] = -DotProduct (right, origin);
	view[13] = -DotProduct (light_up, origin);
	view[14] = -DotProduct (forward, origin);

	znear = 4.f;
	zfar = q_max (radius, znear + 1.f);
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
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_dlight_depth_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, shadow_dlight_depth_tex, -1, "shadowmap dlight depth");
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, atlas_size, atlas_size);
	R_Shadow_ConfigureDepthTexture (shadow_dlight_depth_tex, false);

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
	shadow_fbo = 0;
	shadow_depth_tex = 0;
	shadowmap_size = 0;
	shadow_dlight_fbo = 0;
	shadow_dlight_depth_tex = 0;
	shadow_dlight_atlas_size = 0;
	shadow_dlight_tile_size = 0;
	shadow_dlight_tile_count = 0;
	shadow_dlight_selected_count = 0;
	shadow_sun_validated_once = false;
	shadow_dlight_validated_once = false;
}

void R_ShutdownShadow (void)
{
	R_Shadow_DestroyResources ();
}

void R_ResizeShadowMapIfNeeded (void)
{
	int desired;

	if (r_shadowmap_size.value <= 0.f)
		desired = 0;
	else
		desired = (int)r_shadowmap_size.value;

	if (desired > gl_max_texture_size)
		desired = gl_max_texture_size;

	if (desired < 256)
		desired = 256;

	if (desired == shadowmap_size && shadow_depth_tex)
		return;

	R_Shadow_DestroyResources ();

	if (desired <= 0)
		return;

	glGenTextures (1, &shadow_depth_tex);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_depth_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, shadow_depth_tex, -1, "shadowmap depth");
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, desired, desired);
	R_Shadow_ConfigureDepthTexture (shadow_depth_tex, false);

	GL_GenFramebuffersFunc (1, &shadow_fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_fbo);
	GL_ObjectLabelFunc (GL_FRAMEBUFFER, shadow_fbo, -1, "shadowmap fbo");
	GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_depth_tex, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);

	{
		GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			Sys_Error ("Failed to create shadowmap FBO (status code 0x%X)", status);
	}

	shadowmap_size = desired;
}

void R_Shadow_BindShadowMap (GLenum texunit)
{
	GL_BindNative (texunit, GL_TEXTURE_2D, shadow_depth_tex);
}

void R_Shadow_BindDlightShadowMap (GLenum texunit)
{
	if (shadow_dlight_depth_tex)
		GL_BindNative (texunit, GL_TEXTURE_2D, shadow_dlight_depth_tex);
	else
		GL_BindNative (texunit, GL_TEXTURE_2D, 0);
}

GLuint R_Shadow_GetShadowMapTextureId (void)
{
	return shadow_depth_tex;
}

GLuint R_Shadow_GetDlightShadowMapTextureId (void)
{
	return shadow_dlight_depth_tex;
}

void R_Shadow_SunPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadow_sun.value > 0.f;
	vec4_t sun_dir;
	double t0, t1;
	int draws0, tris0;
	int shadow_drawcalls = 0;
	int brush_count = 0;
	int alias_count = 0;
	qboolean drew_world_caster = false;
	shadow_gl_state_t saved_state;

	R_Shadow_Log_BeginFrame ();
	r_framedata.shadow_debug[0] = 0.f;
	IdentityMatrix (r_framedata.shadow_viewproj);
	VectorSet (r_framedata.shadow_sun_dir, 0.f, 0.f, -1.f);
	r_framedata.shadow_sun_dir[3] = 0.f;
	if (!enabled || !r_sun.enabled || r_sun.intensity <= 0.f)
	{
		R_Shadow_Log_SunPassEarlyOut ("disabled by r_shadows/r_shadow_sun");
		return;
	}
	if (!glprogs.shadow_depth)
	{
		R_Shadow_Log_SunPassEarlyOut ("missing glprogs.shadow_depth");
		return;
	}

	R_ResizeShadowMapIfNeeded ();
	if (!shadow_depth_tex || !shadow_fbo)
	{
		R_Shadow_Log_SunPassEarlyOut ("shadow map resources unavailable");
		return;
	}

	R_Shadow_BuildViewProj (r_framedata.shadow_viewproj, sun_dir);
	r_framedata.shadow_debug[0] = 1.f;
	R_Shadow_ResetBrushAuditCounters ();
	R_Shadow_ResetAliasAuditCounters ();
	VectorCopy (sun_dir, r_framedata.shadow_sun_dir);
	r_framedata.shadow_sun_dir[3] = sun_dir[3];
	R_UploadFrameData ();
	R_Shadow_SaveGLState (&saved_state);

	GL_BeginGroup ("Shadow map (sun)");
	t0 = Sys_DoubleTime ();
	draws0 = rs_brushpasses + rs_aliaspasses;
	tris0 = rs_brushpasses + rs_aliaspasses;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_fbo);
	glViewport (0, 0, shadowmap_size, shadowmap_size);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	GL_DepthRange (ZRANGE_FULL);
	R_Shadow_LogMainViewCullContext ("SUNPASS");
	R_Shadow_LogWrite ("SUNPASS depthcfg clearDepth_expected=%.1f depthFunc_expected=0x%X depth_range=FULL clipctl=%d\n",
		gl_clipcontrol_able ? 0.f : 1.f, gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL, gl_clipcontrol_able ? 1 : 0);
	if (!shadow_sun_validated_once || r_shadow_validate.value > 1.f)
	{
		R_Shadow_ValidateDepthResources ("sun", shadow_fbo, shadow_depth_tex, shadowmap_size, shadowmap_size, GL_NONE);
		shadow_sun_validated_once = true;
	}

	GL_UseProgram (glprogs.shadow_depth);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (6));
	R_Shadow_LogClearDebug ("R_Shadow_Sun", GL_DEPTH_BUFFER_BIT);
	glClear (GL_DEPTH_BUFFER_BIT);

	{
		entity_t **ents = R_GetVisEntities (mod_brush, false, &brush_count);
		R_Shadow_LogWrite ("SUNPASS vis brush_entities=%d\n", brush_count);
		R_DrawBrushModels_Shadow (ents, brush_count);
	}
	{
		entity_t **ents = R_GetVisEntities (mod_alias, false, &alias_count);
		R_Shadow_LogWrite ("SUNPASS vis alias_entities=%d\n", alias_count);
		R_DrawAliasModels_Shadow (ents, alias_count);
	}
	drew_world_caster = R_DrawWorld_Shadow ();
	R_Shadow_LogWrite ("SUNPASS world_caster=%s\n", drew_world_caster ? "drawn" : "missing");
	t1 = Sys_DoubleTime ();
	{
		int brush_in = 0, brush_inst = 0, surf_considered = 0, surf_submitted = 0;
		int alias_in = 0, alias_submitted = 0;
		int legacy_draws = (rs_brushpasses + rs_aliaspasses) - draws0;
		int legacy_tris = (rs_brushpasses + rs_aliaspasses) - tris0;
		R_Shadow_GetBrushAuditCounters (&brush_in, &brush_inst, &surf_considered, &surf_submitted);
		R_Shadow_GetAliasAuditCounters (&alias_in, &alias_submitted);
		shadow_drawcalls = surf_submitted + alias_submitted;
		R_Shadow_LogWrite ("SUNPASS audit brush_entities_in=%d brush_entities_after_cull=%d brush_surfaces=%d brush_surfaces_after_filters=%d alias_entities_in=%d alias_entities_after_cull=%d drawcalls_est=%d legacy_draws=%d legacy_tris=%d\n",
			brush_in, brush_inst, surf_considered, surf_submitted, alias_in, alias_submitted, shadow_drawcalls, legacy_draws, legacy_tris);
		if (shadow_drawcalls <= 0)
		{
			if (q_strcasecmp (shadow_last_empty_sunpass_map, cl.mapname))
			{
				q_strlcpy (shadow_last_empty_sunpass_map, cl.mapname, sizeof (shadow_last_empty_sunpass_map));
				Con_Warning ("SUNPASS produced no casters on map '%s' (brush=%d alias=%d world=%s). World caster missing?\n",
					cl.mapname[0] ? cl.mapname : "<unknown>", brush_count, alias_count, drew_world_caster ? "yes" : "no");
			}
		}
		else
		{
			shadow_last_empty_sunpass_map[0] = '\0';
		}
	}
	R_Shadow_Log_ShadowPassSnapshot ("SUNPASS", shadow_fbo, shadow_depth_tex, shadowmap_size, shadowmap_size,
		shadow_drawcalls, shadow_drawcalls, (t1 - t0) * 1000.0);
	R_Shadow_RestoreGLState (&saved_state);

	GL_EndGroup ();
}

void R_Shadow_DlightPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadow_dlights.value > 0.f && r_dlight_shadows.value > 0.f;
	double t0, t1;
	int draws0, tris0;
	shadow_gl_state_t saved_state;
	R_Shadow_Log_BeginFrame ();
	float sun_viewproj[16];
	int max_tiles;
	int grid;
	int tiles_used = 0;

	shadow_dlight_selected_count = 0;

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

	if (!enabled || !r_sun.enabled || r_sun.intensity <= 0.f)
	{
		R_Shadow_Log_SunPassEarlyOut ("DLIGHTPASS disabled by cvars");
		return;
	}
	if (!glprogs.shadow_depth)
	{
		R_Shadow_Log_SunPassEarlyOut ("DLIGHTPASS missing glprogs.shadow_depth");
		return;
	}

	R_Shadow_ResizeDlightAtlasIfNeeded ();
	if (!shadow_dlight_depth_tex || !shadow_dlight_fbo)
	{
		R_Shadow_Log_SunPassEarlyOut ("DLIGHTPASS resources unavailable");
		return;
	}

	max_tiles = CLAMP (0, (int)r_shadow_dlight_max.value, SHADOW_DLIGHT_MAX);
	max_tiles = q_min (max_tiles, CLAMP (0, (int)r_dlight_max.value, SHADOW_DLIGHT_MAX));
	if (shadow_dlight_tile_count > 0)
		max_tiles = q_min (max_tiles, shadow_dlight_tile_count);
	if (max_tiles <= 0)
	{
		R_Shadow_Log_SunPassEarlyOut ("DLIGHTPASS max_tiles <= 0");
		return;
	}

	if (r_framedata.numlights == 0)
	{
		R_Shadow_Log_SunPassEarlyOut ("DLIGHTPASS no gpu lights");
		return;
	}

	{
		float scores[SHADOW_DLIGHT_MAX];
		for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
			scores[i] = -FLT_MAX;

		for (unsigned int i = 0; i < r_framedata.numlights; ++i)
		{
			dlight_t *dl = r_dlight_sources[i];
			const gpulight_t *glight = &r_lightbuffer.lights[i];
			float dist;
			float score;
			vec3_t delta;

			if (!dl)
				continue;

			VectorSubtract (glight->pos, r_refdef.vieworg, delta);
			dist = VectorLength (delta);
			if (r_shadow_dlight_distance.value > 0.f && dist > r_shadow_dlight_distance.value + glight->radius)
				continue;

			score = (glight->radius * (glight->color[0] + glight->color[1] + glight->color[2])) / (1.f + dist);
			if (dl->kind == DL_PERSISTENT)
				score *= 1.15f;

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
	}

	for (int i = 0; i < max_tiles; ++i)
	{
		if (shadow_dlight_light_indices[i] >= 0)
			tiles_used++;
	}
	shadow_dlight_selected_count = tiles_used;

	if (!tiles_used)
	{
		R_Shadow_Log_SunPassEarlyOut ("DLIGHTPASS no selected lights");
		return;
	}

	memcpy (sun_viewproj, r_framedata.shadow_viewproj, sizeof (sun_viewproj));
	R_Shadow_SaveGLState (&saved_state);

	GL_BeginGroup ("Shadow map (dlights)");
	t0 = Sys_DoubleTime ();
	draws0 = rs_brushpasses + rs_aliaspasses;
	tris0 = rs_brushpasses + rs_aliaspasses;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_dlight_fbo);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	GL_DepthRange (ZRANGE_FULL);
	if (!shadow_dlight_validated_once || r_shadow_validate.value > 1.f)
	{
		R_Shadow_ValidateDepthResources ("dlight", shadow_dlight_fbo, shadow_dlight_depth_tex, shadow_dlight_atlas_size, shadow_dlight_atlas_size, GL_NONE);
		shadow_dlight_validated_once = true;
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

		const gpulight_t *glight = &r_lightbuffer.lights[light_index];
		dlight_t *dl = r_dlight_sources[light_index];
		float viewproj[16];
		int tile_x = i % grid;
		int tile_y = i / grid;
		float lod_mul = 1.f;
		float dist_weight;
		float radius_weight;
		float coverage;
		float scale;
		float offset_x;
		float offset_y;

		dist_weight = 1.f / (1.f + Distance (glight->pos, r_refdef.vieworg));
		radius_weight = CLAMP (0.f, glight->radius / 384.f, 1.f);
		coverage = CLAMP (0.f, glight->radius * dist_weight * 4.f + radius_weight * 0.5f, 1.f);
		if (dl && dl->kind == DL_PERSISTENT)
			coverage = q_min (1.f, coverage + 0.15f);
		if (coverage < 0.20f)
			lod_mul = 0.5f;
		else if (coverage < 0.45f)
			lod_mul = 0.75f;

		scale = ((float)shadow_dlight_tile_size * lod_mul) / (float)shadow_dlight_atlas_size;
		offset_x = tile_x * ((float)shadow_dlight_tile_size / (float)shadow_dlight_atlas_size);
		offset_y = tile_y * ((float)shadow_dlight_tile_size / (float)shadow_dlight_atlas_size);

		R_Shadow_BuildDlightViewProj (viewproj, glight->pos, glight->radius);
		memcpy (r_framedata.shadow_dlight_viewproj[i], viewproj, sizeof (viewproj));
		r_framedata.shadow_dlight_atlas[i][0] = scale;
		r_framedata.shadow_dlight_atlas[i][1] = scale;
		r_framedata.shadow_dlight_atlas[i][2] = offset_x;
		r_framedata.shadow_dlight_atlas[i][3] = offset_y;
		r_framedata.shadow_dlight_info[i][0] = (float)light_index;

		memcpy (r_framedata.shadow_viewproj, viewproj, sizeof (viewproj));
		R_UploadFrameData ();

		{
			int tile_dim = q_max (64, (int)((float)shadow_dlight_tile_size * lod_mul));
			glViewport (tile_x * shadow_dlight_tile_size, tile_y * shadow_dlight_tile_size,
				tile_dim, tile_dim);
			glScissor (tile_x * shadow_dlight_tile_size, tile_y * shadow_dlight_tile_size,
				tile_dim, tile_dim);
		}
		R_Shadow_LogClearDebug ("R_Shadow_Dlight", GL_DEPTH_BUFFER_BIT);
		glClear (GL_DEPTH_BUFFER_BIT);

		GL_UseProgram (glprogs.shadow_depth);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (6));

		{
			int count = 0;
			entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
			R_Shadow_LogWrite ("DLIGHTPASS tile=%d vis brush_entities=%d\n", i, count);
			R_DrawBrushModels_Shadow (ents, count);
		}
		{
			int count = 0;
			entity_t **ents = R_GetVisEntities (mod_alias, false, &count);
			R_Shadow_LogWrite ("DLIGHTPASS tile=%d vis alias_entities=%d\n", i, count);
			R_DrawAliasModels_Shadow (ents, count);
		}
		R_Shadow_LogWrite ("DLIGHTPASS tile=%d world_caster=%s\n", i, R_DrawWorld_Shadow () ? "drawn" : "missing");
	}

	GL_SetScissorEnabled (false);
	t1 = Sys_DoubleTime ();
	R_Shadow_Log_ShadowPassSnapshot ("DLIGHTPASS", shadow_dlight_fbo, shadow_dlight_depth_tex, shadow_dlight_atlas_size, shadow_dlight_atlas_size,
		(rs_brushpasses + rs_aliaspasses) - draws0, (rs_brushpasses + rs_aliaspasses) - tris0, (t1 - t0) * 1000.0);
	R_Shadow_LogWrite ("DLIGHTPASS selected=%d atlas=%d tile=%d tile_count=%d cvar_max=%d\n", shadow_dlight_selected_count, shadow_dlight_atlas_size, shadow_dlight_tile_size, shadow_dlight_tile_count, max_tiles);

	memcpy (r_framedata.shadow_viewproj, sun_viewproj, sizeof (sun_viewproj));
	R_Shadow_RestoreGLState (&saved_state);

	GL_EndGroup ();
}

void R_Shadow_DrawDebug (void)
{
	int mode = (int)r_shadow_debug.value;
	if (mode != 1 && mode != 4)
		return;
	if (!glprogs.shadow_debug)
		return;
	if (mode == 1 && !shadow_depth_tex)
		return;
	if (mode == 4 && !shadow_dlight_depth_tex)
		return;

	GL_BeginGroup ("Shadow map debug");

	GL_UseProgram (glprogs.shadow_debug);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	if (mode == 1)
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_depth_tex);
	else
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_dlight_depth_tex);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}
