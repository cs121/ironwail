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
#include "gl_lightgrid.h"
#include "r_fogvol.h"
#include "r_dlight_pool.h"
#include <math.h>
#include <stdint.h>

typedef struct fog_volume_gpu_s
{
	float mins[4];
	float maxs[4];
	float sphere[4];
	float color_density[4];
	float noise_params[4];
	float velocity_windspeed[4];
	float wind_turbulence[4];
	float misc[4];
	float extra[4];
} fog_volume_gpu_t;

COMPILE_TIME_ASSERT (fog_volume_gpu_align16, (sizeof (fog_volume_gpu_t) % 16) == 0);

typedef struct fog_light_gpu_s
{
	float pos_rad[4];
	float col_int[4];
} fog_light_gpu_t;

typedef struct fog_light_list_gpu_s
{
	int offset_count[4];
} fog_light_list_gpu_t;

typedef struct fog_light_lists_gpu_s
{
	fog_light_list_gpu_t volumes[MAX_FOGVOLUMES];
	fog_light_gpu_t lights[MAX_FOGVOLUMES * MAX_FOGLIGHTS];
} fog_light_lists_gpu_t;

typedef struct fog_lightgrid_gpu_s
{
	float probe_rgb[8][4];
} fog_lightgrid_gpu_t;

COMPILE_TIME_ASSERT (fog_light_gpu_align16, (sizeof (fog_light_gpu_t) % 16) == 0);
COMPILE_TIME_ASSERT (fog_light_list_gpu_align16, (sizeof (fog_light_list_gpu_t) % 16) == 0);
COMPILE_TIME_ASSERT (fog_light_lists_gpu_align16, (sizeof (fog_light_lists_gpu_t) % 16) == 0);
COMPILE_TIME_ASSERT (fog_lightgrid_gpu_align16, (sizeof (fog_lightgrid_gpu_t) % 16) == 0);

typedef struct fogvol_light_candidate_s
{
	int index;
	float score;
	fog_light_gpu_t light;
	vec3_t bounds_mins;
	vec3_t bounds_maxs;
} fogvol_light_candidate_t;

typedef struct fogvol_visible_volume_s
{
	const fog_volume_t *volume;
	float screen_weight;
} fogvol_visible_volume_t;

typedef struct fogvol_light_stats_s
{
	int broadphase_candidates;
	int narrowphase_candidates;
	int narrowphase_accepted;
	double broadphase_seconds;
	double narrowphase_seconds;
} fogvol_light_stats_t;

static fog_volume_t r_fogvolumes[MAX_FOGVOLUMES];
static int r_fogvolume_count = 0;
static fog_volume_t r_fogvolume_entities[MAX_FOGVOLUMES];
static int r_fogvolume_entity_count = 0;

#define MAX_FOG_DLIGHTS 64
#define FOG_TORCH_CACHE_SIZE 128

typedef struct fog_torch_cache_entry_s
{
	const qmodel_t *model;
	qboolean is_torch;
} fog_torch_cache_entry_t;

/* Variant C split: these lights are fog-only and must never enter the normal
 * geometry/cluster/shadow dlight pipelines. */
static int r_num_fog_dlights = 0;
static dlight_t r_fog_dlights[MAX_FOG_DLIGHTS];
static fog_torch_cache_entry_t r_fog_torch_cache[FOG_TORCH_CACHE_SIZE];

