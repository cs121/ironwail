/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "quakedef.h"
#include "draw.h"
#include "r_fogvol.h"
#include <math.h>

extern void R_Clustered_BindForShading (void);

typedef struct fog_volume_gpu_s
{
	float mins[4];
	float maxs[4];
	float color_density[4];
	float noise_params[4];
	float velocity[4];
	float misc[4];
} fog_volume_gpu_t;

static fog_volume_t r_fogvolumes[MAX_FOGVOLUMES];
static int r_fogvolume_count = 0;
static fog_volume_t r_fogvolume_entities[MAX_FOGVOLUMES];
static int r_fogvolume_entity_count = 0;

cvar_t r_fogvol = { "r_fogvol", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_steps = { "r_fogvol_steps", "32", CVAR_ARCHIVE };
cvar_t r_fogvol_halfres = { "r_fogvol_halfres", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_upsample = { "r_fogvol_upsample", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_upsample_k = { "r_fogvol_upsample_k", "100", CVAR_ARCHIVE };
cvar_t r_fogvol_upsample_taps = { "r_fogvol_upsample_taps", "4", CVAR_ARCHIVE };
cvar_t r_fogvol_steps_scale_halfres = { "r_fogvol_steps_scale_halfres", "0.5", CVAR_ARCHIVE };
cvar_t r_fogvol_noise = { "r_fogvol_noise", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noisemode = { "r_fogvol_noisemode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes = { "r_fogvol_testvolumes", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes_dumpstate = { "r_fogvol_testvolumes_dumpstate", "0", CVAR_NONE };
cvar_t r_fogvol_test_rate = { "r_fogvol_test_rate", "10", CVAR_ARCHIVE };
cvar_t r_fogvol_test_every_n_frames = { "r_fogvol_test_every_n_frames", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_test_verbose = { "r_fogvol_test_verbose", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_physblend = { "r_fogvol_physblend", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_alpha = { "r_fogvol_temporal_alpha", "0.9", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_depth_reject = { "r_fogvol_temporal_depth_reject", "0.01", CVAR_ARCHIVE };
cvar_t r_fogvol_jitter = { "r_fogvol_jitter", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal = { "r_fogvol_temporal", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_debug_graph = { "r_fogvol_debug_graph", "0", CVAR_NONE };
cvar_t r_fogvol_hazardlog = { "r_fogvol_hazardlog", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_history_force_clear = { "r_fogvol_history_force_clear", "0", CVAR_NONE };
cvar_t r_fogvol_history_weight_override = { "r_fogvol_history_weight_override", "-1", CVAR_NONE };

static int r_fogvol_history_index = 0;
static int r_fogvol_history_width = 0;
static int r_fogvol_history_height = 0;
static qboolean r_fogvol_history_valid = false;
static qboolean r_fogvol_hazard_logged_frame = false;
static int r_fogvol_hazard_logged_frame_id = -1;
static double r_fogvol_debug_summary_time = 0.0;

typedef struct froxel_grid_s
{
	int width;
	int height;
	int depth;
	GLuint scatter_tex;
	GLuint transmittance_tex;
} froxel_grid_t;

typedef struct fogvol_test_state_s
{
	unsigned statebits;
	unsigned cache_statebits;
	GLint viewport[4];
	GLint scissor_box[4];
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint draw_buffers[8];
	GLint num_draw_buffers;
	GLint draw_status;
	GLint read_status;
	GLint draw_color_type[2];
	GLint draw_color_name[2];
	GLint draw_depth_type;
	GLint draw_depth_name;
	GLint read_color_type;
	GLint read_color_name;
	GLint read_depth_type;
	GLint read_depth_name;
	GLint program;
	GLint vao;
	GLint blend_src_rgb;
	GLint blend_dst_rgb;
	GLint blend_equation_rgb;
	GLint polygon_mode[2];
	GLboolean scissor_test;
	GLboolean blend;
	GLboolean depth_test;
	GLboolean depth_writemask;
	GLboolean color_writemask[4];
	GLboolean cull_face;
	GLfloat color_clear_value[4];
	GLboolean framebuffer_srgb;
	GLboolean dither;
	GLboolean multisample;
	GLint cache_draw_fbo;
	GLint cache_read_fbo;
	GLint cache_program;
	GLint cache_vao;
} fogvol_test_state_t;

typedef struct fogvol_state_cache_s
{
	GLint draw_fbo;
	GLint read_fbo;
	GLint program;
	GLint vao;
} fogvol_state_cache_t;

typedef struct fogvol_restore_state_s
{
	GLint viewport[4];
	GLint scissor_box[4];
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint program;
	GLint vao;
	GLboolean scissor_test;
} fogvol_restore_state_t;

static fogvol_state_cache_t r_fogvol_state_cache = { 0, 0, 0, 0 };

typedef struct fogvol_test_marker_counter_s
{
	char marker[64];
	int count;
} fogvol_test_marker_counter_t;

typedef struct fogvol_test_log_snapshot_s
{
	GLint viewport[4];
	GLint scissor_box[4];
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint program;
	GLboolean scissor_enabled;
} fogvol_test_log_snapshot_t;

typedef struct fogvol_test_logctrl_s
{
	int frame_id;
	fogvol_test_marker_counter_t marker_counts[32];
	int marker_count;
	double window_start;
	double last_suppressed_report;
	int emitted_in_window;
	int suppressed_in_window;
	qboolean has_last_snapshot;
	fogvol_test_log_snapshot_t last_snapshot;
} fogvol_test_logctrl_t;

static fogvol_test_logctrl_t r_fogvol_test_logctrl = {
	-1,
	{{0}},
	0,
	0.0,
	0.0,
	0,
	0,
	false,
	{{0}, {0}, 0, 0, 0, 0, 0, 0}
};

static int R_FogVol_TestFrameId (void)
{
	return (r_framecount > 0) ? r_framecount : host_framecount;
}

static void R_FogVol_TestLog_ReportSuppressedIfNeeded (void)
{
	if (r_fogvol_test_logctrl.suppressed_in_window <= 0)
		return;
	if ((realtime - r_fogvol_test_logctrl.last_suppressed_report) < 1.0)
		return;
	Con_Printf ("FOGVOL_TEST frame_id=%d host_frame=%d marker=SUPPRESSED_SUMMARY call_index=0 callsite=%s:%d suppressed %d logs\n",
		R_FogVol_TestFrameId (),
		host_framecount,
		__FILE__,
		__LINE__,
		r_fogvol_test_logctrl.suppressed_in_window);
	r_fogvol_test_logctrl.last_suppressed_report = realtime;
	r_fogvol_test_logctrl.suppressed_in_window = 0;
}

static void R_FogVol_TestLog_PrepareFrame (void)
{
	const int frame_id = R_FogVol_TestFrameId ();

	if (r_fogvol_test_logctrl.frame_id != frame_id)
	{
		r_fogvol_test_logctrl.frame_id = frame_id;
		r_fogvol_test_logctrl.marker_count = 0;
	}

	if ((realtime - r_fogvol_test_logctrl.window_start) >= 1.0)
	{
		R_FogVol_TestLog_ReportSuppressedIfNeeded ();
		r_fogvol_test_logctrl.window_start = realtime;
		r_fogvol_test_logctrl.emitted_in_window = 0;
	}
}

static int R_FogVol_TestLog_NextCallIndex (const char *marker)
{
	for (int i = 0; i < r_fogvol_test_logctrl.marker_count; ++i)
	{
		if (!q_strcasecmp (r_fogvol_test_logctrl.marker_counts[i].marker, marker))
			return r_fogvol_test_logctrl.marker_counts[i].count++;
	}

	if (r_fogvol_test_logctrl.marker_count < (int)countof (r_fogvol_test_logctrl.marker_counts))
	{
		fogvol_test_marker_counter_t *entry = &r_fogvol_test_logctrl.marker_counts[r_fogvol_test_logctrl.marker_count++];
		q_strlcpy (entry->marker, marker, sizeof (entry->marker));
		entry->count = 1;
		return 0;
	}

	return 0;
}

static qboolean R_FogVol_TestLog_AllowLog (void)
{
	int every_n_frames = (int)Q_rint (r_fogvol_test_every_n_frames.value);
	int rate = (int)Q_rint (r_fogvol_test_rate.value);

	if (every_n_frames < 1)
		every_n_frames = 1;

	if ((R_FogVol_TestFrameId () % every_n_frames) != 0)
	{
		++r_fogvol_test_logctrl.suppressed_in_window;
		R_FogVol_TestLog_ReportSuppressedIfNeeded ();
		return false;
	}

	if (rate > 0 && r_fogvol_test_logctrl.emitted_in_window >= rate)
	{
		++r_fogvol_test_logctrl.suppressed_in_window;
		R_FogVol_TestLog_ReportSuppressedIfNeeded ();
		return false;
	}

	++r_fogvol_test_logctrl.emitted_in_window;
	return true;
}

static qboolean R_FogVol_TestLog_ShouldEmitSnapshot (const fogvol_test_log_snapshot_t *snapshot)
{
	if (r_fogvol_test_verbose.value >= 2.f)
		return true;
	if (!r_fogvol_test_logctrl.has_last_snapshot)
	{
		r_fogvol_test_logctrl.last_snapshot = *snapshot;
		r_fogvol_test_logctrl.has_last_snapshot = true;
		return true;
	}
	if (memcmp (&r_fogvol_test_logctrl.last_snapshot, snapshot, sizeof (*snapshot)) != 0)
	{
		r_fogvol_test_logctrl.last_snapshot = *snapshot;
		return true;
	}
	return false;
}

static qboolean R_FogVol_TestLog_Begin (const char *marker, const char *file, int line, int *call_index)
{
	R_FogVol_TestLog_PrepareFrame ();
	*call_index = R_FogVol_TestLog_NextCallIndex (marker);
	if (!R_FogVol_TestLog_AllowLog ())
		return false;
	(void)file;
	(void)line;
	return true;
}

#define R_FogVol_LogBufferMarker(marker) R_FogVol_LogBufferMarker_Impl ((marker), __FILE__, __LINE__)
#define R_FogVol_SetDrawBufferDebug(buf, marker) R_FogVol_SetDrawBufferDebug_Impl ((buf), (marker), __FILE__, __LINE__)
#define R_FogVol_SetReadBufferDebug(buf, marker) R_FogVol_SetReadBufferDebug_Impl ((buf), (marker), __FILE__, __LINE__)
#define R_FogVol_LogPipelineState(marker) R_FogVol_LogPipelineState_Impl ((marker), __FILE__, __LINE__)
#define R_FogVol_TestState_Log(phase, state) R_FogVol_TestState_Log_Impl ((phase), (state), __FILE__, __LINE__)

static void R_FogVol_BindFramebuffer (GLenum target, GLuint fbo)
{
	GL_BindFramebufferFunc (target, fbo);
	if (target == GL_FRAMEBUFFER)
	{
		r_fogvol_state_cache.draw_fbo = (GLint)fbo;
		r_fogvol_state_cache.read_fbo = (GLint)fbo;
	}
	else if (target == GL_DRAW_FRAMEBUFFER)
		r_fogvol_state_cache.draw_fbo = (GLint)fbo;
	else if (target == GL_READ_FRAMEBUFFER)
		r_fogvol_state_cache.read_fbo = (GLint)fbo;
}

static void R_FogVol_UseProgram (GLuint prog)
{
	GL_UseProgram (prog);
	r_fogvol_state_cache.program = (GLint)prog;
}

static void R_FogVol_BindVertexArray (GLuint vao)
{
	GL_BindVertexArrayFunc (vao);
	r_fogvol_state_cache.vao = (GLint)vao;
}

static void R_FogVol_SetDepthMask (qboolean enabled)
{
	if (enabled)
		GL_SetState (glstate & ~GLS_NO_ZWRITE);
	else
		GL_SetState (glstate | GLS_NO_ZWRITE);
}

static qboolean R_FogVol_TestDebugEnabled (void)
{
	return r_fogvol_testvolumes.value > 0.f || r_fogvol_testvolumes_dumpstate.value > 0.f;
}

static void R_FogVol_LogBufferMarker_Impl (const char *marker, const char *file, int line)
{
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;
	int call_index = 0;

	if (!R_FogVol_TestDebugEnabled ())
		return;
	if (!R_FogVol_TestLog_Begin (marker, file, line, &call_index))
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);
	Con_Printf ("FOGVOL_TEST frame_id=%d host_frame=%d marker=%s call_index=%d callsite=%s:%d draw_fbo=%d read_fbo=%d draw_buffer=0x%04x read_buffer=0x%04x\n",
		R_FogVol_TestFrameId (), host_framecount, marker, call_index, file, line,
		draw_fbo, read_fbo, (unsigned)draw_buffer, (unsigned)read_buffer);
}

static void R_FogVol_SetDrawBufferDebug_Impl (GLenum buf, const char *marker, const char *file, int line)
{
	glDrawBuffer (buf);
	R_FogVol_LogBufferMarker_Impl (marker, file, line);
}

static void R_FogVol_SetReadBufferDebug_Impl (GLenum buf, const char *marker, const char *file, int line)
{
	glReadBuffer (buf);
	R_FogVol_LogBufferMarker_Impl (marker, file, line);
}

static void R_FogVol_LogPipelineState_Impl (const char *marker, const char *file, int line)
{
	fogvol_test_log_snapshot_t snapshot;
	GLint viewport[4] = {0};
	GLint scissor_box[4] = {0};
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;
	GLint program = 0;
	GLboolean scissor_enabled = GL_FALSE;
	int call_index = 0;

	if (!R_FogVol_TestDebugEnabled ())
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, viewport);
	scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);

	snapshot.viewport[0] = viewport[0];
	snapshot.viewport[1] = viewport[1];
	snapshot.viewport[2] = viewport[2];
	snapshot.viewport[3] = viewport[3];
	snapshot.scissor_box[0] = scissor_box[0];
	snapshot.scissor_box[1] = scissor_box[1];
	snapshot.scissor_box[2] = scissor_box[2];
	snapshot.scissor_box[3] = scissor_box[3];
	snapshot.draw_fbo = draw_fbo;
	snapshot.read_fbo = read_fbo;
	snapshot.draw_buffer = draw_buffer;
	snapshot.read_buffer = read_buffer;
	snapshot.program = program;
	snapshot.scissor_enabled = scissor_enabled;

	if (!R_FogVol_TestLog_Begin (marker, file, line, &call_index))
		return;
	if (!R_FogVol_TestLog_ShouldEmitSnapshot (&snapshot))
		return;

	Con_Printf (
		"FOGVOL_TEST frame_id=%d host_frame=%d marker=%s call_index=%d callsite=%s:%d draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) "
		"scissor=%d scissor_box=(%d %d %d %d) prog=%d draw_buffer=0x%04x read_buffer=0x%04x\n",
		R_FogVol_TestFrameId (), host_framecount, marker, call_index, file, line,
		draw_fbo,
		read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_enabled,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		program,
		(unsigned)draw_buffer,
		(unsigned)read_buffer);
}

static GLuint R_FogVol_GetFramebufferColorAttachmentTexture (GLenum target)
{
	GLint object_type = GL_NONE;
	GLint object_name = 0;

	GL_GetFramebufferAttachmentParameterivFunc (target, GL_COLOR_ATTACHMENT0,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object_type);
	if (object_type == GL_TEXTURE)
	{
		GL_GetFramebufferAttachmentParameterivFunc (target, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object_name);
		return (GLuint)object_name;
	}

	return 0;
}

static void R_FogVol_LogHazardPass (const char *pass,
	GLuint main_tex,
	GLuint history_tex,
	GLuint fog_tex)
{
	GLint draw_fbo = 0;
	GLint read_fbo = 0;
	GLuint draw_tex = 0;
	GLuint read_tex = 0;

	if (r_fogvol_hazardlog.value <= 0.f)
		return;
	if (!R_FogVol_TestDebugEnabled () && r_fogvol_debug_graph.value <= 0.f)
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	draw_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_DRAW_FRAMEBUFFER);
	read_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_READ_FRAMEBUFFER);

	Con_Printf (
		"FOGVOL_HAZARD pass=%s draw_fbo=%d read_fbo=%d draw_tex=%u read_tex=%u input_main=%u input_history=%u input_fog=%u\n",
		pass,
		draw_fbo,
		read_fbo,
		draw_tex,
		read_tex,
		main_tex,
		history_tex,
		fog_tex);

	if (draw_tex && (main_tex == draw_tex || history_tex == draw_tex || fog_tex == draw_tex))
	{
		Con_Printf ("FOGVOL_HAZARD pass=%s hazard=feedback_loop draw_tex=%u\n", pass, draw_tex);
	}

	if (!q_strcasecmp (pass, "FINAL_COPY") && draw_tex && read_tex && draw_tex == read_tex)
	{
		Con_Printf ("FOGVOL_HAZARD pass=%s hazard=inplace_copy tex=%u\n", pass, draw_tex);
	}
}

static void R_FogVol_AssertNoFeedbackHazard (const char *pass_name, GLuint draw_tex, GLuint read_tex)
{
#if !defined(NDEBUG)
	if (draw_tex && read_tex && draw_tex == read_tex)
		Sys_Error ("Fogvol feedback hazard in pass %s", pass_name);
#else
	(void)pass_name;
	(void)draw_tex;
	(void)read_tex;
#endif
}

static void R_FogVol_AssertNoBoundFeedbackHazard (const char *pass_name)
{
#if !defined(NDEBUG)
	const GLuint draw_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_DRAW_FRAMEBUFFER);
	const GLuint read_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_READ_FRAMEBUFFER);
	R_FogVol_AssertNoFeedbackHazard (pass_name, draw_tex, read_tex);
#else
	(void)pass_name;
#endif
}

static void R_FogVol_SetHistoryValid (qboolean valid, const char *reason)
{
	if (r_fogvol_history_valid == valid)
		return;
	r_fogvol_history_valid = valid;
	if (r_fogvol_debug_graph.value > 0.f)
	{
		Con_Printf ("FOGVOL_HISTORY_VALID %d reason=%s\n", valid ? 1 : 0, reason ? reason : "unknown");
	}
}

static void R_FogVol_ClearHistoryTexture (GLuint tex, GLuint fbo, int width, int height)
{
	if (!tex || !fbo || width <= 0 || height <= 0)
		return;
	R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, fbo);
	R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY_CLEAR draw=COLOR_ATTACHMENT0");
	glViewport (0, 0, width, height);
	glDisable (GL_SCISSOR_TEST);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor (0.f, 0.f, 0.f, 0.f);
	glClear (GL_COLOR_BUFFER_BIT);
}

static GLuint R_FogVol_GetAttachmentTextureByIndex (GLenum target, int attachment_index)
{
	GLint object_type = GL_NONE;
	GLint object_name = 0;
	if (attachment_index < 0)
		return 0;
	GL_GetFramebufferAttachmentParameterivFunc (target, GL_COLOR_ATTACHMENT0 + attachment_index,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object_type);
	if (object_type != GL_TEXTURE)
		return 0;
	GL_GetFramebufferAttachmentParameterivFunc (target, GL_COLOR_ATTACHMENT0 + attachment_index,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object_name);
	return (GLuint)object_name;
}

static qboolean R_FogVol_LogHazardOncePerFrame (const char *pass, const char *reason)
{
	const int frame_id = R_FogVol_TestFrameId ();
	if (r_fogvol_hazard_logged_frame_id != frame_id)
	{
		r_fogvol_hazard_logged_frame_id = frame_id;
		r_fogvol_hazard_logged_frame = false;
	}
	if (r_fogvol_hazard_logged_frame)
		return false;
	r_fogvol_hazard_logged_frame = true;
	Con_Printf ("FOGVOL_HAZARD pass=%s hazard=%s\n", pass, reason);
	return true;
}

static qboolean R_FogVol_PassHasFeedbackHazard (const char *pass, GLuint draw_tex, const GLuint *read_tex, int num_read_tex, qboolean log_hazard)
{
	GLint draw_fbo = 0, read_fbo = 0, viewport[4] = {0};
	GLuint attachments[2] = {0, 0};
	qboolean hazard = false;
	char reason[128] = "";
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, viewport);
	attachments[0] = R_FogVol_GetAttachmentTextureByIndex (GL_DRAW_FRAMEBUFFER, 0);
	attachments[1] = R_FogVol_GetAttachmentTextureByIndex (GL_DRAW_FRAMEBUFFER, 1);

	for (int i = 0; i < num_read_tex; ++i)
	{
		if (!read_tex[i])
			continue;
		if (draw_tex && read_tex[i] == draw_tex)
		{
			hazard = true;
			q_snprintf (reason, sizeof (reason), "read_tex[%d]=%u equals draw_tex=%u", i, read_tex[i], draw_tex);
			break;
		}
		for (int a = 0; a < 2; ++a)
		{
			if (attachments[a] && read_tex[i] == attachments[a])
			{
				hazard = true;
				q_snprintf (reason, sizeof (reason), "read_tex[%d]=%u attached to draw_fbo color%d", i, read_tex[i], a);
				break;
			}
		}
		if (hazard)
			break;
	}

	if (r_fogvol_debug_graph.value > 0.f)
	{
		Con_Printf ("FOGVOL_GRAPH pass=%s draw_fbo=%d read_fbo=%d viewport=%dx%d draw_tex=%u read_tex0=%u read_tex1=%u att0=%u att1=%u srgb=%d msaa=%d\n",
			pass, draw_fbo, read_fbo, viewport[2], viewport[3], draw_tex,
			num_read_tex > 0 ? read_tex[0] : 0, num_read_tex > 1 ? read_tex[1] : 0, attachments[0], attachments[1],
			glIsEnabled(GL_FRAMEBUFFER_SRGB) ? 1 : 0, glIsEnabled(GL_MULTISAMPLE) ? 1 : 0);
	}

	if (hazard)
	{
		if (log_hazard && r_fogvol_hazardlog.value > 0.f)
			R_FogVol_LogHazardOncePerFrame (pass, reason);
		R_FogVol_SetHistoryValid (false, "hazard");
	}

	return hazard;
}

static void R_FogVol_TestState_Capture (fogvol_test_state_t *state)
{
	GLenum draw_color_attachment = GL_BACK_LEFT;
	GLenum read_color_attachment = GL_BACK_LEFT;

	state->statebits = glstate;
	state->cache_statebits = glstate;
	glGetIntegerv (GL_VIEWPORT, state->viewport);
	state->scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, state->scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &state->draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &state->read_buffer);
	state->num_draw_buffers = 0;
	for (int i = 0; i < (int)countof (state->draw_buffers); ++i)
		state->draw_buffers[i] = GL_NONE;
	{
		GLint max_draw_buffers = 0;
		glGetIntegerv (GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
		state->num_draw_buffers = q_min (max_draw_buffers, (int)countof (state->draw_buffers));
		for (int i = 0; i < state->num_draw_buffers; ++i)
			glGetIntegerv (GL_DRAW_BUFFER0 + i, &state->draw_buffers[i]);
	}
	state->draw_status = (GLint)GL_CheckFramebufferStatusFunc (GL_DRAW_FRAMEBUFFER);
	state->read_status = (GLint)GL_CheckFramebufferStatusFunc (GL_READ_FRAMEBUFFER);
	if (state->draw_fbo)
		draw_color_attachment = GL_COLOR_ATTACHMENT0;
	if (state->read_fbo)
		read_color_attachment = GL_COLOR_ATTACHMENT0;
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, draw_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->draw_color_type[0]);
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, draw_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->draw_color_name[0]);
	if (state->draw_fbo)
	{
		GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->draw_color_type[1]);
		GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->draw_color_name[1]);
	}
	else
	{
		state->draw_color_type[1] = GL_NONE;
		state->draw_color_name[1] = 0;
	}
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->draw_depth_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->draw_depth_name);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, read_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->read_color_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, read_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->read_color_name);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->read_depth_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->read_depth_name);
	glGetIntegerv (GL_CURRENT_PROGRAM, &state->program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &state->vao);
	state->blend = glIsEnabled (GL_BLEND);
	glGetIntegerv (GL_BLEND_SRC_RGB, &state->blend_src_rgb);
	glGetIntegerv (GL_BLEND_DST_RGB, &state->blend_dst_rgb);
	glGetIntegerv (GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
	state->depth_test = glIsEnabled (GL_DEPTH_TEST);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &state->depth_writemask);
	glGetBooleanv (GL_COLOR_WRITEMASK, state->color_writemask);
	state->cull_face = glIsEnabled (GL_CULL_FACE);
	glGetIntegerv (GL_POLYGON_MODE, state->polygon_mode);
	glGetFloatv (GL_COLOR_CLEAR_VALUE, state->color_clear_value);
	state->framebuffer_srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	state->dither = glIsEnabled (GL_DITHER);
	state->multisample = glIsEnabled (GL_MULTISAMPLE);
	state->cache_draw_fbo = r_fogvol_state_cache.draw_fbo;
	state->cache_read_fbo = r_fogvol_state_cache.read_fbo;
	state->cache_program = r_fogvol_state_cache.program;
	state->cache_vao = r_fogvol_state_cache.vao;
}


static void R_FogVol_TestState_Log_Impl (const char *phase, const fogvol_test_state_t *state, const char *file, int line)
{
	int call_index = 0;

	if (!R_FogVol_TestDebugEnabled ())
		return;
	if (!R_FogVol_TestLog_Begin (phase, file, line, &call_index))
		return;

	Con_Printf (
		"FOGVOL_TEST frame_id=%d host_frame=%d marker=%s call_index=%d callsite=%s:%d "
		"viewport=(%d %d %d %d) scissor_test=%d scissor_box=(%d %d %d %d) "
		"draw_fbo=%d read_fbo=%d draw_buffer=0x%04x read_buffer=0x%04x "
		"draw_buffers=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x] "
		"draw_status=0x%04x read_status=0x%04x "
		"draw_att0=(type=0x%04x,name=%d) draw_att1=(type=0x%04x,name=%d) draw_depth=(type=0x%04x,name=%d) "
		"read_att0=(type=0x%04x,name=%d) read_depth=(type=0x%04x,name=%d) "
		"prog=%d vao=%d blend=%d blend_func=(%d,%d) blend_eq=%d "
		"depth_test=%d depth_writemask=%d color_writemask=(%d %d %d %d) cull=%d poly=(%d,%d) "
		"clear_color=(%.3f %.3f %.3f %.3f) srgb=%d dither=%d multisample=%d glstate=0x%08x "
		"cache(glstate=0x%08x draw_fbo=%d read_fbo=%d prog=%d vao=%d)\n",
		R_FogVol_TestFrameId (), host_framecount, phase, call_index, file, line,
		state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3],
		state->scissor_test,
		state->scissor_box[0], state->scissor_box[1], state->scissor_box[2], state->scissor_box[3],
		state->draw_fbo, state->read_fbo,
		(unsigned)state->draw_buffer, (unsigned)state->read_buffer,
		(unsigned)state->draw_buffers[0], (unsigned)state->draw_buffers[1],
		(unsigned)state->draw_buffers[2], (unsigned)state->draw_buffers[3],
		(unsigned)state->draw_buffers[4], (unsigned)state->draw_buffers[5],
		(unsigned)state->draw_buffers[6], (unsigned)state->draw_buffers[7],
		(unsigned)state->draw_status, (unsigned)state->read_status,
		(unsigned)state->draw_color_type[0], state->draw_color_name[0],
		(unsigned)state->draw_color_type[1], state->draw_color_name[1],
		(unsigned)state->draw_depth_type, state->draw_depth_name,
		(unsigned)state->read_color_type, state->read_color_name,
		(unsigned)state->read_depth_type, state->read_depth_name,
		state->program,
		state->vao,
		state->blend,
		state->blend_src_rgb, state->blend_dst_rgb,
		state->blend_equation_rgb,
		state->depth_test,
		state->depth_writemask,
		state->color_writemask[0], state->color_writemask[1], state->color_writemask[2], state->color_writemask[3],
		state->cull_face,
		state->polygon_mode[0], state->polygon_mode[1],
		state->color_clear_value[0], state->color_clear_value[1], state->color_clear_value[2], state->color_clear_value[3],
		state->framebuffer_srgb,
		state->dither,
		state->multisample,
		state->statebits,
		state->cache_statebits,
		state->cache_draw_fbo,
		state->cache_read_fbo,
		state->cache_program,
		state->cache_vao);
}