cvar_t r_fogvol = { "r_fogvol", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_steps = { "r_fogvol_steps", "32", CVAR_ARCHIVE };
cvar_t r_fogvol_maxsteps = { "r_fogvol_maxsteps", "128", CVAR_ARCHIVE };
cvar_t r_fogvol_stepsize = { "r_fogvol_stepsize", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_halfres = { "r_fogvol_halfres", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_upsample = { "r_fogvol_upsample", "1", CVAR_ARCHIVE };
/* r_fogvol_upsample_k: bilateral weight scale for depth difference in the
 * upsample pass.  exp(-|delta_depth| * K) — higher K = sharper depth edges.
 * Default changed from 100 to 25: value 100 produced extremely harsh depth
 * discontinuities at fog/geometry edges visible as a 1-pixel black fringe.
 * 25 gives well-defined edges while preserving smooth gradients in open areas.
 * Recommended range: 10–50. */
cvar_t r_fogvol_upsample_k = { "r_fogvol_upsample_k", "25", CVAR_ARCHIVE };
cvar_t r_fogvol_upsample_taps = { "r_fogvol_upsample_taps", "4", CVAR_ARCHIVE };
cvar_t r_fogvol_steps_scale_halfres = { "r_fogvol_steps_scale_halfres", "0.5", CVAR_ARCHIVE };
cvar_t r_fogvol_noise = { "r_fogvol_noise", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_subsample = { "r_fogvol_noise_subsample", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_lod_switch_dist = { "r_fogvol_noise_lod_switch_dist", "64", CVAR_ARCHIVE };
cvar_t r_fogvol_domainwarp_dist = { "r_fogvol_domainwarp_dist", "128", CVAR_ARCHIVE };
cvar_t r_fogvol_noisemode = { "r_fogvol_noisemode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes = { "r_fogvol_testvolumes", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes_dumpstate = { "r_fogvol_testvolumes_dumpstate", "0", CVAR_NONE };
cvar_t r_fogvol_physblend = { "r_fogvol_physblend", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_blendmode = { "r_fogvol_blendmode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_emissive = { "r_fogvol_emissive", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_alpha = { "r_fogvol_temporal_alpha", "0.9", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_depth_reject = { "r_fogvol_temporal_depth_reject", "0.01", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_confidence_min_alpha = { "r_fogvol_temporal_confidence_min_alpha", "0.05", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_disocclusion_bias = { "r_fogvol_temporal_disocclusion_bias", "0.5", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_clamp_strength = { "r_fogvol_temporal_clamp_strength", "1.5", CVAR_ARCHIVE };
cvar_t r_fogvol_checkerboard = { "r_fogvol_checkerboard", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_light_subsample = { "r_fogvol_light_subsample", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_jitter = { "r_fogvol_jitter", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_debug = { "r_fogvol_debug", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_density_scale = { "r_fogvol_density_scale", "1", CVAR_ARCHIVE };
/* r_fogvol_sigma_max: maximum optical depth *per raymarching step*.
 * exp(-4) ≈ 0.018, so 4.0 is already near-black in one step.
 * Previous default of "2" was applied as a per-unit extinction cap, which
 * saturated instantly at any stepLen > ~1 unit.  Now treated as an optical
 * depth cap (sigma*stepLen), making the value scale-independent. */
cvar_t r_fogvol_sigma_max = { "r_fogvol_sigma_max", "4", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog = { "r_fogvol_globalfog", "1", CVAR_ARCHIVE };
/* r_fogvol_globalfog_density_scale: scales the density taken from the map's
 * classic Quake fog command before passing it to the volumetric raymarcher.
 * Quake fog density is dimensionless (used in GL_EXP as exp(-density*z));
 * the raymarcher treats density as an extinction coefficient in quake-units^-1.
 * A map fog density of 0.1 means GL_EXP gives 37% transmittance at z=10 units,
 * which is far too dense for the raymarcher (sigma=0.1, stepLen~64 → opaque).
 * Scale down by ~0.01–0.1 to get visually comparable results.
 * Default changed from 1 to 0.05 so global fog is not white-out by default. */
cvar_t r_fogvol_globalfog_density_scale = { "r_fogvol_globalfog_density_scale", "0.05", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_falloff = { "r_fogvol_globalfog_falloff", "64", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_noise_scale = { "r_fogvol_globalfog_noise_scale", "0.02", CVAR_ARCHIVE };
/* r_fogvol_globalfog_noise_amount: 0=uniform fog, 1=fully noise-modulated.
 * Was 0 (disabled) by default, making global fog always uniform regardless of
 * other noise settings. Changed to 0.4 for visible clumping out of the box. */
cvar_t r_fogvol_globalfog_noise_amount = { "r_fogvol_globalfog_noise_amount", "0.4", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_noise_bias = { "r_fogvol_globalfog_noise_bias", "0.0", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_velocity_x = { "r_fogvol_globalfog_velocity_x", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_velocity_y = { "r_fogvol_globalfog_velocity_y", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_velocity_z = { "r_fogvol_globalfog_velocity_z", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_height = { "r_fogvol_globalfog_height", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_height_scale = { "r_fogvol_globalfog_height_scale", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_globalfog_priority = { "r_fogvol_globalfog_priority", "-1", CVAR_ARCHIVE };
cvar_t r_fogvol_light = { "r_fogvol_light", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_lightgrid = { "r_fogvol_lightgrid", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_light_max = { "r_fogvol_light_max", "16", CVAR_ARCHIVE };
cvar_t r_fogvol_light_stats = { "r_fogvol_light_stats", "0", CVAR_NONE };
cvar_t r_fogvol_shadow = { "r_fogvol_shadow", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_samples = { "r_fogvol_shadow_samples", "2", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_strength = { "r_fogvol_shadow_strength", "0.8", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_jitter = { "r_fogvol_shadow_jitter", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_sun_dir = { "r_fogvol_sun_dir", "1", CVAR_ARCHIVE };

extern cvar_t gl_farclip;

static int r_fogvol_history_index = 0;
static int r_fogvol_history_width = 0;
static int r_fogvol_history_height = 0;
static qboolean r_fogvol_composite_valid = false;


void R_FogVol_ClearHistory (void)
{
	r_fogvol_history_index = 0;
	r_fogvol_history_width = 0;
	r_fogvol_history_height = 0;
	r_fogvol_composite_valid = false;

	if (!framebufs.fogvol.history_fbo[0] || !framebufs.fogvol.history_fbo[1])
		return;

	/* BUG FIX (C-04): Save the previously bound draw FBO so we can restore it
	 * after clearing.  R_FogVol_ClearHistory() may be called during map loading
	 * or from contexts where a different FBO is active — unconditionally leaving
	 * composite.fbo bound corrupts the GL state for the caller. */
	{
		const float zero[4] = {0.f, 0.f, 0.f, 0.f};
		GLint prev_fbo = 0;
		glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.fogvol.history_fbo[0]);
		GL_ClearBufferfvFunc (GL_COLOR, 0, zero);
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.fogvol.history_fbo[1]);
		GL_ClearBufferfvFunc (GL_COLOR, 0, zero);

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, (GLuint)prev_fbo);
	}
}

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
	GLint active_texture;
	unsigned int glstate_bits;
	GLboolean scissor_test;
	GLboolean framebuffer_srgb;
} fogvol_restore_state_t;

static fogvol_state_cache_t r_fogvol_state_cache = { 0, 0, 0, 0 };

static void R_FogVol_BindFramebuffer (GLenum target, GLuint fbo)
{
	GL_BindFramebufferFunc (target, fbo);
	/* BEST PRACTICE #10: GL_FRAMEBUFFER is an alias that binds both draw and
	 * read targets simultaneously (GL 3.0+).  Reflect that in the cache so
	 * that subsequent GL_DRAW/READ_FRAMEBUFFER queries against the cache are
	 * correct even if they weren't set individually. */
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

static qboolean R_FogVol_StateDebugEnabled (void)
{
	return r_fogvol_debug.value > 0.f || developer.value > 0.f;
}

static void R_FogVol_LogStateSnapshot (const char *marker)
{
	GLint viewport[4] = {0};
	GLint scissor_box[4] = {0};
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;
	GLint program = 0;
	GLint active_texture = 0;
	GLboolean scissor_enabled = GL_FALSE;

	if (!R_FogVol_StateDebugEnabled ())
		return;

	glGetIntegerv (GL_VIEWPORT, viewport);
	scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &active_texture);

	Con_DPrintf (
		"FOGVOL_STATE %s draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) scissor=%d box=(%d %d %d %d) drawbuf=0x%04x readbuf=0x%04x prog=%d active_tex=%d\n",
		marker,
		draw_fbo, read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_enabled,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		(unsigned)draw_buffer, (unsigned)read_buffer,
		program, active_texture - GL_TEXTURE0);
}

static void R_FogVol_LogBufferMarker (const char *marker)
{
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;

	if (!R_FogVol_TestDebugEnabled ())
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);
	Con_Printf ("FOGVOL_TEST marker=%s draw_fbo=%d read_fbo=%d draw_buffer=0x%04x read_buffer=0x%04x\n",
		marker, draw_fbo, read_fbo, (unsigned)draw_buffer, (unsigned)read_buffer);
}

static void R_FogVol_SetDrawBufferDebug (GLenum buf, const char *marker)
{
	glDrawBuffer (buf);
	R_FogVol_LogBufferMarker (marker);
}

static void R_FogVol_SetReadBufferDebug (GLenum buf, const char *marker)
{
	glReadBuffer (buf);
	R_FogVol_LogBufferMarker (marker);
}

static void R_FogVol_LogPipelineState (const char *marker)
{
	GLint viewport[4] = {0};
	GLint scissor_box[4] = {0};
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;
	GLint program = 0;
	GLboolean scissor_enabled = GL_FALSE;

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

	Con_Printf (
		"FOGVOL_TEST marker=%s draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) "
		"scissor=%d scissor_box=(%d %d %d %d) prog=%d draw_buffer=0x%04x read_buffer=0x%04x\n",
		marker,
		draw_fbo,
		read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_enabled,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		program,
		(unsigned)draw_buffer,
		(unsigned)read_buffer);
}

static void R_FogDlights_Clear (void)
{
	r_num_fog_dlights = 0;
}

static void R_FogDlights_Add (const vec3_t origin, float radius, const vec3_t color)
{
	dlight_t *dl;

	if (r_num_fog_dlights >= MAX_FOG_DLIGHTS)
		return;
	if (radius <= 0.f)
		return;

	dl = &r_fog_dlights[r_num_fog_dlights++];
	memset (dl, 0, sizeof (*dl));
	VectorCopy (origin, dl->origin);
	dl->radius = q_max (0.f, radius);
	dl->color[0] = CLAMP (0.f, color[0], 1.f);
	dl->color[1] = CLAMP (0.f, color[1], 1.f);
	dl->color[2] = CLAMP (0.f, color[2], 1.f);
	dl->active = true;
}

static qboolean R_IsTorchName (const char *name)
{
	return name
		&& (q_strcasestr (name, "torch")
			|| q_strcasestr (name, "flame")
			|| q_strcasestr (name, "fire")
			|| q_strcasestr (name, "burn")
			|| q_strcasestr (name, "lantern"));
}

static qboolean R_IsTorchModel (const qmodel_t *model)
{
	const uintptr_t key = (uintptr_t)model;
	int slot;

	if (!model)
		return false;

	slot = (int)(key % FOG_TORCH_CACHE_SIZE);
	if (r_fog_torch_cache[slot].model != model)
	{
		r_fog_torch_cache[slot].model = model;
		r_fog_torch_cache[slot].is_torch = R_IsTorchName (model->name);
	}
	return r_fog_torch_cache[slot].is_torch;
}

static void R_FogDlights_AddTorchEntities (void)
{
	const vec3_t torch_color = {1.f, 0.55f, 0.2f};

	for (int i = 0; i < cl_numvisedicts; ++i)
	{
		const entity_t *ent = cl_visedicts[i];
		float flicker_a;
		float flicker_b;
		float flicker;
		float radius;

		if (!ent || !ent->model || !R_IsTorchModel (ent->model))
			continue;

		flicker_a = 0.5f + 0.5f * sinf ((float)cl.time * 7.0f + (float)i * 12.9898f);
		flicker_b = 0.5f + 0.5f * sinf ((float)cl.time * 13.0f + (float)i * 3.1f + 1.7f);
		flicker = CLAMP (0.f, 0.7f * flicker_a + 0.3f * flicker_b, 1.f);
		radius = 96.f * (0.85f + 0.15f * flicker);
		R_FogDlights_Add (ent->origin, radius, torch_color);
	}
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

	if (!R_FogVol_TestDebugEnabled ())
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

static void R_FogVol_TestState_Capture (fogvol_test_state_t *state)
{
	GLenum draw_color_attachment = GL_BACK_LEFT;
	GLenum read_color_attachment = GL_BACK_LEFT;

	state->statebits = glstate;
	/* BEST PRACTICE #9: cache_statebits mirrors glstate at capture time.
	 * Both fields currently hold identical values; if the engine ever
	 * separates its internal cache bitfield from the applied bitfield,
	 * record the cache-side value here independently. */
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


static void R_FogVol_TestState_Log (const char *phase, const fogvol_test_state_t *state)
{
	Con_Printf (
		"FOGVOL_TEST %s viewport=(%d %d %d %d) scissor_test=%d scissor_box=(%d %d %d %d) "
		"draw_fbo=%d read_fbo=%d draw_buffer=0x%04x read_buffer=0x%04x "
		"draw_buffers=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x] "
		"draw_status=0x%04x read_status=0x%04x "
		"draw_att0=(type=0x%04x,name=%d) draw_att1=(type=0x%04x,name=%d) draw_depth=(type=0x%04x,name=%d) "
		"read_att0=(type=0x%04x,name=%d) read_depth=(type=0x%04x,name=%d) "
		"prog=%d vao=%d blend=%d blend_func=(%d,%d) blend_eq=%d "
		"depth_test=%d depth_writemask=%d color_writemask=(%d %d %d %d) cull=%d poly=(%d,%d) "
		"clear_color=(%.3f %.3f %.3f %.3f) srgb=%d dither=%d multisample=%d glstate=0x%08x "
		"cache(glstate=0x%08x draw_fbo=%d read_fbo=%d prog=%d vao=%d)\n",
		phase,
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
	glGetIntegerv (GL_ACTIVE_TEXTURE, &state->active_texture);
	state->glstate_bits = glstate;
	state->framebuffer_srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
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
	GL_ActiveTextureFunc ((GLenum)state->active_texture);

	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	R_FogVol_SetDepthMask (true);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_SetState (state->glstate_bits);
	if (state->framebuffer_srgb)
		glEnable (GL_FRAMEBUFFER_SRGB);
	else
		glDisable (GL_FRAMEBUFFER_SRGB);
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

static int R_FogVol_CompareLightCandidate (const void *a, const void *b)
{
	const fogvol_light_candidate_t *la = (const fogvol_light_candidate_t *)a;
	const fogvol_light_candidate_t *lb = (const fogvol_light_candidate_t *)b;

	if (la->score > lb->score)
		return -1;
	if (la->score < lb->score)
		return 1;
	if (la->index < lb->index)
		return -1;
	if (la->index > lb->index)
		return 1;
	return 0;
}

static float R_FogVol_DistanceToVolume (const fog_volume_t *volume, const vec3_t p)
{
	if (!volume)
		return 999999.f;

	if (volume->shape > 0)
	{
		vec3_t d;
		VectorSubtract (p, volume->sphereCenter, d);
		return q_max (0.f, VectorLength (d) - q_max (0.f, volume->sphereRadius));
	}

	{
		float dx = q_max (q_max (volume->mins[0] - p[0], 0.f), p[0] - volume->maxs[0]);
		float dy = q_max (q_max (volume->mins[1] - p[1], 0.f), p[1] - volume->maxs[1]);
		float dz = q_max (q_max (volume->mins[2] - p[2], 0.f), p[2] - volume->maxs[2]);
		return sqrtf (dx * dx + dy * dy + dz * dz);
	}
}

static qboolean R_FogVol_LightOverlapsVolume (const fog_volume_t *volume, const vec3_t light_origin, float light_radius)
{
	float dist;

	if (!volume || light_radius <= 0.f)
		return false;

	dist = R_FogVol_DistanceToVolume (volume, light_origin);
	return dist <= light_radius;
}

static void R_FogVol_GetVolumeBounds (const fog_volume_t *vol, vec3_t bmins, vec3_t bmaxs)
{
	if (vol->shape == 1)
	{
		for (int a = 0; a < 3; ++a)
		{
			bmins[a] = vol->sphereCenter[a] - vol->sphereRadius;
			bmaxs[a] = vol->sphereCenter[a] + vol->sphereRadius;
		}
	}
	else
	{
		VectorCopy (vol->mins, bmins);
		VectorCopy (vol->maxs, bmaxs);
	}
}

static qboolean R_FogVol_LightOverlapsBounds (const vec3_t light_origin, float light_radius, const vec3_t bmins, const vec3_t bmaxs)
{
	float dist2 = 0.f;

	for (int a = 0; a < 3; ++a)
	{
		if (light_origin[a] < bmins[a])
		{
			float d = bmins[a] - light_origin[a];
			dist2 += d * d;
		}
		else if (light_origin[a] > bmaxs[a])
		{
			float d = light_origin[a] - bmaxs[a];
			dist2 += d * d;
		}
	}

	return dist2 <= light_radius * light_radius;
}

static qboolean R_FogVol_BoundsOverlap (const vec3_t mins0, const vec3_t maxs0, const vec3_t mins1, const vec3_t maxs1)
{
	for (int a = 0; a < 3; ++a)
	{
		if (maxs0[a] < mins1[a] || mins0[a] > maxs1[a])
			return false;
	}
	return true;
}

static int R_FogVol_BuildVisibleVolumeCache (fogvol_visible_volume_t *visible, int max_visible, vec3_t fog_bounds_mins, vec3_t fog_bounds_maxs, qboolean *has_fog_bounds)
{
	int visible_count = 0;

	*has_fog_bounds = false;
	VectorSet (fog_bounds_mins, 0.f, 0.f, 0.f);
	VectorSet (fog_bounds_maxs, 0.f, 0.f, 0.f);

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		const fog_volume_t *vol = &r_fogvolumes[i];
		vec3_t bmins, bmaxs;
		int sx0, sy0, sx1, sy1;

		R_FogVol_GetVolumeBounds (vol, bmins, bmaxs);

		if (!*has_fog_bounds)
		{
			VectorCopy (bmins, fog_bounds_mins);
			VectorCopy (bmaxs, fog_bounds_maxs);
			*has_fog_bounds = true;
		}
		else
		{
			for (int a = 0; a < 3; ++a)
			{
				fog_bounds_mins[a] = q_min (fog_bounds_mins[a], bmins[a]);
				fog_bounds_maxs[a] = q_max (fog_bounds_maxs[a], bmaxs[a]);
			}
		}

		if (visible_count >= max_visible)
			continue;

		if (R_FogVol_ProjectAABBToScreenRect (vol, &sx0, &sy0, &sx1, &sy1, true))
		{
			const float view_area = (float)(q_max (1, r_refdef.vrect.width) * q_max (1, r_refdef.vrect.height));
			const float rect_area = (float)q_max (1, (sx1 - sx0) * (sy1 - sy0));
			visible[visible_count].volume = vol;
			visible[visible_count].screen_weight = CLAMP (0.1f, rect_area / view_area, 1.f);
			visible_count++;
		}
	}

	return visible_count;
}

static int R_FogVol_BuildFrameLightBroadphase (const fogvol_visible_volume_t *visible_volumes, int visible_count, const vec3_t fog_bounds_mins, const vec3_t fog_bounds_maxs, qboolean has_fog_bounds, fogvol_light_candidate_t *out, int max_out)
{
	const dlight_t *const *active;
	int active_count = 0;
	int candidate_count = 0;

	if (!out || max_out <= 0 || visible_count <= 0)
		return 0;

	active = DLightPool_GetActiveList (&active_count);
	if (active && active_count > 0)
	{
		for (int i = 0; i < active_count && candidate_count < max_out; ++i)
		{
			const dlight_t *dl = active[i];
			vec3_t to_light;
			float overlap_score = 0.f;
			float score;
			float intensity;
			float radius;

			if (!dl || !CL_DlightIsActive (dl))
				continue;

			radius = dl->radius;
			if (radius <= 0.f)
				continue;

			intensity = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));
			if (intensity <= 0.f)
				continue;

			if (active_count > 192 && has_fog_bounds)
			{
				if (!R_FogVol_LightOverlapsBounds (dl->origin, radius * 4.f, fog_bounds_mins, fog_bounds_maxs))
					continue;
			}

			for (int v = 0; v < visible_count; ++v)
			{
				if (!R_FogVol_LightOverlapsVolume (visible_volumes[v].volume, dl->origin, radius))
					continue;
				overlap_score += visible_volumes[v].screen_weight;
			}

			if (overlap_score <= 0.f)
				continue;

			VectorSubtract (dl->origin, r_refdef.vieworg, to_light);
			score = overlap_score * intensity * radius;
			score *= 1.f / (1.f + 0.0025f * sqrtf (DotProduct (to_light, to_light)));
			if (score <= 0.f)
				continue;

			out[candidate_count].index = (dl->id != 0) ? dl->id : i;
			out[candidate_count].score = score;
			out[candidate_count].light.pos_rad[0] = dl->origin[0];
			out[candidate_count].light.pos_rad[1] = dl->origin[1];
			out[candidate_count].light.pos_rad[2] = dl->origin[2];
			out[candidate_count].light.pos_rad[3] = radius;
			out[candidate_count].light.col_int[0] = dl->color[0];
			out[candidate_count].light.col_int[1] = dl->color[1];
			out[candidate_count].light.col_int[2] = dl->color[2];
			out[candidate_count].light.col_int[3] = 1.f;
			out[candidate_count].bounds_mins[0] = dl->origin[0] - radius;
			out[candidate_count].bounds_mins[1] = dl->origin[1] - radius;
			out[candidate_count].bounds_mins[2] = dl->origin[2] - radius;
			out[candidate_count].bounds_maxs[0] = dl->origin[0] + radius;
			out[candidate_count].bounds_maxs[1] = dl->origin[1] + radius;
			out[candidate_count].bounds_maxs[2] = dl->origin[2] + radius;
			candidate_count++;
		}
	}

	for (int i = 0; i < r_num_fog_dlights && candidate_count < max_out; ++i)
	{
		const dlight_t *dl = &r_fog_dlights[i];
		float overlap_score = 0.f;

		if (!CL_DlightIsActive (dl) || dl->radius <= 0.f)
			continue;

		for (int v = 0; v < visible_count; ++v)
		{
			if (!R_FogVol_LightOverlapsVolume (visible_volumes[v].volume, dl->origin, dl->radius))
				continue;
			overlap_score += visible_volumes[v].screen_weight;
		}

		if (overlap_score <= 0.f)
			continue;

		out[candidate_count].index = 100000 + i;
		out[candidate_count].score = overlap_score * dl->radius;
		out[candidate_count].light.pos_rad[0] = dl->origin[0];
		out[candidate_count].light.pos_rad[1] = dl->origin[1];
		out[candidate_count].light.pos_rad[2] = dl->origin[2];
		out[candidate_count].light.pos_rad[3] = dl->radius;
		out[candidate_count].light.col_int[0] = dl->color[0];
		out[candidate_count].light.col_int[1] = dl->color[1];
		out[candidate_count].light.col_int[2] = dl->color[2];
		out[candidate_count].light.col_int[3] = 1.f;
		out[candidate_count].bounds_mins[0] = dl->origin[0] - dl->radius;
		out[candidate_count].bounds_mins[1] = dl->origin[1] - dl->radius;
		out[candidate_count].bounds_mins[2] = dl->origin[2] - dl->radius;
		out[candidate_count].bounds_maxs[0] = dl->origin[0] + dl->radius;
		out[candidate_count].bounds_maxs[1] = dl->origin[1] + dl->radius;
		out[candidate_count].bounds_maxs[2] = dl->origin[2] + dl->radius;
		candidate_count++;
	}

	if (candidate_count > 1)
		qsort (out, candidate_count, sizeof (out[0]), R_FogVol_CompareLightCandidate);

	return candidate_count;
}

static int R_FogVol_BuildLightListForVolume (const fog_volume_t *volume, const fogvol_light_candidate_t *prefiltered, int prefiltered_count, fog_light_gpu_t *out, int max_count, fogvol_light_stats_t *stats)
{
	vec3_t volume_mins;
	vec3_t volume_maxs;
	int copy_count = 0;

	if (!volume || !out || max_count <= 0)
		return 0;
	if (!prefiltered || prefiltered_count <= 0)
		return 0;

	R_FogVol_GetVolumeBounds (volume, volume_mins, volume_maxs);
	for (int i = 0; i < prefiltered_count && copy_count < max_count; ++i)
	{
		const fogvol_light_candidate_t *candidate = &prefiltered[i];
		if (stats)
			stats->narrowphase_candidates++;

		if (!R_FogVol_BoundsOverlap (candidate->bounds_mins, candidate->bounds_maxs, volume_mins, volume_maxs))
			continue;

		out[copy_count++] = candidate->light;
		if (stats)
			stats->narrowphase_accepted++;
	}

	return copy_count;
}

void R_FogVol_Init (void)
{
	Cvar_RegisterVariable (&r_fogvol);
	Cvar_RegisterVariable (&r_fogvol_steps);
	Cvar_RegisterVariable (&r_fogvol_maxsteps);
	Cvar_RegisterVariable (&r_fogvol_stepsize);
	Cvar_RegisterVariable (&r_fogvol_halfres);
	Cvar_RegisterVariable (&r_fogvol_upsample);
	Cvar_RegisterVariable (&r_fogvol_upsample_k);
	Cvar_RegisterVariable (&r_fogvol_upsample_taps);
	Cvar_RegisterVariable (&r_fogvol_steps_scale_halfres);
	Cvar_RegisterVariable (&r_fogvol_noise);
	Cvar_RegisterVariable (&r_fogvol_noise_subsample);
	Cvar_RegisterVariable (&r_fogvol_noise_lod_switch_dist);
	Cvar_RegisterVariable (&r_fogvol_domainwarp_dist);
	Cvar_RegisterVariable (&r_fogvol_noisemode);
	Cvar_RegisterVariable (&r_fogvol_testvolumes);
	Cvar_RegisterVariable (&r_fogvol_testvolumes_dumpstate);
	Cvar_RegisterVariable (&r_fogvol_physblend);
	Cvar_RegisterVariable (&r_fogvol_blendmode);
	Cvar_RegisterVariable (&r_fogvol_emissive);
	Cvar_RegisterVariable (&r_fogvol_temporal_alpha);
	Cvar_RegisterVariable (&r_fogvol_temporal_depth_reject);
	Cvar_RegisterVariable (&r_fogvol_temporal_confidence_min_alpha);
	Cvar_RegisterVariable (&r_fogvol_temporal_disocclusion_bias);
	Cvar_RegisterVariable (&r_fogvol_temporal_clamp_strength);
	Cvar_RegisterVariable (&r_fogvol_checkerboard);
	Cvar_RegisterVariable (&r_fogvol_light_subsample);
	Cvar_RegisterVariable (&r_fogvol_jitter);
	Cvar_RegisterVariable (&r_fogvol_debug);
	Cvar_RegisterVariable (&r_fogvol_density_scale);
	Cvar_RegisterVariable (&r_fogvol_sigma_max);
	Cvar_RegisterVariable (&r_fogvol_globalfog);
	Cvar_RegisterVariable (&r_fogvol_globalfog_density_scale);
	Cvar_RegisterVariable (&r_fogvol_globalfog_falloff);
	Cvar_RegisterVariable (&r_fogvol_globalfog_noise_scale);
	Cvar_RegisterVariable (&r_fogvol_globalfog_noise_amount);
	Cvar_RegisterVariable (&r_fogvol_globalfog_noise_bias);
	Cvar_RegisterVariable (&r_fogvol_globalfog_velocity_x);
	Cvar_RegisterVariable (&r_fogvol_globalfog_velocity_y);
	Cvar_RegisterVariable (&r_fogvol_globalfog_velocity_z);
	Cvar_RegisterVariable (&r_fogvol_globalfog_height);
	Cvar_RegisterVariable (&r_fogvol_globalfog_height_scale);
	Cvar_RegisterVariable (&r_fogvol_globalfog_priority);
	Cvar_RegisterVariable (&r_fogvol_light);
	Cvar_RegisterVariable (&r_fogvol_lightgrid);
	Cvar_RegisterVariable (&r_fogvol_light_max);
	Cvar_RegisterVariable (&r_fogvol_light_stats);
	Cvar_RegisterVariable (&r_fogvol_shadow);
	Cvar_RegisterVariable (&r_fogvol_shadow_samples);
	Cvar_RegisterVariable (&r_fogvol_shadow_strength);
	Cvar_RegisterVariable (&r_fogvol_shadow_jitter);
	Cvar_RegisterVariable (&r_fogvol_sun_dir);
}

void R_FogVol_Clear (void)
{
	r_fogvolume_count = 0;
}

static void R_FogVol_ClearEntities (void)
{
	r_fogvolume_entity_count = 0;
}

static void R_FogVol_ClampVolume (fog_volume_t *volume)
{
	volume->density = CLAMP (0.f, volume->density, 10.f);
	volume->falloff = CLAMP (0.f, volume->falloff, 256.f);
	volume->noiseAmount = CLAMP (0.f, volume->noiseAmount, 1.f);
	volume->noiseScale = CLAMP (0.001f, volume->noiseScale, 0.5f);
	volume->turbulence = CLAMP (0.f, volume->turbulence, 2.f);
	volume->emissiveStrength = CLAMP (0.f, volume->emissiveStrength, 16.f);
	volume->shape = CLAMP (0, volume->shape, 2);
	volume->blendMode = CLAMP (-1, volume->blendMode, 1);
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
		/* BUG FIX #7a: Threshold was > 2.f which wrongly keeps values in
		 * (1,2] as-is even though linear RGB is defined on [0,1].  Use > 1.f
		 * so that any channel exceeding the linear range triggers the 0-255
		 * normalisation path.
		 * BUG FIX #7b: If sscanf returns < 3 we fall through to the defaults
		 * (r=g=b=1), but the original code mixed partial results from sscanf
		 * with the uninitialised defaults in the caller's stack frame.
		 * The explicit == 3 guard above is correct; no change needed there. */
		if (r > 1.f || g > 1.f || b > 1.f)
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

static float R_FogVol_PointDistance (const vec3_t point, const fog_volume_t *volume)
{
	/* BUG FIX (C-07): Declare all locals at the top of the function so this
	 * code compiles cleanly under strict C89 (-std=c89 / /Za).  Mixed
	 * declarations after statements are a C99 extension that some Quake-engine
	 * toolchains reject. */
	float dist2 = 0.f;
	int i;

	if (volume->shape == 1)
	{
		vec3_t delta;
		float dist;
		VectorSubtract (point, volume->sphereCenter, delta);
		dist = VectorLength (delta) - volume->sphereRadius;
		return q_max (0.f, dist);
	}

	for (i = 0; i < 3; ++i)
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

static void R_FogVol_AddGlobalFog (void)
{
	fog_volume_t volume;
	float *color = Fog_GetColor ();

	if (r_fogvol_globalfog.value <= 0.f)
		return;
	if (r_fogvol.value <= 0.f)
		return;
	if (Fog_GetDensity () <= 0.f)
		return;

	memset (&volume, 0, sizeof (volume));

	VectorSet (volume.mins, -8192.f, -8192.f, -8192.f);
	VectorSet (volume.maxs, 8192.f, 8192.f, 8192.f);

	VectorCopy (color, volume.color);

	volume.density = Fog_GetDensity () * q_max (0.f, r_fogvol_globalfog_density_scale.value);
	volume.falloff = q_max (0.f, r_fogvol_globalfog_falloff.value);
	volume.mode = 0;
	volume.noiseScale = q_max (0.f, r_fogvol_globalfog_noise_scale.value);
	volume.noiseAmount = CLAMP (0.f, r_fogvol_globalfog_noise_amount.value, 1.f);
	volume.noiseBias = r_fogvol_globalfog_noise_bias.value;
	volume.velocity[0] = r_fogvol_globalfog_velocity_x.value;
	volume.velocity[1] = r_fogvol_globalfog_velocity_y.value;
	volume.velocity[2] = r_fogvol_globalfog_velocity_z.value;
	volume.maxDistance = 0.f;
	volume.priority = (int)Q_rint (r_fogvol_globalfog_priority.value);
	volume.enabled = 1;
	volume.height = r_fogvol_globalfog_height.value;
	volume.heightScale = r_fogvol_globalfog_height_scale.value;

	R_FogVol_AddVolume (&volume);
}

qboolean R_FogVol_CanRenderGlobal (void)
{
	if (Fog_GetDensity () <= 0.f)
		return false;
	if (!glprogs.fogvol)
		return false;
	if (framebufs.composite.color_tex == 0 || framebufs.fogvol.color_tex[0] == 0)
		return false;
	if (framebufs.composite.depth_stencil_tex == 0)
		return false;
	return true;
}

/* Returns the fogvol composite texture that was rendered this frame.
 * After R_FogVol_Render(), r_fogvol_history_index points to the slot that
 * received the temporal composite output — that is the most recent result.
 * Returns 0 if fogvol did not render this frame (no active volumes). */
GLuint R_FogVol_GetCompositeTex (void)
{
	if (!r_fogvol_composite_valid)
		return 0;

	GLuint tex = framebufs.fogvol.composite_tex[r_fogvol_history_index];
	return tex;
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
		volume.shape = 0;
		volume.blendMode = -1;
		volume.emissiveStrength = 0.f;
		volume.noiseScale = 0.05f;
		volume.noiseAmount = 0.5f;
		volume.noiseBias = 0.f;
		volume.turbulence = 0.f;
		VectorSet (volume.velocity, 0.f, 0.f, 0.f);
		volume.windSpeed = 0.f;
		VectorSet (volume.windDir, 0.f, 0.f, 0.f);
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
			/* BUG FIX #1: original used strlen(key) which is the length of the
			 * string *before* the shift, so the NUL terminator was correctly
			 * included only by accident when key length > 1.  For a key of
			 * exactly "_" (len=1) strlen returns 1, memmove copies 1 byte
			 * ("" with no NUL) leaving garbage after position 0.
			 * Use strlen(key)+1 to always include the NUL terminator. */
			if (key[0] == '_')
				memmove (key, key + 1, strlen (key) + 1);
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
			else if (!strcmp (key, "shape"))
			{
				if (!q_strcasecmp (value, "sphere") || atoi (value) == 1)
					volume.shape = 1;
				else
					volume.shape = 0;
			}
			else if (!strcmp (key, "radius"))
			{
				volume.sphereRadius = atof (value);
			}
			else if (!strcmp (key, "center"))
			{
				R_FogVol_ParseVector (value, volume.sphereCenter);
			}
			else if (!strcmp (key, "blendmode"))
			{
				volume.blendMode = atoi (value);
			}
			else if (!strcmp (key, "emissive"))
			{
				volume.emissiveStrength = atof (value);
			}
			else if (!strcmp (key, "wind_dir"))
			{
				R_FogVol_ParseVector (value, volume.windDir);
			}
			else if (!strcmp (key, "wind_speed"))
			{
				volume.windSpeed = atof (value);
			}
			else if (!strcmp (key, "turbulence"))
			{
				volume.turbulence = atof (value);
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
				if (volume.shape == 1)
				{
					for (int a = 0; a < 3; ++a)
						volume.sphereCenter[a] = 0.5f * (volume.mins[a] + volume.maxs[a]);
					if (volume.sphereRadius <= 0.f)
					{
						float ex = 0.5f * (volume.maxs[0] - volume.mins[0]);
						float ey = 0.5f * (volume.maxs[1] - volume.mins[1]);
						float ez = 0.5f * (volume.maxs[2] - volume.mins[2]);
						volume.sphereRadius = q_max (ex, q_max (ey, ez));
					}
				}
				R_FogVol_AddEntityVolume (&volume);
			}
		}

		data = COM_Parse (data);
	}
}

void R_FogVol_AddTestVolumes (void)
{
	fog_volume_t volume;
	vec3_t origin, forward, right, up;
	AngleVectors (r_refdef.viewangles, forward, right, up);
	VectorCopy (r_refdef.vieworg, origin);

	/* Single test volume: a fog patch placed 100 units ahead of the camera.
	 * The player is OUTSIDE the volume looking in, so this cleanly tests
	 * the compositing pipeline without camera-inside-volume complications.
	 *
	 * Expected result: a visible blue-grey fog patch floating in the scene.
	 * If the scene outside the volume looks correct and the volume itself
	 * shows fog colour, the pipeline is working.
	 *
	 * density=0.02 → tau≈2.0 at 100 units → transmittance≈0.14 (clearly
	 * visible, opaque-looking fog patch). */
	memset (&volume, 0, sizeof (volume));
	VectorSet (volume.color, 0.6f, 0.75f, 1.0f);
	volume.density    = 0.02f;
	volume.falloff    = 16.f;
	volume.mode       = 0;
	volume.shape      = 0;
	volume.blendMode  = -1;
	volume.noiseScale = 0.05f;
	volume.noiseAmount = 0.6f;
	volume.noiseBias  = 0.0f;
	VectorSet (volume.velocity, 0.f, 0.f, 4.f);
	volume.maxDistance = 0.f;
	volume.priority   = 0;
	volume.enabled    = 1;
	/* Place the box 100..260 units ahead, 96 units wide, 80 units tall.
	 * Compute proper axis-aligned bounds from the oriented corners. */
	{
		vec3_t c0, c1;
		VectorMA (origin, 100.f, forward, c0);
		VectorMA (origin, 260.f, forward, c1);
		/* half-width along right axis */
		for (int a = 0; a < 3; ++a)
		{
			float span = fabsf (right[a]) * 96.f;
			volume.mins[a] = q_min (c0[a], c1[a]) - span;
			volume.maxs[a] = q_max (c0[a], c1[a]) + span;
		}
		volume.mins[2] = origin[2] - 40.f;
		volume.maxs[2] = origin[2] + 80.f;
	}
	R_FogVol_AddVolume (&volume);
}

void R_FogVol_BuildList (void)
{
	R_FogVol_Clear ();
	R_FogDlights_Clear ();
	R_FogDlights_AddTorchEntities ();

	/* BUG FIX (testvolumes): r_fogvol_testvolumes must work independently of
	 * r_fogvol so developers can test the pipeline without setting up entity
	 * fog volumes.  Add test volumes first if requested, then check r_fogvol
	 * for the entity-volume path. */
	if (r_fogvol_testvolumes.value > 0.f)
		R_FogVol_AddTestVolumes ();

	if (r_fogvol.value <= 0.f)
		goto sort_and_done;

	R_FogVol_AddGlobalFog ();

	for (int i = 0; i < r_fogvolume_entity_count; ++i)
	{
		const fog_volume_t *volume = &r_fogvolume_entities[i];

		if (!volume->enabled)
			continue;
		if (volume->maxDistance > 0.f)
		{
			float dist = R_FogVol_PointDistance (r_refdef.vieworg, volume);
			if (dist > volume->maxDistance)
				continue;
		}
		R_FogVol_AddVolume (volume);
	}

sort_and_done:
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

	{
		vec3_t bmins, bmaxs;
		if (v->shape == 1)
		{
			for (int a = 0; a < 3; ++a)
			{
				bmins[a] = v->sphereCenter[a] - v->sphereRadius;
				bmaxs[a] = v->sphereCenter[a] + v->sphereRadius;
			}
		}
		else
		{
			VectorCopy (v->mins, bmins);
			VectorCopy (v->maxs, bmaxs);
		}

		/* BUG FIX (C-SCISSOR-01): When the camera is inside the fog volume,
		 * projecting the 8 AABB corners can produce a degenerate or zero-area
		 * scissor rect (all corners behind near-plane → behind==8 → return false,
		 * or clipped NDC rect that doesn't cover the screen centre).
		 * This caused the fog to disappear entirely when looking away from the
		 * volume's corners while standing inside it (Image 1 vs Image 2 bug).
		 *
		 * Fix: detect camera-inside early and return the full viewport immediately.
		 * For sphere volumes use a radius check; for AABB volumes use component-wise
		 * comparison against bmins/bmaxs (already computed above). */
		{
			qboolean cam_inside;
			if (v->shape == 1)
			{
				vec3_t d;
				VectorSubtract (r_refdef.vieworg, v->sphereCenter, d);
				cam_inside = (DotProduct (d, d) < v->sphereRadius * v->sphereRadius);
			}
			else
			{
				cam_inside = (r_refdef.vieworg[0] >= bmins[0] && r_refdef.vieworg[0] <= bmaxs[0] &&
				              r_refdef.vieworg[1] >= bmins[1] && r_refdef.vieworg[1] <= bmaxs[1] &&
				              r_refdef.vieworg[2] >= bmins[2] && r_refdef.vieworg[2] <= bmaxs[2]);
			}
			if (cam_inside)
			{
				*x0 = view_x;
				*y0 = view_y;
				*x1 = view_x + view_w;
				*y1 = view_y + view_h;
				return true;
			}
		}

		corners[0][0] = bmins[0]; corners[0][1] = bmins[1]; corners[0][2] = bmins[2];
		corners[1][0] = bmaxs[0]; corners[1][1] = bmins[1]; corners[1][2] = bmins[2];
		corners[2][0] = bmins[0]; corners[2][1] = bmaxs[1]; corners[2][2] = bmins[2];
		corners[3][0] = bmaxs[0]; corners[3][1] = bmaxs[1]; corners[3][2] = bmins[2];
		corners[4][0] = bmins[0]; corners[4][1] = bmins[1]; corners[4][2] = bmaxs[2];
		corners[5][0] = bmaxs[0]; corners[5][1] = bmins[1]; corners[5][2] = bmaxs[2];
		corners[6][0] = bmins[0]; corners[6][1] = bmaxs[1]; corners[6][2] = bmaxs[2];
		corners[7][0] = bmaxs[0]; corners[7][1] = bmaxs[1]; corners[7][2] = bmaxs[2];
	}

	{
		int behind = 0;
		for (int i = 0; i < 8; ++i)
		{
			ProjectVector (corners[i], r_matviewproj, proj);
			/* BUG FIX #2 / BUG-C-01: Track corners behind the near plane.
			 * If ALL 8 corners are behind the camera the volume is fully
			 * occluded and we return false.  If some are behind and some in
			 * front, the AABB straddles the near plane — conservatively expand
			 * to the full viewport so we never miss geometry near the edges.
			 * The original code broke out of the loop on the first behind-corner,
			 * which prevented detection of the all-behind case. */
			if (proj[2] <= 0.f)
			{
				behind++;
				/* Expand to full viewport and continue counting. */
				minx = -1.f;
				miny = -1.f;
				maxx =  1.f;
				maxy =  1.f;
				valid = 1;
				continue;
			}
			minx = q_min (minx, proj[0]);
			miny = q_min (miny, proj[1]);
			maxx = q_max (maxx, proj[0]);
			maxy = q_max (maxy, proj[1]);
			valid = 1;
		}
		/* All corners behind the near plane — volume is not visible. */
		if (behind == 8)
			return false;
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
	GLuint buf;
	GLbyte *ofs;
	fog_volume_gpu_t gpu_volumes[MAX_FOGVOLUMES];
	fog_light_lists_gpu_t fog_lights;
	fog_lightgrid_gpu_t fog_lightgrid;
	const int mode = CLAMP (0, (int)Q_rint (r_fogvol_debug.value), 8);
	float inv_viewproj[16];
	GLuint src_tex;
	GLuint dst_tex;
	GLuint dst_fbo;
	GLuint depth_tex;
	GLuint velocity_tex;
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
	float depth_near;
	float depth_far;
	float depth_sky_cutoff;
	int fog_src = 0;
	int fog_dst = 0;
	GLuint final_tex = 0;
	GLuint composite_src_tex = 0;
	GLuint composite_src_fbo = 0;
	qboolean use_test_guard;
	qboolean dumpstate_always;
	fogvol_restore_state_t restore_state;
	qboolean fog_light_enabled = false;
	qboolean fog_lightgrid_enabled = false;
	qboolean fog_lightgrid_has_data = false;
	const lightgrid_t *lightgrid = NULL;
	vec3_t shadow_dir;
	int shadow_samples;
	fogvol_visible_volume_t visible_volumes[MAX_FOGVOLUMES];
	fogvol_light_candidate_t frame_candidates[256];
	fogvol_light_stats_t light_stats;
	vec3_t fog_bounds_mins;
	vec3_t fog_bounds_maxs;
	int visible_count = 0;
	int frame_candidate_count = 0;
	qboolean has_fog_bounds = false;
	const qboolean light_stats_enabled = r_fogvol_light_stats.value > 0.f;

	/* Per-frame validity: only expose a fogvol composite texture after this
	 * frame actually produced one. */
	r_fogvol_composite_valid = false;

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
		Con_Printf ("FOGVOL_TEST dumpstate %s\n", dumpstate_always ? "enabled" : "disabled");
		last_dumpstate = (int)Q_rint (r_fogvol_testvolumes_dumpstate.value);
	}

	use_test_guard = (r_fogvol_testvolumes.value > 0.f);
	if (use_test_guard)
		R_FogVol_CaptureRestoreState (&restore_state);
	if (R_FogVol_TestDebugEnabled ())
	{
		R_GLStateDump ("before-fogvol");
		R_FogVol_LogPipelineState ("FOGVOL_BEGIN");
	}
	R_FogVol_LogStateSnapshot ("before-pass");
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
	depth_near = 0.5f;
	depth_far = gl_farclip.value > depth_near ? gl_farclip.value : depth_near + 1.f;
	depth_sky_cutoff = gl_clipcontrol_able ? 0.001f : 0.999f;

	if (use_halfres && r_fogvol_steps_scale_halfres.value > 0.f)
		steps = (int)Q_rint (r_fogvol_steps.value * r_fogvol_steps_scale_halfres.value);
	else
		steps = (int)Q_rint (r_fogvol_steps.value);
	if (r_fogvol_stepsize.value > 0.f)
	{
		float step_size = q_max (1.f, r_fogvol_stepsize.value);
		steps = (int)Q_rint (depth_far / step_size);
	}
	steps = CLAMP (8, steps, q_max (8, (int)Q_rint (r_fogvol_maxsteps.value)));

	memset (&fog_lights, 0, sizeof (fog_lights));
	memset (&light_stats, 0, sizeof (light_stats));
	lightgrid = Lightgrid_Get ();
	fog_lightgrid_has_data = (lightgrid && lightgrid->octree && r_lightgrid.value > 0.f);
	fog_lightgrid_enabled = fog_lightgrid_has_data && (r_fogvol_lightgrid.value > 0.f);

	if (r_fogvol_light.value > 0.f)
	{
		const double broadphase_start = light_stats_enabled ? Sys_DoubleTime () : 0.0;
		int max_lights = CLAMP (0, (int)Q_rint (r_fogvol_light_max.value), MAX_FOGLIGHTS);
		int write_offset = 0;

		visible_count = R_FogVol_BuildVisibleVolumeCache (visible_volumes, countof (visible_volumes), fog_bounds_mins, fog_bounds_maxs, &has_fog_bounds);
		if (visible_count > 0 && max_lights > 0)
		{
			frame_candidate_count = R_FogVol_BuildFrameLightBroadphase (visible_volumes, visible_count, fog_bounds_mins, fog_bounds_maxs, has_fog_bounds, frame_candidates, countof (frame_candidates));
			light_stats.broadphase_candidates = frame_candidate_count;
		}
		if (light_stats_enabled)
			light_stats.broadphase_seconds = Sys_DoubleTime () - broadphase_start;

		for (int i = 0; i < r_fogvolume_count; ++i)
		{
			int remaining = (int)countof (fog_lights.lights) - write_offset;
			int count = 0;
			if (remaining > 0 && max_lights > 0 && frame_candidate_count > 0)
			{
				const double narrowphase_start = light_stats_enabled ? Sys_DoubleTime () : 0.0;
				int per_volume_cap = q_min (max_lights, remaining);
				count = R_FogVol_BuildLightListForVolume (&r_fogvolumes[i], frame_candidates, frame_candidate_count, &fog_lights.lights[write_offset], per_volume_cap, light_stats_enabled ? &light_stats : NULL);
				if (light_stats_enabled)
					light_stats.narrowphase_seconds += Sys_DoubleTime () - narrowphase_start;
			}

			fog_lights.volumes[i].offset_count[0] = write_offset;
			fog_lights.volumes[i].offset_count[1] = count;
			write_offset += count;
			fog_light_enabled = fog_light_enabled || (count > 0);
		}

		if (light_stats_enabled)
		{
			Con_DPrintf ("fogvol_light_stats: broadphase_candidates=%d narrowphase_candidates=%d accepted=%d broadphase_ms=%.3f narrowphase_ms=%.3f\n",
				light_stats.broadphase_candidates,
				light_stats.narrowphase_candidates,
				light_stats.narrowphase_accepted,
				light_stats.broadphase_seconds * 1000.0,
				light_stats.narrowphase_seconds * 1000.0);
		}
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

		gpu->sphere[0] = v->sphereCenter[0];
		gpu->sphere[1] = v->sphereCenter[1];
		gpu->sphere[2] = v->sphereCenter[2];
		gpu->sphere[3] = q_max (0.f, v->sphereRadius);

		gpu->color_density[0] = v->color[0];
		gpu->color_density[1] = v->color[1];
		gpu->color_density[2] = v->color[2];
		gpu->color_density[3] = v->density;

		gpu->noise_params[0] = CLAMP (0.005f, v->noiseScale, 0.5f);
		gpu->noise_params[1] = CLAMP (0.f, v->noiseAmount, 1.f);
		gpu->noise_params[2] = v->noiseBias;
		gpu->noise_params[3] = v->maxDistance;

		gpu->velocity_windspeed[0] = v->velocity[0];
		gpu->velocity_windspeed[1] = v->velocity[1];
		gpu->velocity_windspeed[2] = v->velocity[2];
		gpu->velocity_windspeed[3] = v->windSpeed;

		gpu->wind_turbulence[0] = v->windDir[0];
		gpu->wind_turbulence[1] = v->windDir[1];
		gpu->wind_turbulence[2] = v->windDir[2];
		gpu->wind_turbulence[3] = v->turbulence;

		gpu->misc[0] = (float)v->priority;
		gpu->misc[1] = (float)v->enabled;
		gpu->misc[2] = v->falloff;
		gpu->misc[3] = v->height;

		gpu->extra[0] = (float)v->shape;
		gpu->extra[1] = (float)((v->blendMode >= 0) ? v->blendMode : (int)Q_rint (r_fogvol_blendmode.value));
		gpu->extra[2] = v->emissiveStrength;
		gpu->extra[3] = v->heightScale;
	}

	GL_Upload (GL_UNIFORM_BUFFER, gpu_volumes, sizeof (fog_volume_gpu_t) * r_fogvolume_count, &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 2, buf, (GLintptr)ofs, sizeof (fog_volume_gpu_t) * r_fogvolume_count);

	/* PERF FIX (C-02): glGetError() forces a GPU-CPU pipeline flush and is
	 * expensive in the per-frame hot path.  Guard with NDEBUG so release builds
	 * skip the sync entirely.  The lights UBO upload failure path becomes a
	 * no-op in release, which is acceptable: a failed UBO upload would simply
	 * leave fog lights disabled for one frame rather than crashing. */
#if !defined(NDEBUG)
	/* Clear any stale GL errors before the lights UBO upload so glGetError()
	 * below only catches errors from THIS call, not from earlier unrelated GL
	 * operations — a stale error would silently disable all fog lights. */
	while (glGetError () != GL_NO_ERROR) {}
#endif
	GL_Upload (GL_UNIFORM_BUFFER, &fog_lights, sizeof (fog_lights), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 4, buf, (GLintptr)ofs, sizeof (fog_lights));
#if !defined(NDEBUG)
	if (glGetError () != GL_NO_ERROR)
	{
		fog_light_enabled = false;
		memset (&fog_lights, 0, sizeof (fog_lights));
		GL_Upload (GL_UNIFORM_BUFFER, &fog_lights, sizeof (fog_lights), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 4, buf, (GLintptr)ofs, sizeof (fog_lights));
	}
#endif

	GL_BeginGroup ("Fog volumes");
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
	GL_Uniform1iFunc (23, r_framecount);
	GL_Uniform1iFunc (24, r_fogvol_noise_subsample.value > 0.f ? 1 : 0);
	GL_Uniform1fFunc (25, q_max (1.f, r_fogvol_noise_lod_switch_dist.value));
	GL_Uniform1fFunc (26, q_max (0.f, r_fogvol_domainwarp_dist.value));
	GL_Uniform1iFunc (27, r_fogvol_checkerboard.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (28, use_halfres ? 1 : 0);
	GL_Uniform1iFunc (29, r_fogvol_light_subsample.value > 0.f ? 1 : 0);
	GL_UniformMatrix4fvFunc (4, 1, GL_FALSE, inv_viewproj);
	GL_Uniform3fFunc (8, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	GL_Uniform4fFunc (9, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);
	GL_Uniform2fFunc (10, depth_scale_x, depth_scale_y);
	/* BUG FIX (halfres distortion): In halfres mode the fog viewport is placed
	 * at (0,0) so gl_FragCoord.xy starts from (0,0), not (view_x,view_y).
	 * FogDepthScale already maps fragcoord to full-res pixel space, so
	 * screenPos = fragcoord * DepthScale correctly ranges over [0,glwidth) even
	 * in halfres.  The shader computes viewUv as
	 *   (screenPos - FogViewParams.xy) * FogViewParams.zw
	 * In fullres the viewport IS at (view_x,view_y), so screenPos starts there
	 * and subtracting view_x/view_y gives (0,0) at the view origin — correct.
	 * In halfres screenPos starts at (0,0), so passing view_x/view_y shifts the
	 * entire UV mapping into negative territory, causing the visible warping.
	 * Fix: pass xy=(0,0) in halfres; zw=(1/view_w,1/view_h) is unchanged since
	 * screenPos is already in full-res-equivalent coordinates. */
	GL_Uniform4fFunc (11,
		use_halfres ? 0.f : view_x,
		use_halfres ? 0.f : view_y,
		1.f / view_w, 1.f / view_h);
	GL_Uniform4fFunc (12, depth_near, depth_far, gl_clipcontrol_able ? 1.f : 0.f, depth_sky_cutoff);
	GL_Uniform2fFunc (13, q_max (0.f, r_fogvol_density_scale.value), q_max (0.001f, r_fogvol_sigma_max.value));
	GL_Uniform1iFunc (14, r_fogvol_emissive.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (15, CLAMP (0, (int)Q_rint (r_fogvol_blendmode.value), 1));
	GL_Uniform1iFunc (16, fog_light_enabled ? 1 : 0);
	shadow_samples = CLAMP (1, (int)Q_rint (r_fogvol_shadow_samples.value), 8);
	if (r_fogvol_sun_dir.value > 0.f && R_GetSun (shadow_dir, NULL, NULL, NULL))
	{
		/* Convention: r_sun.dir points from scene toward the sun (light source).
		 * Fog shadow marching moves from sample point toward the blocker, so use
		 * the same direction without flipping to keep shader/CPU conventions aligned. */
	}
	else
	{
		VectorScale (vpn, -1.f, shadow_dir);
	}
	/* BUG FIX (C-09): Length check must happen BEFORE VectorNormalize.
	 * VectorNormalize on a zero-vector causes division-by-zero / NaN.
	 * Only normalize when the vector is non-degenerate. */
	if (VectorLength (shadow_dir) < 0.001f)
		VectorSet (shadow_dir, 0.f, 0.f, -1.f);
	else
		VectorNormalize (shadow_dir);
	GL_Uniform1iFunc (17, r_fogvol_shadow.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (18, shadow_samples);
	GL_Uniform1fFunc (19, CLAMP (0.f, r_fogvol_shadow_strength.value, 4.f));
	GL_Uniform1fFunc (20, r_fogvol_shadow_jitter.value > 0.f ? 1.f : 0.f);
	GL_Uniform3fFunc (21, shadow_dir[0], shadow_dir[1], shadow_dir[2]);
	GL_Uniform1iFunc (22, fog_lightgrid_enabled ? 1 : 0);

	if (use_halfres)
		glViewport (0, 0, fog_width, fog_height);
	else
		glViewport ((int)view_x, (int)view_y, (int)view_w, (int)view_h);
	depth_tex = framebufs.composite.depth_stencil_tex;
	velocity_tex = 0;
	if (framebufs.scene.velocity_tex)
	{
		qboolean msaa = framebufs.scene.samples > 1;
		velocity_tex = msaa ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
		/* r_fogvol.c has no access to gl_rmain.c's internal framesetup state.
		 * Keep the available velocity texture and let TAA/motion resolve handle
		 * stale-history rejection through their normal confidence path. */
	}
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
	/* BUG FIX #3: fog_src must start at 0 and be maintained across iterations.
	 * The original never initialised fog_src before the loop's first use in
	 * `fog_dst = (i==0) ? 0 : (1 - fog_src)`, and never advanced fog_src after
	 * the first iteration for the i==0 case.  fog_src is now explicitly set to
	 * match fog_dst at the bottom of each iteration via `fog_src = fog_dst`
	 * (which already existed at line 1181) — the missing piece was initialising
	 * it consistently here so the first `(1 - fog_src)` is deterministic. */
	fog_src = 0;

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;
		GLuint src_fbo;
		static const vec3_t corner_lut[8] = {
			{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f},
			{0.f, 0.f, 1.f}, {1.f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {1.f, 1.f, 1.f}
		};

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
			R_FogVol_LogHazardPass ("FINAL_COPY", src_tex, 0, 0);
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
			/* BUG FIX: Update src_fbo to match src_tex after the copy.
			 * Previously src_fbo stayed as composite.fbo while src_tex
			 * became finalcopy_tex, making the subsequent READ_FRAMEBUFFER
			 * bind incorrect and hazard checks misleading. */
			src_tex = framebufs.fogvol.finalcopy_tex;
			src_fbo = framebufs.fogvol.finalcopy_fbo;
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
		if (mode == 1)
			Con_DPrintf ("FOGVOL debug ITER idx=%d src_tex=%u depth_tex=%u dst_tex=%u src_fbo=%u dst_fbo=%u scissor=(%d %d %d %d)\n",
				i, src_tex, depth_tex, dst_tex, src_fbo, dst_fbo, x0, y0, x1 - x0, y1 - y0);
		R_FogVol_AssertNoFeedbackHazard ("ITER", dst_tex, src_tex);
		R_FogVol_AssertNoBoundFeedbackHazard ("ITER");
		memset (&fog_lightgrid, 0, sizeof (fog_lightgrid));
		if (fog_lightgrid_enabled)
		{
			for (int c = 0; c < 8; ++c)
			{
				vec3_t probe_pos;
				const lightgrid_probe_t *probe;
				probe_pos[0] = v->mins[0] + (v->maxs[0] - v->mins[0]) * corner_lut[c][0];
				probe_pos[1] = v->mins[1] + (v->maxs[1] - v->mins[1]) * corner_lut[c][1];
				probe_pos[2] = v->mins[2] + (v->maxs[2] - v->mins[2]) * corner_lut[c][2];
				probe = R_GetLightgridSample (probe_pos);
				if (probe)
				{
					fog_lightgrid.probe_rgb[c][0] = probe->rgb[0] * probe->ao;
					fog_lightgrid.probe_rgb[c][1] = probe->rgb[1] * probe->ao;
					fog_lightgrid.probe_rgb[c][2] = probe->rgb[2] * probe->ao;
				}
			}
		}
		GL_Upload (GL_UNIFORM_BUFFER, &fog_lightgrid, sizeof (fog_lightgrid), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 5, buf, (GLintptr)ofs, sizeof (fog_lightgrid));
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
	/* BUG FIX (C-08): final_fbo is derived from fog_src which is 0 here only
	 * when no iterations ran (has_drawn=false).  In that case the code jumps
	 * to `done` via the has_drawn check below and final_fbo is never used —
	 * but the declaration order made this non-obvious and fragile.  Declare
	 * final_fbo only after confirming has_drawn so the value is always valid
	 * when it reaches code that actually uses it.  This matches final_tex
	 * which was already set correctly inside the loop via fog_src=fog_dst. */
	{
		GLuint final_fbo = framebufs.fogvol.fbo[fog_src];

	if (!has_drawn)
	{
		R_FogVol_BindFramebuffer (GL_FRAMEBUFFER, framebufs.composite.fbo);
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "temporal draw=COLOR_ATTACHMENT0");
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "temporal read=COLOR_ATTACHMENT0");
		glViewport (glx, gly, glwidth, glheight);
		goto done;
	}

	if (r_fogvol_temporal_alpha.value > 0.f && glprogs.fogvol_temporal && final_tex)
	{
		int history_valid = (r_fogvol_history_width == fog_width && r_fogvol_history_height == fog_height);
		int history_src = r_fogvol_history_index;
		int history_dst = 1 - history_src;
		int composite_src = history_src;
		int composite_dst = 1 - composite_src;
		if (!history_valid)
		{
			r_fogvol_history_width = fog_width;
			r_fogvol_history_height = fog_height;
			/* BUG FIX #4: Reset history_index to 0 on invalidation so we always
			 * start from a known-clean slot after a resolution change, rather
			 * than inheriting whatever index was live from a previous frame
			 * size that no longer matches. */
			r_fogvol_history_index = 0;
			history_src = 0;
			history_dst = 1;
			composite_src = 0;
			composite_dst = 1;
		}

		R_FogVol_UseProgram (glprogs.fogvol_temporal);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		/* history_src and history_dst are always opposite values after the init
		 * block above; keep relying on that invariant. */
		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, history_fbo[history_src]);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.fogvol.composite_fbo[composite_dst]);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "COMPOSITE read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "COMPOSITE draw=COLOR_ATTACHMENT0");
		glViewport (0, 0, fog_width, fog_height);
		R_FogVol_AssertNoFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], final_tex);
		R_FogVol_AssertNoFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], history_tex[history_src]);
		R_FogVol_AssertNoBoundFeedbackHazard ("COMPOSITE");
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, final_tex);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, history_tex[history_src]);
		GL_BindNative (GL_TEXTURE2, GL_TEXTURE_2D, depth_tex);
		GL_BindNative (GL_TEXTURE3, GL_TEXTURE_2D, velocity_tex);
		R_FogVol_LogHazardPass ("COMPOSITE", final_tex, history_tex[history_src], final_tex);
		GL_Uniform1fFunc (0, r_fogvol_temporal_alpha.value);
		GL_Uniform1fFunc (1, r_fogvol_temporal_depth_reject.value);
		GL_Uniform1iFunc (2, mode);
		GL_UniformMatrix4fvFunc (3, 1, GL_FALSE, inv_viewproj);
		GL_Uniform4fFunc (4, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);
		GL_Uniform2fFunc (5, depth_scale_x, depth_scale_y);
		GL_Uniform1iFunc (6, history_valid ? 1 : 0);
		/* BUG FIX (halfres distortion): temporal shader also runs at viewport
		 * (0,0,fog_width,fog_height) in halfres, so view_x/view_y must not be
		 * subtracted — pass (0,0) as the origin, same reasoning as FogViewParams
		 * in the raymarcher pass above. */
		GL_Uniform4fFunc (7,
			use_halfres ? 0.f : view_x,
			use_halfres ? 0.f : view_y,
			1.f / view_w, 1.f / view_h);
		GL_Uniform4fFunc (8,
			CLAMP (0.f, r_fogvol_temporal_confidence_min_alpha.value, 1.f),
			q_max (0.f, r_fogvol_temporal_disocclusion_bias.value),
			q_max (0.1f, r_fogvol_temporal_clamp_strength.value),
			0.f);
		GL_Uniform1iFunc (9, velocity_tex != 0 ? 1 : 0);
		GL_Uniform1iFunc (10, r_fogvol_checkerboard.value > 0.f ? 1 : 0);
		if (mode == 1)
			Con_DPrintf ("FOGVOL debug COMPOSITE final_tex=%u history_tex=%u depth_tex=%u velocity_tex=%u dst_tex=%u conf[min=%.3f disocc=%.3f clamp=%.3f]\n",
				final_tex, history_tex[history_src], depth_tex, velocity_tex, framebufs.fogvol.composite_tex[composite_dst],
				r_fogvol_temporal_confidence_min_alpha.value, r_fogvol_temporal_disocclusion_bias.value, r_fogvol_temporal_clamp_strength.value);
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
	}

	if (has_drawn)
	{
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
				/* BUG FIX (white screen): protect alpha channel — upsample shader
				 * outputs alpha=1.0 (fixed in fogvol_upsample.frag), but guard here
				 * so the composite FBO alpha is never overwritten. */
				glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
				glDrawArrays (GL_TRIANGLES, 0, 3);
				glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			}
			else
			{
				R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, final_fbo);
				R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
				R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
				R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
				R_FogVol_LogHazardPass ("HISTORY", final_tex, 0, final_tex);
				R_FogVol_AssertNoFeedbackHazard ("HISTORY", framebufs.composite.color_tex, final_tex);
				/* BUG FIX (white screen): protect alpha channel on linear upscale blit. */
				glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
				GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
					0, 0, glwidth, glheight,
					GL_COLOR_BUFFER_BIT, GL_LINEAR);
				glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
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
			/* BUG FIX (white screen): Protect composite FBO alpha channel.
			 * BUG FIX (grey washout): Only blit the viewrect region, not the
			 * full glwidth×glheight.  The fog FBO is cleared to (0,0,0,0)
			 * outside the viewrect; blitting that area would overwrite HUD
			 * and other non-fog pixels with black.  Blit only the view area
			 * that was actually rendered by the fog shader. */
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			GL_BlitFramebufferFunc (
				(int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h),
				(int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h),
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}

		r_fogvol_composite_valid = true;
	}

	if (mode == 1)
		R_DebugFlushGeometry ();

	} /* end final_fbo block (BUG FIX C-08) */

done:
	R_FogVol_LogStateSnapshot ("after-pass");
	if (use_halfres)
	{
		GLint viewport[4] = {0};
		GLint scissor_box[4] = {0};
		GLboolean scissor_enabled = GL_FALSE;
		const GLint expected_vp[4] = { glx, gly, glwidth, glheight };

		glGetIntegerv (GL_VIEWPORT, viewport);
		scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
		glGetIntegerv (GL_SCISSOR_BOX, scissor_box);

		if (viewport[0] != expected_vp[0] || viewport[1] != expected_vp[1] || viewport[2] != expected_vp[2] || viewport[3] != expected_vp[3]
			|| (scissor_enabled && (scissor_box[0] != expected_vp[0] || scissor_box[1] != expected_vp[1] || scissor_box[2] != expected_vp[2] || scissor_box[3] != expected_vp[3])))
		{
			Con_DPrintf ("FOGVOL_STATE defensive-restore viewport=(%d %d %d %d) scissor=%d box=(%d %d %d %d)\n",
				viewport[0], viewport[1], viewport[2], viewport[3],
				scissor_enabled,
				scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
			glViewport (expected_vp[0], expected_vp[1], expected_vp[2], expected_vp[3]);
			GL_SetScissorEnabled (false);
			glScissor (expected_vp[0], expected_vp[1], expected_vp[2], expected_vp[3]);
		}
	}
	if (R_FogVol_TestDebugEnabled ())
	{
		R_FogVol_LogPipelineState ("FOGVOL_END");
		R_GLStateDump ("after-fogvol");
	}
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

void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num)
{
	/* TODO: Inject fog volumes into the froxel grid for Forward+ clustered
	 * lighting integration.  Currently a no-op stub — clustered rendering
	 * bypasses this path and the fog volumes are rendered via R_FogVol_Render()
	 * as a separate post-process pass.  When implemented, this function should
	 * iterate vols[0..num-1] and mark each froxel that overlaps the AABB or
	 * sphere of the volume with the volume's density and colour. */
	(void)grid;
	(void)vols;
	(void)num;
}