static void R_FogVol_CaptureRestoreState (fogvol_restore_state_t *state)
{
	glGetIntegerv (GL_VIEWPORT, state->viewport);
	state->scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, state->scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &state->draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &state->read_buffer);
	glGetIntegerv (GL_CURRENT_PROGRAM, &state->program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &state->vao);
}

static void R_FogVol_Restore3DRenderState (const fogvol_restore_state_t *state)
{
	R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, state->draw_fbo);
	R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, state->read_fbo);
	glDrawBuffer ((GLenum)state->draw_buffer);
	glReadBuffer ((GLenum)state->read_buffer);
	glViewport (state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
	if (state->scissor_test)
		GL_SetScissorEnabled (true);
	else
		GL_SetScissorEnabled (false);
	glScissor (state->scissor_box[0], state->scissor_box[1], state->scissor_box[2], state->scissor_box[3]);
	R_FogVol_UseProgram ((GLuint)state->program);
	R_FogVol_BindVertexArray ((GLuint)state->vao);

	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	R_FogVol_SetDepthMask (true);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (0));
}

static qboolean R_FogVol_MatrixInverse4x4 (const float m[16], float out[16])
{
	float inv[16];
	float det;

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
		m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15]
		- m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15]
		+ m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14]
		- m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15]
		- m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15]
		+ m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15]
		- m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14]
		+ m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15]
		+ m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15]
		- m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15]
		+ m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14]
		- m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
		- m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
		+ m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
		- m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
		+ m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (fabsf (det) < 1e-8f)
		return false;

	det = 1.f / det;
	for (int i = 0; i < 16; ++i)
		out[i] = inv[i] * det;
	return true;
}

static int R_FogVol_ComparePriority (const void *a, const void *b)
{
	const fog_volume_t *va = (const fog_volume_t *)a;
	const fog_volume_t *vb = (const fog_volume_t *)b;

	if (va->priority < vb->priority)
		return -1;
	if (va->priority > vb->priority)
		return 1;
	return 0;
}

void R_FogVol_Init (void)
{
	Cvar_RegisterVariable (&r_fogvol);
	Cvar_RegisterVariable (&r_fogvol_steps);
	Cvar_RegisterVariable (&r_fogvol_halfres);
	Cvar_RegisterVariable (&r_fogvol_upsample);
	Cvar_RegisterVariable (&r_fogvol_upsample_k);
	Cvar_RegisterVariable (&r_fogvol_upsample_taps);
	Cvar_RegisterVariable (&r_fogvol_steps_scale_halfres);
	Cvar_RegisterVariable (&r_fogvol_noise);
	Cvar_RegisterVariable (&r_fogvol_noisemode);
	Cvar_RegisterVariable (&r_fogvol_testvolumes);
	Cvar_RegisterVariable (&r_fogvol_testvolumes_dumpstate);
	Cvar_RegisterVariable (&r_fogvol_test_rate);
	Cvar_RegisterVariable (&r_fogvol_test_every_n_frames);
	Cvar_RegisterVariable (&r_fogvol_test_verbose);
	Cvar_RegisterVariable (&r_fogvol_physblend);
	Cvar_RegisterVariable (&r_fogvol_temporal_alpha);
	Cvar_RegisterVariable (&r_fogvol_temporal_depth_reject);
	Cvar_RegisterVariable (&r_fogvol_jitter);
	Cvar_RegisterVariable (&r_fogvol_temporal);
	Cvar_RegisterVariable (&r_fogvol_debug_graph);
	Cvar_RegisterVariable (&r_fogvol_hazardlog);
	Cvar_RegisterVariable (&r_fogvol_history_force_clear);
	Cvar_RegisterVariable (&r_fogvol_history_weight_override);
}

void R_FogVol_Clear (void)
{
	r_fogvolume_count = 0;
	R_FogVol_SetHistoryValid (false, "clear");
}

void R_FogVol_NotifyFramebuffersRecreated (void)
{
	r_fogvol_history_width = 0;
	r_fogvol_history_height = 0;
	r_fogvol_history_index = 0;
	R_FogVol_SetHistoryValid (false, "framebuffers_recreated");
}

static void R_FogVol_ClearEntities (void)
{
	r_fogvolume_entity_count = 0;
}

static void R_FogVol_ClampVolume (fog_volume_t *volume)
{
	volume->density = CLAMP (0.f, volume->density, 10.f);
	volume->falloff = CLAMP (0.f, volume->falloff, 256.f);
}

static void R_FogVol_AddVolume (const fog_volume_t *volume)
{
	fog_volume_t clamped;

	if (r_fogvolume_count >= MAX_FOGVOLUMES)
		return;
	clamped = *volume;
	R_FogVol_ClampVolume (&clamped);
	r_fogvolumes[r_fogvolume_count++] = clamped;
}

static void R_FogVol_AddEntityVolume (const fog_volume_t *volume)
{
	fog_volume_t clamped;

	if (r_fogvolume_entity_count >= MAX_FOGVOLUMES)
		return;
	clamped = *volume;
	R_FogVol_ClampVolume (&clamped);
	r_fogvolume_entities[r_fogvolume_entity_count++] = clamped;
}

static void R_FogVol_ParseColor (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
	{
		// Inputs are assumed to be linear; 0..255 values are normalized to 0..1.
		if (r > 2.f || g > 2.f || b > 2.f)
		{
			r *= 1.f / 255.f;
			g *= 1.f / 255.f;
			b *= 1.f / 255.f;
		}
	}
	color[0] = r;
	color[1] = g;
	color[2] = b;
}

static qboolean R_FogVol_ParseVector (const char *value, vec3_t out)
{
	return value && sscanf (value, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

static float R_FogVol_PointAABBDistance (const vec3_t point, const fog_volume_t *volume)
{
	float dist2 = 0.f;

	for (int i = 0; i < 3; ++i)
	{
		if (point[i] < volume->mins[i])
		{
			float d = volume->mins[i] - point[i];
			dist2 += d * d;
		}
		else if (point[i] > volume->maxs[i])
		{
			float d = point[i] - volume->maxs[i];
			dist2 += d * d;
		}
	}

	return sqrtf (dist2);
}

void R_FogVol_ParseEntities (void)
{
	const char *data;

	R_FogVol_ClearEntities ();

	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;

	data = cl.worldmodel->entities;
	data = COM_Parse (data);
	while (data && com_token[0])
	{
		fog_volume_t volume;
		qboolean is_fog_volume = false;
		char modelname[64] = "";
		vec3_t origin = {0.f, 0.f, 0.f};
		qboolean has_origin = false;

		if (com_token[0] != '{')
			break;

		memset (&volume, 0, sizeof (volume));
		VectorSet (volume.color, 1.f, 1.f, 1.f);
		volume.density = 0.1f;
		volume.falloff = 16.f;
		volume.mode = 0;
		volume.noiseScale = 0.05f;
		volume.noiseAmount = 0.5f;
		volume.noiseBias = 0.f;
		VectorSet (volume.velocity, 0.f, 0.f, 0.f);
		volume.maxDistance = 2048.f;
		volume.priority = 0;
		volume.enabled = 1;
		volume.height = 0.f;
		volume.heightScale = 0.f;

		while (1)
		{
			char key[64], value[1024];
			data = COM_Parse (data);
			if (!data || !com_token[0])
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			if (key[0] == '_')
				memmove (key, key + 1, strlen (key));
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));

			if (!strcmp (key, "classname"))
			{
				if (!strcmp (value, "func_fog_volume") || !strcmp (value, "trigger_fog_volume"))
					is_fog_volume = true;
			}
			else if (!strcmp (key, "model"))
			{
				q_strlcpy (modelname, value, sizeof (modelname));
			}
			else if (!strcmp (key, "origin"))
			{
				has_origin = R_FogVol_ParseVector (value, origin);
			}
			else if (!strcmp (key, "_color") || !strcmp (key, "color"))
			{
				R_FogVol_ParseColor (value, volume.color);
			}
			else if (!strcmp (key, "density"))
			{
				volume.density = atof (value);
			}
			else if (!strcmp (key, "falloff"))
			{
				volume.falloff = atof (value);
			}
			else if (!strcmp (key, "maxdist"))
			{
				volume.maxDistance = atof (value);
			}
			else if (!strcmp (key, "priority"))
			{
				volume.priority = atoi (value);
			}
			else if (!strcmp (key, "noise_scale"))
			{
				volume.noiseScale = atof (value);
			}
			else if (!strcmp (key, "noise_amount"))
			{
				volume.noiseAmount = atof (value);
			}
			else if (!strcmp (key, "noise_bias"))
			{
				volume.noiseBias = atof (value);
			}
			else if (!strcmp (key, "velocity"))
			{
				R_FogVol_ParseVector (value, volume.velocity);
			}
			else if (!strcmp (key, "mode"))
			{
				volume.mode = atoi (value);
			}
			else if (!strcmp (key, "height"))
			{
				volume.height = atof (value);
			}
			else if (!strcmp (key, "height_scale"))
			{
				volume.heightScale = atof (value);
			}
		}

		if (is_fog_volume && modelname[0])
		{
			qmodel_t *model = Mod_ForName (modelname, false);
			if (model && model->type == mod_brush)
			{
				vec3_t mins;
				vec3_t maxs;
				VectorCopy (model->mins, mins);
				VectorCopy (model->maxs, maxs);
				if (has_origin)
				{
					VectorAdd (mins, origin, mins);
					VectorAdd (maxs, origin, maxs);
				}
				VectorCopy (mins, volume.mins);
				VectorCopy (maxs, volume.maxs);
				R_FogVol_AddEntityVolume (&volume);
			}
		}

		data = COM_Parse (data);
	}
}

void R_FogVol_AddTestVolumes (void)
{
	fog_volume_t volume;
	vec3_t origin;
	VectorCopy (r_refdef.vieworg, origin);

	memset (&volume, 0, sizeof (volume));
	VectorSet (volume.color, 0.8f, 0.85f, 0.9f);
	volume.density = 0.35f;
	volume.falloff = 24.f;
	volume.mode = 0;
	volume.noiseScale = 0.08f;
	volume.noiseAmount = 0.85f;
	volume.noiseBias = 0.0f;
	VectorSet (volume.velocity, 0.f, 0.f, 6.f);
	volume.maxDistance = 0.f;
	volume.priority = 0;
	volume.enabled = 1;
	VectorSet (volume.mins, origin[0] - 32.f, origin[1] - 32.f, origin[2] - 16.f);
	VectorSet (volume.maxs, origin[0] + 32.f, origin[1] + 32.f, origin[2] + 48.f);
	R_FogVol_AddVolume (&volume);

	memset (&volume, 0, sizeof (volume));
	VectorSet (volume.color, 0.7f, 0.75f, 0.85f);
	volume.density = 0.05f;
	volume.falloff = 32.f;
	volume.mode = 0;
	volume.noiseScale = 0.02f;
	volume.noiseAmount = 0.25f;
	volume.noiseBias = 0.0f;
	VectorSet (volume.velocity, -1.f, 0.5f, 0.f);
	volume.maxDistance = 0.f;
	volume.priority = 1;
	volume.enabled = 1;
	VectorSet (volume.mins, origin[0] - 256.f, origin[1] - 256.f, origin[2] - 128.f);
	VectorSet (volume.maxs, origin[0] + 256.f, origin[1] + 256.f, origin[2] + 128.f);
	R_FogVol_AddVolume (&volume);
}

void R_FogVol_BuildList (void)
{
	R_FogVol_Clear ();

	if (r_fogvol.value <= 0.f)
		return;

	for (int i = 0; i < r_fogvolume_entity_count; ++i)
	{
		const fog_volume_t *volume = &r_fogvolume_entities[i];

		if (!volume->enabled)
			continue;
		if (volume->maxDistance > 0.f)
		{
			float dist = R_FogVol_PointAABBDistance (r_refdef.vieworg, volume);
			if (dist > volume->maxDistance)
				continue;
		}
		R_FogVol_AddVolume (volume);
	}

	if (r_fogvol_testvolumes.value > 0.f)
		R_FogVol_AddTestVolumes ();

	if (r_fogvolume_count > 1)
		qsort (r_fogvolumes, r_fogvolume_count, sizeof (fog_volume_t), R_FogVol_ComparePriority);
}

qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres)
{
	vec3_t corners[8];
	vec3_t proj;
	float minx = 1e30f;
	float miny = 1e30f;
	float maxx = -1e30f;
	float maxy = -1e30f;
	int view_x = 0;
	int view_y = 0;
	int view_w = r_refdef.vrect.width / q_max (1, r_refdef.scale);
	int view_h = r_refdef.vrect.height / q_max (1, r_refdef.scale);
	int valid = 0;

	if (fullres || !GL_NeedsSceneEffects ())
	{
		view_x = glx + r_refdef.vrect.x;
		view_y = gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height;
		view_w = r_refdef.vrect.width;
		view_h = r_refdef.vrect.height;
	}

	corners[0][0] = v->mins[0]; corners[0][1] = v->mins[1]; corners[0][2] = v->mins[2];
	corners[1][0] = v->maxs[0]; corners[1][1] = v->mins[1]; corners[1][2] = v->mins[2];
	corners[2][0] = v->mins[0]; corners[2][1] = v->maxs[1]; corners[2][2] = v->mins[2];
	corners[3][0] = v->maxs[0]; corners[3][1] = v->maxs[1]; corners[3][2] = v->mins[2];
	corners[4][0] = v->mins[0]; corners[4][1] = v->mins[1]; corners[4][2] = v->maxs[2];
	corners[5][0] = v->maxs[0]; corners[5][1] = v->mins[1]; corners[5][2] = v->maxs[2];
	corners[6][0] = v->mins[0]; corners[6][1] = v->maxs[1]; corners[6][2] = v->maxs[2];
	corners[7][0] = v->maxs[0]; corners[7][1] = v->maxs[1]; corners[7][2] = v->maxs[2];

	for (int i = 0; i < 8; ++i)
	{
		ProjectVector (corners[i], r_matviewproj, proj);
		if (proj[2] <= 0.f)
			continue;
		minx = q_min (minx, proj[0]);
		miny = q_min (miny, proj[1]);
		maxx = q_max (maxx, proj[0]);
		maxy = q_max (maxy, proj[1]);
		valid = 1;
	}

	if (!valid)
		return false;

	{
		float fx0 = (minx * 0.5f + 0.5f) * (float)view_w + (float)view_x;
		float fy0 = (miny * 0.5f + 0.5f) * (float)view_h + (float)view_y;
		float fx1 = (maxx * 0.5f + 0.5f) * (float)view_w + (float)view_x;
		float fy1 = (maxy * 0.5f + 0.5f) * (float)view_h + (float)view_y;

		int ix0 = (int)floorf (fx0);
		int iy0 = (int)floorf (fy0);
		int ix1 = (int)ceilf (fx1);
		int iy1 = (int)ceilf (fy1);

		ix0 = CLAMP (view_x, ix0, view_x + view_w);
		iy0 = CLAMP (view_y, iy0, view_y + view_h);
		ix1 = CLAMP (view_x, ix1, view_x + view_w);
		iy1 = CLAMP (view_y, iy1, view_y + view_h);

		if (ix1 <= ix0 || iy1 <= iy0)
			return false;

		*x0 = ix0;
		*y0 = iy0;
		*x1 = ix1;
		*y1 = iy1;
	}

	return true;
}

void R_FogVol_DrawDebug2D (void)
{
}

void R_FogVol_Render (void)
{
	static int last_dumpstate = -1;
	int steps;
	const int mode = CLAMP (0, (int)Q_rint (r_fogvol_testvolumes.value), 11);
	GLuint buf;
	GLbyte *ofs;
	fog_volume_gpu_t gpu_volumes[MAX_FOGVOLUMES];
	float inv_viewproj[16];
	GLuint src_tex;
	GLuint dst_tex;
	GLuint dst_fbo;
	GLuint depth_tex;
	GLuint fog_tex[2];
	GLuint fog_fbo[2];
	GLuint history_tex[2];
	GLuint history_fbo[2];
	qboolean has_drawn = false;
	qboolean use_halfres;
	int fog_width;
	int fog_height;
	float depth_scale_x;
	float depth_scale_y;
	float view_x;
	float view_y;
	float view_w;
	float view_h;
	int fog_src = 0;
	int fog_dst = 0;
	GLuint final_tex = 0;
	GLuint composite_src_tex = 0;
	GLuint composite_src_fbo = 0;
	qboolean use_test_guard;
	qboolean dumpstate_always;
	fogvol_restore_state_t restore_state;

	if (r_fogvol.value <= 0.f)
		return;
	if (!glprogs.fogvol)
		return;
	if (r_fogvolume_count <= 0)
		return;
	if (framebufs.composite.color_tex == 0 || framebufs.fogvol.color_tex[0] == 0)
		return;
	if (framebufs.composite.depth_stencil_tex == 0)
		return;
	if (!R_FogVol_MatrixInverse4x4 (r_matviewproj, inv_viewproj))
		return;

	dumpstate_always = (r_fogvol_testvolumes_dumpstate.value > 0.f);
	if (last_dumpstate != (int)Q_rint (r_fogvol_testvolumes_dumpstate.value))
	{
		int call_index = 0;
		if (R_FogVol_TestLog_Begin ("DUMPSTATE", __FILE__, __LINE__, &call_index))
		{
			Con_Printf ("FOGVOL_TEST frame_id=%d host_frame=%d marker=DUMPSTATE call_index=%d callsite=%s:%d dumpstate %s\n",
				R_FogVol_TestFrameId (), host_framecount, call_index, __FILE__, __LINE__,
				dumpstate_always ? "enabled" : "disabled");
		}
		last_dumpstate = (int)Q_rint (r_fogvol_testvolumes_dumpstate.value);
	}

	use_test_guard = (r_fogvol_testvolumes.value > 0.f);
	if (use_test_guard)
		R_FogVol_CaptureRestoreState (&restore_state);
	R_FogVol_LogPipelineState ("FOGVOL_BEGIN");
	if (use_test_guard)
		R_FogVol_LogBufferMarker ("pre");

	use_halfres = (r_fogvol_halfres.value > 0.f);
	fog_width = use_halfres ? framebufs.fogvol.width : glwidth;
	fog_height = use_halfres ? framebufs.fogvol.height : glheight;
	depth_scale_x = (float)glwidth / (float)fog_width;
	depth_scale_y = (float)glheight / (float)fog_height;
	view_x = (float)(glx + r_refdef.vrect.x);
	view_y = (float)(gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height);
	view_w = (float)r_refdef.vrect.width;
	view_h = (float)r_refdef.vrect.height;

	if (use_halfres && r_fogvol_steps_scale_halfres.value > 0.f)
		steps = (int)Q_rint (r_fogvol_steps.value * r_fogvol_steps_scale_halfres.value);
	else
		steps = (int)Q_rint (r_fogvol_steps.value);
	steps = CLAMP (8, steps, 128);

	if (r_fogvol_testvolumes.value > 0.f && r_fogvol_test_verbose.value > 0.f && (realtime - r_fogvol_debug_summary_time) >= 1.0)
	{
		const int lightgrid_active = (r_framedata.lightgrid_params[0] > 0.5f);
		const int dlights_active = (r_framedata.numlights > 0 && r_framedata.dlight_params[3] > 0.f);
		const int sun_active = (r_framedata.shadow_debug[0] > 0.5f);
		const int ambient_active = (r_framedata.fogdata[3] > 0.f);
		Con_Printf ("FOGVOL_SUMMARY grid=%dx%d steps=%d z=ray-linear halfres=%d format=fogvol_rgba16f active_lights ambient=%d static=%d dyn=%d sun=%d phys=%d\n",
			fog_width, fog_height, steps, use_halfres ? 1 : 0, ambient_active, lightgrid_active, dlights_active, sun_active,
			r_fogvol_physblend.value > 0.f ? 1 : 0);
		r_fogvol_debug_summary_time = realtime;
	}

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		fog_volume_gpu_t *gpu = &gpu_volumes[i];

		gpu->mins[0] = v->mins[0];
		gpu->mins[1] = v->mins[1];
		gpu->mins[2] = v->mins[2];
		gpu->mins[3] = 0.f;

		gpu->maxs[0] = v->maxs[0];
		gpu->maxs[1] = v->maxs[1];
		gpu->maxs[2] = v->maxs[2];
		gpu->maxs[3] = 0.f;

		gpu->color_density[0] = v->color[0];
		gpu->color_density[1] = v->color[1];
		gpu->color_density[2] = v->color[2];
		gpu->color_density[3] = v->density;

		gpu->noise_params[0] = CLAMP (0.005f, v->noiseScale, 0.5f);
		gpu->noise_params[1] = CLAMP (0.f, v->noiseAmount, 1.f);
		gpu->noise_params[2] = v->noiseBias;
		gpu->noise_params[3] = v->maxDistance;

		gpu->velocity[0] = v->velocity[0];
		gpu->velocity[1] = v->velocity[1];
		gpu->velocity[2] = v->velocity[2];
		gpu->velocity[3] = 0.f;

		gpu->misc[0] = (float)v->priority;
		gpu->misc[1] = (float)v->enabled;
		gpu->misc[2] = v->falloff;
		gpu->misc[3] = (float)v->mode;
	}

	GL_Upload (GL_UNIFORM_BUFFER, gpu_volumes, sizeof (fog_volume_gpu_t) * r_fogvolume_count, &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 12, buf, (GLintptr)ofs, sizeof (fog_volume_gpu_t) * r_fogvolume_count);

	GL_BeginGroup ("Fog volumes");
	// Pipeline map: SceneColor + SceneDepth + FrameData + clustered-light buffers feed
	// the fog volume raymarch pass (physical blend: scene*T + inscatter), then optional
	// temporal resolve + optional depth-aware upsample before compositing back to framebufs.composite.
	// Space conventions: SceneDepth (clip/NDC) -> world via FogInvViewProj, march in world/view distance.
	// Validation checklist (debug views via r_fogvol_testvolumes):
	//  - Nearby surfaces should keep contrast (transmittance near 1), distant pixels should fade.
	//  - Colored dynlights should tint inscattering; disabling dynamic lights removes only that term.
	//  - Maps with/without lightgrid should stay plausible via ambient/static fallback terms.
	R_Clustered_BindForShading ();
	R_FogVol_UseProgram (glprogs.fogvol);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_SetScissorEnabled (false);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	GL_Uniform1iFunc (0, steps);
	GL_Uniform1iFunc (1, r_fogvol_noise.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (2, mode);
	GL_Uniform1iFunc (5, (int)Q_rint (r_fogvol_noisemode.value));
	GL_Uniform1iFunc (6, r_fogvol_physblend.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (7, r_fogvol_jitter.value > 0.f ? 1 : 0);
	GL_UniformMatrix4fvFunc (4, 1, GL_FALSE, inv_viewproj);
	GL_Uniform3fFunc (8, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	GL_Uniform4fFunc (9, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);
	GL_Uniform2fFunc (10, depth_scale_x, depth_scale_y);
	GL_Uniform4fFunc (11, view_x, view_y, 1.f / view_w, 1.f / view_h);

	if (use_halfres)
		glViewport (0, 0, fog_width, fog_height);
	else
		glViewport ((int)view_x, (int)view_y, (int)view_w, (int)view_h);
	depth_tex = framebufs.composite.depth_stencil_tex;
	fog_tex[0] = framebufs.fogvol.color_tex[0];
	fog_tex[1] = framebufs.fogvol.color_tex[1];
	fog_fbo[0] = framebufs.fogvol.fbo[0];
	fog_fbo[1] = framebufs.fogvol.fbo[1];
	history_tex[0] = framebufs.fogvol.history_tex[0];
	history_tex[1] = framebufs.fogvol.history_tex[1];
	history_fbo[0] = framebufs.fogvol.history_fbo[0];
	history_fbo[1] = framebufs.fogvol.history_fbo[1];
	src_tex = framebufs.composite.color_tex;
	final_tex = 0;

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;
		GLuint src_fbo;

		if (!v->enabled)
			continue;
		if (!R_FogVol_ProjectAABBToScreenRect (v, &x0, &y0, &x1, &y1, true))
			continue;
		if (use_halfres)
		{
			float scale_x = (float)fog_width / (float)glwidth;
			float scale_y = (float)fog_height / (float)glheight;
			int hx0 = (int)floorf ((float)x0 * scale_x);
			int hy0 = (int)floorf ((float)y0 * scale_y);
			int hx1 = (int)ceilf ((float)x1 * scale_x);
			int hy1 = (int)ceilf ((float)y1 * scale_y);
			x0 = CLAMP (0, hx0, fog_width);
			y0 = CLAMP (0, hy0, fog_height);
			x1 = CLAMP (0, hx1, fog_width);
			y1 = CLAMP (0, hy1, fog_height);
			if (x1 <= x0 || y1 <= y0)
				continue;
		}

		if (mode == 1)
		{
			vec3_t color;
			VectorCopy (v->color, color);
			R_DebugDrawWireBox (v->mins, v->maxs, color, true);
		}

		fog_dst = (i == 0) ? 0 : (1 - fog_src);
		dst_tex = fog_tex[fog_dst];
		dst_fbo = fog_fbo[fog_dst];

		if (i == 0)
		{
			src_tex = framebufs.composite.color_tex;
			src_fbo = framebufs.composite.fbo;
			GL_SetScissorEnabled (false);
			R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, src_fbo);
			R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.fogvol.finalcopy_fbo);
			R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "FINAL_COPY read=COLOR_ATTACHMENT0");
			R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "FINAL_COPY draw=COLOR_ATTACHMENT0");
			R_FogVol_LogHazardPass ("FINAL_COPY", src_tex, 0, src_tex);
			R_FogVol_AssertNoFeedbackHazard ("FINAL_COPY", framebufs.fogvol.finalcopy_tex, src_tex);
			if (use_halfres)
			{
				GL_BlitFramebufferFunc (0, 0, glwidth, glheight,
					0, 0, fog_width, fog_height,
					GL_COLOR_BUFFER_BIT, GL_LINEAR);
			}
			else
			{
				GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
					0, 0, fog_width, fog_height,
					GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			src_tex = framebufs.fogvol.finalcopy_tex;
		}
		else
		{
			src_tex = fog_tex[fog_src];
			src_fbo = fog_fbo[fog_src];
		}

		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, src_fbo);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, dst_fbo);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "ITER read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "ITER draw=COLOR_ATTACHMENT0");
		R_FogVol_LogHazardPass ("ITER", src_tex, 0, src_tex);
		R_FogVol_AssertNoFeedbackHazard ("ITER", dst_tex, src_tex);
		R_FogVol_AssertNoBoundFeedbackHazard ("ITER");
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, src_tex);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, depth_tex);
		GL_SetScissorEnabled (true);
		glScissor (x0, y0, x1 - x0, y1 - y0);
		GL_Uniform1iFunc (3, i);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		GL_SetScissorEnabled (false);

		fog_src = fog_dst;
		final_tex = fog_tex[fog_src];
		has_drawn = true;
	}
	GL_SetScissorEnabled (false);
	GLuint final_fbo = framebufs.fogvol.fbo[fog_src];

	if (!has_drawn)
	{
		R_FogVol_BindFramebuffer (GL_FRAMEBUFFER, framebufs.composite.fbo);
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "temporal draw=COLOR_ATTACHMENT0");
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "temporal read=COLOR_ATTACHMENT0");
		glViewport (glx, gly, glwidth, glheight);
		goto done;
	}

	if (has_drawn && r_fogvol_temporal.value > 0.f && glprogs.fogvol_temporal && final_tex)
	{
		int history_valid = (r_fogvol_history_valid && r_fogvol_history_width == fog_width && r_fogvol_history_height == fog_height);
		int history_src = r_fogvol_history_index;
		int history_dst = 1 - history_src;
		int composite_src = history_src;
		int composite_dst = 1 - composite_src;
		if (!history_valid)
		{
			r_fogvol_history_width = fog_width;
			r_fogvol_history_height = fog_height;
			history_src = 0;
			history_dst = 1;
			composite_src = 0;
			composite_dst = 1;
			r_fogvol_history_index = history_src;
			R_FogVol_ClearHistoryTexture (history_tex[0], history_fbo[0], fog_width, fog_height);
			R_FogVol_ClearHistoryTexture (history_tex[1], history_fbo[1], fog_width, fog_height);
			R_FogVol_SetHistoryValid (false, "history_resize_or_init");
		}

		if (r_fogvol_history_force_clear.value > 0.f)
		{
			R_FogVol_ClearHistoryTexture (history_tex[history_src], history_fbo[history_src], fog_width, fog_height);
			history_valid = 0;
			R_FogVol_SetHistoryValid (false, "history_force_clear");
		}

		if (!history_tex[history_src])
		{
			history_valid = 0;
			R_FogVol_SetHistoryValid (false, "history_tex_zero");
		}

		R_FogVol_UseProgram (glprogs.fogvol_temporal);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		if (history_src == history_dst)
			history_dst = 1 - history_src;
		if (composite_src == composite_dst)
			composite_dst = 1 - composite_src;
		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, history_fbo[history_src]);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.fogvol.composite_fbo[composite_dst]);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "COMPOSITE read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "COMPOSITE draw=COLOR_ATTACHMENT0");
		glViewport (0, 0, fog_width, fog_height);
		{
			GLuint read_texes[2] = { final_tex, history_valid ? history_tex[history_src] : 0 };
			qboolean hazard = R_FogVol_PassHasFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], read_texes, 2, true);
			if (hazard)
				history_valid = 0;
		}
		R_FogVol_AssertNoFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], final_tex);
		R_FogVol_AssertNoFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], history_tex[history_src]);
		R_FogVol_AssertNoBoundFeedbackHazard ("COMPOSITE");
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, final_tex);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, history_valid ? history_tex[history_src] : 0);
		GL_BindNative (GL_TEXTURE2, GL_TEXTURE_2D, depth_tex);
		R_FogVol_LogHazardPass ("COMPOSITE", final_tex, history_valid ? history_tex[history_src] : 0, final_tex);
		{
			float history_alpha = r_fogvol_temporal_alpha.value;
			if (r_fogvol_history_weight_override.value >= 0.f)
				history_alpha = CLAMP (0.f, r_fogvol_history_weight_override.value, 1.f);
			if (!history_valid)
				history_alpha = 0.f;
			GL_Uniform1fFunc (0, history_alpha);
		}
		GL_Uniform1fFunc (1, r_fogvol_temporal_depth_reject.value);
		GL_Uniform1iFunc (2, mode);
		GL_UniformMatrix4fvFunc (3, 1, GL_FALSE, inv_viewproj);
		GL_Uniform4fFunc (4, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);
		GL_Uniform2fFunc (5, depth_scale_x, depth_scale_y);
		GL_Uniform1iFunc (6, history_valid ? 1 : 0);
		GL_Uniform4fFunc (7, view_x, view_y, 1.f / view_w, 1.f / view_h);
		glDrawArrays (GL_TRIANGLES, 0, 3);

		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, framebufs.fogvol.composite_fbo[composite_dst]);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, history_fbo[history_dst]);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
		R_FogVol_AssertNoFeedbackHazard ("HISTORY", history_tex[history_dst], framebufs.fogvol.composite_tex[composite_dst]);
		R_FogVol_AssertNoBoundFeedbackHazard ("HISTORY");
		R_FogVol_LogHazardPass ("HISTORY", framebufs.fogvol.composite_tex[composite_dst], 0, framebufs.fogvol.composite_tex[composite_dst]);
		GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
			0, 0, fog_width, fog_height,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		composite_src_tex = framebufs.fogvol.composite_tex[composite_dst];
		composite_src_fbo = framebufs.fogvol.composite_fbo[composite_dst];
		final_tex = history_tex[history_dst];
		final_fbo = history_fbo[history_dst];
		r_fogvol_history_index = history_dst;
		r_fogvol_history_width = fog_width;
		r_fogvol_history_height = fog_height;
		R_FogVol_SetHistoryValid (true, "history_written");
	}

	if (has_drawn)
	{
		if (!(r_fogvol_temporal.value > 0.f && glprogs.fogvol_temporal && final_tex))
			R_FogVol_SetHistoryValid (false, "temporal_disabled_or_unavailable");
		R_FogVol_BindFramebuffer (GL_FRAMEBUFFER, framebufs.composite.fbo);
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
		glViewport (glx, gly, glwidth, glheight);
		if (use_halfres)
		{
			if (composite_src_tex)
			{
				final_tex = composite_src_tex;
				final_fbo = composite_src_fbo;
			}
			if (r_fogvol_upsample.value > 0.f && glprogs.fogvol_upsample)
			{
				int taps = (int)Q_rint (r_fogvol_upsample_taps.value);
				taps = (taps == 9) ? 9 : 4;
				R_FogVol_UseProgram (glprogs.fogvol_upsample);
				GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
				R_FogVol_AssertNoFeedbackHazard ("HISTORY", framebufs.composite.color_tex, final_tex);
				GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, final_tex);
				GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, depth_tex);
				R_FogVol_LogHazardPass ("HISTORY", final_tex, 0, final_tex);
				GL_Uniform4fFunc (0, (float)glwidth, (float)glheight, (float)fog_width, (float)fog_height);
				GL_Uniform1fFunc (1, r_fogvol_upsample_k.value);
				GL_Uniform1iFunc (2, taps);
				glDrawArrays (GL_TRIANGLES, 0, 3);
			}
			else
			{
				R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, final_fbo);
				R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
				R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
				R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
				R_FogVol_LogHazardPass ("HISTORY", final_tex, 0, final_tex);
				R_FogVol_AssertNoFeedbackHazard ("HISTORY", framebufs.composite.color_tex, final_tex);
				GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
					0, 0, glwidth, glheight,
					GL_COLOR_BUFFER_BIT, GL_LINEAR);
			}
		}
		else
		{
			R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, final_fbo);
			R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
			R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
			R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
			R_FogVol_LogHazardPass ("HISTORY", final_tex, 0, final_tex);
			R_FogVol_AssertNoFeedbackHazard ("HISTORY", framebufs.composite.color_tex, final_tex);
			GL_BlitFramebufferFunc (0, 0, glwidth, glheight,
				0, 0, glwidth, glheight,
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
	}

	if (mode == 1)
		R_DebugFlushGeometry ();

done:
	R_FogVol_LogPipelineState ("FOGVOL_END");
	if (use_test_guard)
	{
		R_FogVol_Restore3DRenderState (&restore_state);
		if (dumpstate_always)
		{
			fogvol_test_state_t restored_state;
			R_FogVol_TestState_Capture (&restored_state);
			R_FogVol_TestState_Log ("restored-baseline", &restored_state);
		}
	}

	GL_EndGroup ();
}


void R_FogVol_LogEndFrameState (void)
{
	R_FogVol_LogPipelineState ("END_FRAME");
}

int R_FogVol_BindForFroxelBuild (void)
{
	R_FogVol_BuildList ();
	if (r_fogvol.value <= 0.f)
		return 0;
	if (r_fogvolume_count <= 0)
		return 0;
	return r_fogvolume_count;
}

void R_FogVol_InjectBuiltIntoFroxel (void)
{
	froxel_grid_t grid;
	if (r_fogvolume_count <= 0)
		return;
	grid.width = framebufs.atmos_froxel.width;
	grid.height = framebufs.atmos_froxel.height;
	grid.depth = framebufs.atmos_froxel.depth;
	grid.scatter_tex = framebufs.atmos_froxel.scatter_tex;
	grid.transmittance_tex = framebufs.atmos_froxel.transmittance_tex;
	R_FogVol_InjectIntoGrid (&grid, r_fogvolumes, r_fogvolume_count);
}

void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num)
{
	int gx, gy, gz;

	if (!grid || !vols || num <= 0)
		return;
	if (grid->scatter_tex == 0 || grid->transmittance_tex == 0)
		return;
	if (grid->width <= 0 || grid->height <= 0 || grid->depth <= 0)
		return;
	if (!glprogs.atmos_froxel_build)
		return;

	gx = (grid->width + 3) / 4;
	gy = (grid->height + 3) / 4;
	gz = (grid->depth + 3) / 4;

	GL_BeginGroup ("Atmosphere: FogVol Inject");
	GL_UseProgram (glprogs.atmos_froxel_build);
	GL_BindImageTextureFunc (0, grid->scatter_tex, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
	GL_BindImageTextureFunc (1, grid->transmittance_tex, 0, GL_TRUE, 0, GL_READ_WRITE, GL_R16F);
	GL_Uniform4fFunc (0, (float)grid->width, (float)grid->height, (float)grid->depth, 0.f);

	for (int i = 0; i < num; ++i)
	{
		const fog_volume_t *v = &vols[i];
		float albedo = CLAMP (0.f, v->noiseAmount, 1.f);
		float emissive = q_max (0.f, v->noiseBias);
		float intensity = q_max (0.f, v->density);

		if (!v->enabled || v->density <= 0.f)
			continue;

		GL_Uniform4fFunc (1, q_max (0.f, v->density), q_max (0.f, v->falloff), albedo, emissive);
		GL_Uniform4fFunc (2, CLAMP (0.f, v->color[0], 4.f), CLAMP (0.f, v->color[1], 4.f), CLAMP (0.f, v->color[2], 4.f), intensity);
		GL_Uniform4fFunc (3, 0.f, 0.f, 1.f, 0.f);
		GL_Uniform4fFunc (4, v->mins[0], v->mins[1], v->mins[2], 1.f);
		GL_Uniform4fFunc (5, v->maxs[0], v->maxs[1], v->maxs[2], 0.f);
		GL_DispatchComputeFunc (gx, gy, gz);
	}

	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
	GL_EndGroup ();
}
