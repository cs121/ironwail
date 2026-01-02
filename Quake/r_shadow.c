/*
Copyright (C) 2024

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#include "quakedef.h"
#include "shaders/texunits.glsl"
#include <float.h>

extern cvar_t gl_farclip;
extern cvar_t r_shadows;
extern cvar_t r_shadowmap;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadowmap_size;
extern cvar_t r_shadow_bias;
extern cvar_t r_shadow_normalbias;
extern cvar_t r_shadowmap_bias;
extern cvar_t r_shadowmap_slopebias;
extern cvar_t r_shadowmap_cull_front;
extern cvar_t r_shadowmap_force_disable_scissor;
extern cvar_t r_shadowmap_freeze;
extern cvar_t r_shadow_freeze;
extern cvar_t r_shadow_pcf;
extern cvar_t r_shadow_pcf_taps;
extern cvar_t r_shadow_debug;
extern cvar_t r_shadowmap_debug;
extern cvar_t r_shadow_debug_depthview;
extern cvar_t r_shadow_debug_depthview_invert;
extern cvar_t r_shadow_sun_dir;
extern cvar_t r_shadow_dlights;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_dlight_distance;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_dlight_pcf_taps;
extern dlight_t *r_dlight_sources[DLIGHT_GPU_MAX];

static GLuint shadow_fbo;
static GLuint shadow_depth_tex;
static GLuint shadow_debug_tex;
static GLuint shadow_compare_sampler;
static GLuint shadow_raw_sampler;
static int shadowmap_size;
static GLuint shadow_dlight_fbo;
static GLuint shadow_dlight_depth_tex;
static int shadow_dlight_atlas_size;
static int shadow_dlight_tile_size;
static int shadow_dlight_tile_count;
static int shadow_dlight_selected_count;
static int shadow_dlight_light_indices[SHADOW_DLIGHT_MAX];
static float shadow_frozen_viewproj[16];
static vec4_t shadow_frozen_sun_dir;
static qboolean shadow_frozen_valid;
static float shadow_dlight_frozen_viewproj[SHADOW_DLIGHT_MAX][16];
static vec4_t shadow_dlight_frozen_atlas[SHADOW_DLIGHT_MAX];
static vec4_t shadow_dlight_frozen_info[SHADOW_DLIGHT_MAX];
static int shadow_dlight_frozen_selected_count;
static qboolean shadow_dlight_frozen_valid;

typedef struct shadow_state_s
{
	GLint framebuffer;
	GLint viewport[4];
	GLint scissor_box[4];
	GLboolean scissor_enabled;
	GLboolean blend_enabled;
	GLboolean color_mask[4];
	GLboolean depth_mask;
	GLboolean depth_test;
	GLint depth_func;
	GLdouble depth_range[2];
} shadow_state_t;

static void R_Shadow_SaveState (shadow_state_t *state)
{
	glGetIntegerv (GL_FRAMEBUFFER_BINDING, &state->framebuffer);
	glGetIntegerv (GL_VIEWPORT, state->viewport);
	glGetIntegerv (GL_SCISSOR_BOX, state->scissor_box);
	glGetBooleanv (GL_SCISSOR_TEST, &state->scissor_enabled);
	glGetBooleanv (GL_BLEND, &state->blend_enabled);
	glGetBooleanv (GL_COLOR_WRITEMASK, state->color_mask);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &state->depth_mask);
	glGetBooleanv (GL_DEPTH_TEST, &state->depth_test);
	glGetIntegerv (GL_DEPTH_FUNC, &state->depth_func);
	glGetDoublev (GL_DEPTH_RANGE, state->depth_range);
}

static void R_Shadow_RestoreState (const shadow_state_t *state)
{
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, state->framebuffer);
	glViewport (state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
	glScissor (state->scissor_box[0], state->scissor_box[1], state->scissor_box[2], state->scissor_box[3]);
	if (state->scissor_enabled)
		glEnable (GL_SCISSOR_TEST);
	else
		glDisable (GL_SCISSOR_TEST);
	if (state->blend_enabled)
		glEnable (GL_BLEND);
	else
		glDisable (GL_BLEND);
	glColorMask (state->color_mask[0], state->color_mask[1], state->color_mask[2], state->color_mask[3]);
	glDepthMask (state->depth_mask);
	if (state->depth_test)
		glEnable (GL_DEPTH_TEST);
	else
		glDisable (GL_DEPTH_TEST);
	glDepthFunc (state->depth_func);
	glDepthRange (state->depth_range[0], state->depth_range[1]);
}

static void R_Shadow_LogFboInfo (const char *label, int width, int height, GLuint depth_tex)
{
	GLint internal_format = 0;
	GLint attachment_type = 0;
	GLint attachment_name = 0;
	GLint draw_buffer = 0;
	GLint read_buffer = 0;

	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
	GL_GetFramebufferAttachmentParameterivFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachment_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attachment_name);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);

	Con_Printf ("Shadow %s FBO: %dx%d depth tex %u fmt 0x%X depth attachment type 0x%X name %d draw 0x%X read 0x%X\n",
		label, width, height, depth_tex, internal_format, attachment_type, attachment_name, draw_buffer, read_buffer);
	if (attachment_type == GL_NONE || attachment_name == 0)
		Con_Warning ("Shadow %s FBO depth attachment missing\n", label);
}

static void R_Shadow_LogDepthTextureParams (const char *label)
{
	GLint compare_mode = 0;
	GLint compare_func = 0;
	GLint internal_format = 0;
	GLint min_filter = 0;
	GLint mag_filter = 0;
	GLint wrap_s = 0;
	GLint wrap_t = 0;

	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &compare_mode);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, &compare_func);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);

	Con_DPrintf ("Shadow %s depth tex params: fmt 0x%X compare_mode 0x%X compare_func 0x%X min 0x%X mag 0x%X wrap_s 0x%X wrap_t 0x%X\n",
		label, internal_format, compare_mode, compare_func, min_filter, mag_filter, wrap_s, wrap_t);
	if (compare_mode == GL_NONE)
		Con_Warning ("Shadow %s depth tex compare mode disabled\n", label);
}

static void R_Shadow_LogPassState (const char *label)
{
	GLdouble clear_depth = 0.0;
	GLint depth_func = 0;
	GLboolean depth_mask = GL_FALSE;
	GLboolean cull_enabled = GL_FALSE;
	GLint cull_mode = 0;
	GLenum fbo_status = GL_FRAMEBUFFER_UNDEFINED;

	glGetDoublev (GL_DEPTH_CLEAR_VALUE, &clear_depth);
	glGetIntegerv (GL_DEPTH_FUNC, &depth_func);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &depth_mask);
	glGetBooleanv (GL_CULL_FACE, &cull_enabled);
	glGetIntegerv (GL_CULL_FACE_MODE, &cull_mode);
	fbo_status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);

	Con_DPrintf ("Shadow %s pass state: clear_depth %.3f depth_func 0x%X depth_mask %d cull %d mode 0x%X fbo 0x%X\n",
		label, clear_depth, depth_func, depth_mask ? 1 : 0, cull_enabled ? 1 : 0, cull_mode, fbo_status);
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
	shadow_dlight_frozen_valid = false;
	shadow_dlight_frozen_selected_count = 0;
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
	if (shadow_debug_tex)
	{
		GL_DeleteNativeTexture (shadow_debug_tex);
		shadow_debug_tex = 0;
	}
	if (shadow_compare_sampler)
	{
		GL_DeleteSamplersFunc (1, &shadow_compare_sampler);
		shadow_compare_sampler = 0;
	}
	if (shadow_raw_sampler)
	{
		GL_DeleteSamplersFunc (1, &shadow_raw_sampler);
		shadow_raw_sampler = 0;
	}
	shadowmap_size = 0;
	shadow_frozen_valid = false;
	R_Shadow_DestroyDlightResources ();
}

static void R_Shadow_GetSunDirection (vec3_t out_dir)
{
	vec3_t dir = { 0.3f, 0.5f, -1.0f };
	float x = dir[0];
	float y = dir[1];
	float z = dir[2];

	if (r_shadow_sun_dir.string && *r_shadow_sun_dir.string)
	{
		if (sscanf (r_shadow_sun_dir.string, "%f %f %f", &x, &y, &z) == 3)
		{
			dir[0] = x;
			dir[1] = y;
			dir[2] = z;
		}
	}

	VectorCopy (dir, out_dir);
	if (VectorNormalize (out_dir) == 0.f)
	{
		out_dir[0] = 0.f;
		out_dir[1] = 0.f;
		out_dir[2] = -1.f;
	}
}

static void R_Shadow_BuildViewProj (float out_viewproj[16], vec4_t out_sun_dir)
{
	vec3_t sun_dir;
	vec3_t up = { 0.f, 0.f, 1.f };
	vec3_t right;
	vec3_t light_up;
	vec3_t view_forward;
	vec3_t view_right;
	vec3_t view_up;
	vec3_t camera_center;
	vec3_t eye;
	float view[16];
	float ortho[16];
	float tanx;
	float tany;
	float znear;
	float zfar;
	float wnear;
	float hnear;
	float wfar;
	float hfar;
	float center_dist;
	float near_dist;
	float far_dist;
	float shadow_radius;

	R_Shadow_GetSunDirection (sun_dir);
	VectorCopy (sun_dir, out_sun_dir);
	out_sun_dir[3] = 0.f;

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
	(void)view_right;
	(void)view_up;

	tanx = tanf (DEG2RAD (r_fovx) * 0.5f);
	tany = tanf (DEG2RAD (r_fovy) * 0.5f);

	znear = 1.f;
	zfar = q_max (gl_farclip.value, znear + 1.f);

	wnear = tanx * znear;
	hnear = tany * znear;
	wfar = tanx * zfar;
	hfar = tany * zfar;

	center_dist = 0.5f * (zfar + znear);
	near_dist = sqrtf (wnear * wnear + hnear * hnear + (center_dist - znear) * (center_dist - znear));
	far_dist = sqrtf (wfar * wfar + hfar * hfar + (zfar - center_dist) * (zfar - center_dist));
	shadow_radius = q_max (near_dist, far_dist);
	if (shadow_radius <= 0.f)
		shadow_radius = 1.f;

	VectorMA (r_refdef.vieworg, center_dist, view_forward, camera_center);
	VectorMA (camera_center, -shadow_radius, sun_dir, eye);

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
	view[12] = -DotProduct (right, eye);
	view[13] = -DotProduct (light_up, eye);
	view[14] = -DotProduct (sun_dir, eye);

	{
		float znear = 0.f;
		float zfar = shadow_radius * 2.f;

		if (gl_clipcontrol_able)
		{
			float tmp = znear;
			znear = zfar;
			zfar = tmp;
		}

		R_Shadow_OrthoMatrix (ortho, -shadow_radius, shadow_radius, -shadow_radius, shadow_radius, znear, zfar);
	}

	memcpy (out_viewproj, ortho, sizeof (ortho));
	MatrixMultiply (out_viewproj, view);
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
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	{
		const float border[4] = { 1.f, 1.f, 1.f, 1.f };
		glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	{
		static qboolean logged = false;
		if (!logged)
		{
			R_Shadow_LogDepthTextureParams ("dlight");
			logged = true;
		}
	}

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
	R_Shadow_LogFboInfo ("dlight", atlas_size, atlas_size, shadow_dlight_depth_tex);

	shadow_dlight_atlas_size = atlas_size;
	shadow_dlight_tile_size = tile_size;
	shadow_dlight_tile_count = grid * grid;
}
void R_InitShadow (void)
{
	shadow_fbo = 0;
	shadow_depth_tex = 0;
	shadow_debug_tex = 0;
	shadow_compare_sampler = 0;
	shadow_raw_sampler = 0;
	shadowmap_size = 0;
	shadow_dlight_fbo = 0;
	shadow_dlight_depth_tex = 0;
	shadow_dlight_atlas_size = 0;
	shadow_dlight_tile_size = 0;
	shadow_dlight_tile_count = 0;
	shadow_dlight_selected_count = 0;
	shadow_frozen_valid = false;
	shadow_dlight_frozen_valid = false;
	shadow_dlight_frozen_selected_count = 0;
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

	if (desired > 0 && desired < 256)
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
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	{
		static qboolean logged = false;
		if (!logged)
		{
			R_Shadow_LogDepthTextureParams ("sun");
			logged = true;
		}
	}

	if (!shadow_compare_sampler || !shadow_raw_sampler)
	{
		if (shadow_compare_sampler)
			GL_DeleteSamplersFunc (1, &shadow_compare_sampler);
		if (shadow_raw_sampler)
			GL_DeleteSamplersFunc (1, &shadow_raw_sampler);
		shadow_compare_sampler = 0;
		shadow_raw_sampler = 0;

		GL_GenSamplersFunc (1, &shadow_compare_sampler);
		GL_GenSamplersFunc (1, &shadow_raw_sampler);

		GL_SamplerParameteriFunc (shadow_compare_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		GL_SamplerParameteriFunc (shadow_compare_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		GL_SamplerParameteriFunc (shadow_compare_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		GL_SamplerParameteriFunc (shadow_compare_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		GL_SamplerParameteriFunc (shadow_compare_sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		GL_SamplerParameteriFunc (shadow_compare_sampler, GL_TEXTURE_COMPARE_FUNC, gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);

		GL_SamplerParameteriFunc (shadow_raw_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		GL_SamplerParameteriFunc (shadow_raw_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		GL_SamplerParameteriFunc (shadow_raw_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		GL_SamplerParameteriFunc (shadow_raw_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		GL_SamplerParameteriFunc (shadow_raw_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	}

	glGenTextures (1, &shadow_debug_tex);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_debug_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, shadow_debug_tex, -1, "shadowmap debug depth");
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, GL_R32F, desired, desired);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	GL_GenFramebuffersFunc (1, &shadow_fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_fbo);
	GL_ObjectLabelFunc (GL_FRAMEBUFFER, shadow_fbo, -1, "shadowmap fbo");
	GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_depth_tex, 0);
	GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_debug_tex, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);

	{
		GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			Sys_Error ("Failed to create shadowmap FBO (status code 0x%X)", status);
	}
	R_Shadow_LogFboInfo ("sun", desired, desired, shadow_depth_tex);

	shadowmap_size = desired;
}

void R_Shadow_BindShadowMap (GLenum texunit)
{
	GL_BindNative (texunit, GL_TEXTURE_2D, shadow_depth_tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	GL_BindSamplerFunc (texunit - GL_TEXTURE0, shadow_raw_sampler);
}

void R_Shadow_BindShadowMapRaw (GLenum texunit)
{
	R_Shadow_BindShadowMap (texunit);
}

void R_Shadow_BindDlightShadowMap (GLenum texunit)
{
	if (shadow_dlight_depth_tex)
	{
		GL_BindNative (texunit, GL_TEXTURE_2D, shadow_dlight_depth_tex);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);
		GL_BindSamplerFunc (texunit - GL_TEXTURE0, 0);
	}
	else
	{
		GL_BindNative (texunit, GL_TEXTURE_2D, 0);
		GL_BindSamplerFunc (texunit - GL_TEXTURE0, 0);
	}
}

void R_Shadow_SunPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadowmap.value > 0.f && r_shadow_sun.value > 0.f;
	vec4_t sun_dir;
	float debug_mode = r_shadowmap_debug.value > 0.f ? r_shadowmap_debug.value : r_shadow_debug.value;
	static int last_debug_mode = -1;
	static qboolean logged_state = false;
	qboolean debug_depth = r_shadow_debug_depthview.value > 0.f;
	shadow_state_t prev_state;

	r_framedata.shadow_debug[0] = 0.f;
	IdentityMatrix (r_framedata.shadow_viewproj);
	VectorSet (r_framedata.shadow_sun_dir, 0.f, 0.f, -1.f);
	r_framedata.shadow_sun_dir[3] = 0.f;
	if (!enabled || !glprogs.shadow_depth)
		return;

	R_ResizeShadowMapIfNeeded ();
	if (!shadow_depth_tex || !shadow_fbo)
		return;

	if ((r_shadowmap_freeze.value > 0.f || r_shadow_freeze.value > 0.f) && shadow_frozen_valid)
	{
		memcpy (r_framedata.shadow_viewproj, shadow_frozen_viewproj, sizeof (shadow_frozen_viewproj));
		VectorCopy (shadow_frozen_sun_dir, r_framedata.shadow_sun_dir);
		r_framedata.shadow_sun_dir[3] = 0.f;
	}
	else
	{
		R_Shadow_BuildViewProj (r_framedata.shadow_viewproj, sun_dir);
		VectorCopy (sun_dir, r_framedata.shadow_sun_dir);
		r_framedata.shadow_sun_dir[3] = 0.f;
		memcpy (shadow_frozen_viewproj, r_framedata.shadow_viewproj, sizeof (shadow_frozen_viewproj));
		VectorCopy (sun_dir, shadow_frozen_sun_dir);
		shadow_frozen_sun_dir[3] = 0.f;
		shadow_frozen_valid = true;
	}
	r_framedata.shadow_debug[0] = 1.f;
	R_UploadFrameData ();

	GL_BeginGroup ("Shadow map (sun)");

	R_Shadow_SaveState (&prev_state);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_fbo);
	glViewport (0, 0, shadowmap_size, shadowmap_size);
	if (debug_depth && shadow_debug_tex)
	{
		const GLenum buffers[1] = { GL_COLOR_ATTACHMENT0 };
		GL_DrawBuffersFunc (1, buffers);
		glReadBuffer (GL_NONE);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
	else
	{
		glDrawBuffer (GL_NONE);
		glReadBuffer (GL_NONE);
		glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	}
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);
	glDepthMask (GL_TRUE);
	glEnable (GL_DEPTH_TEST);
	glDepthRange (0.0, 1.0);
	glClearDepth (gl_clipcontrol_able ? 0.f : 1.f);
	glDepthFunc (gl_clipcontrol_able ? GL_GREATER : GL_LEQUAL);
	if (!logged_state)
	{
		R_Shadow_LogPassState ("sun");
		logged_state = true;
	}

	GL_UseProgram (glprogs.shadow_depth);
	GL_Uniform1iFunc (1, debug_depth ? 1 : 0);
	GL_SetState (GLS_BLEND_OPAQUE |
		(debug_depth ? GLS_CULL_NONE :
			(r_shadowmap_cull_front.value > 0.f ? GLS_CULL_FRONT : GLS_CULL_BACK)) |
		GLS_ATTRIBS (6));
	glClear (GL_DEPTH_BUFFER_BIT);

	{
		int count = 0;
		entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
		R_DrawBrushModels_Shadow (ents, count);
	}
	{
		int count = 0;
		entity_t **ents = R_GetVisEntities (mod_alias, false, &count);
		for (int i = 0; i < count; ++i)
		{
			entity_t *ent = ents[i];
			if (ent && ent->model && ent->model->type == mod_alias)
				R_DrawAliasShadow (ent);
		}
		R_FlushAliasShadows ();
	}

	R_Shadow_RestoreState (&prev_state);

	if (debug_mode >= 0.5f && (int)debug_mode != last_debug_mode)
	{
		Con_DPrintf ("Shadow viewproj m00=%.4f m11=%.4f sun_dir=(%.3f %.3f %.3f)\n",
			r_framedata.shadow_viewproj[0],
			r_framedata.shadow_viewproj[5],
			r_framedata.shadow_sun_dir[0],
			r_framedata.shadow_sun_dir[1],
			r_framedata.shadow_sun_dir[2]);
		last_debug_mode = (int)debug_mode;
	}
	else if (debug_mode < 0.5f)
	{
		last_debug_mode = -1;
	}

	GL_EndGroup ();
}

void R_Shadow_DlightPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadowmap.value > 0.f && r_shadow_dlights.value > 0.f;
	float sun_viewproj[16];
	int max_tiles;
	int grid;
	int tiles_used = 0;
	static qboolean logged_state = false;
	shadow_state_t prev_state;

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

	if (!enabled || !glprogs.shadow_depth)
		return;

	R_Shadow_ResizeDlightAtlasIfNeeded ();
	if (!shadow_dlight_depth_tex || !shadow_dlight_fbo)
		return;

	max_tiles = CLAMP (0, (int)r_shadow_dlight_max.value, SHADOW_DLIGHT_MAX);
	if (shadow_dlight_tile_count > 0)
		max_tiles = q_min (max_tiles, shadow_dlight_tile_count);
	if (max_tiles <= 0)
		return;

	if (r_framedata.numlights == 0)
		return;

	if ((r_shadowmap_freeze.value > 0.f || r_shadow_freeze.value > 0.f) && shadow_dlight_frozen_valid)
	{
		for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
		{
			memcpy (r_framedata.shadow_dlight_viewproj[i], shadow_dlight_frozen_viewproj[i], sizeof (shadow_dlight_frozen_viewproj[i]));
			memcpy (r_framedata.shadow_dlight_atlas[i], shadow_dlight_frozen_atlas[i], sizeof (shadow_dlight_frozen_atlas[i]));
			memcpy (r_framedata.shadow_dlight_info[i], shadow_dlight_frozen_info[i], sizeof (shadow_dlight_frozen_info[i]));
		}
		shadow_dlight_selected_count = shadow_dlight_frozen_selected_count;
		R_UploadFrameData ();
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
		return;

	memcpy (sun_viewproj, r_framedata.shadow_viewproj, sizeof (sun_viewproj));

	GL_BeginGroup ("Shadow map (dlights)");

	R_Shadow_SaveState (&prev_state);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_dlight_fbo);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask (GL_TRUE);
	glEnable (GL_DEPTH_TEST);
	glDepthRange (0.0, 1.0);
	glClearDepth (gl_clipcontrol_able ? 0.f : 1.f);
	glDepthFunc (gl_clipcontrol_able ? GL_GREATER : GL_LEQUAL);
	if (!logged_state)
	{
		R_Shadow_LogPassState ("dlights");
		logged_state = true;
	}

	grid = shadow_dlight_atlas_size / shadow_dlight_tile_size;
	if (grid < 1)
		grid = 1;

	for (int i = 0; i < max_tiles; ++i)
	{
		int light_index = shadow_dlight_light_indices[i];
		if (light_index < 0)
			continue;

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

		glEnable (GL_SCISSOR_TEST);
		glViewport (tile_x * shadow_dlight_tile_size, tile_y * shadow_dlight_tile_size,
			shadow_dlight_tile_size, shadow_dlight_tile_size);
		glScissor (tile_x * shadow_dlight_tile_size, tile_y * shadow_dlight_tile_size,
			shadow_dlight_tile_size, shadow_dlight_tile_size);
		glClear (GL_DEPTH_BUFFER_BIT);

		GL_UseProgram (glprogs.shadow_depth);
		GL_Uniform1iFunc (1, 0);
		GL_SetState (GLS_BLEND_OPAQUE |
			(r_shadowmap_cull_front.value > 0.f ? GLS_CULL_FRONT : GLS_CULL_BACK) |
			GLS_ATTRIBS (6));

		{
			int count = 0;
			entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
			R_DrawBrushModels_Shadow (ents, count);
		}
		{
			int count = 0;
			entity_t **ents = R_GetVisEntities (mod_alias, false, &count);
			for (int i = 0; i < count; ++i)
			{
				entity_t *ent = ents[i];
				if (ent && ent->model && ent->model->type == mod_alias)
					R_DrawAliasShadow (ent);
			}
			R_FlushAliasShadows ();
		}
	}

	glDisable (GL_SCISSOR_TEST);

	memcpy (r_framedata.shadow_viewproj, sun_viewproj, sizeof (sun_viewproj));

	shadow_dlight_frozen_selected_count = shadow_dlight_selected_count;
	for (int i = 0; i < SHADOW_DLIGHT_MAX; ++i)
	{
		memcpy (shadow_dlight_frozen_viewproj[i], r_framedata.shadow_dlight_viewproj[i], sizeof (shadow_dlight_frozen_viewproj[i]));
		memcpy (shadow_dlight_frozen_atlas[i], r_framedata.shadow_dlight_atlas[i], sizeof (shadow_dlight_frozen_atlas[i]));
		memcpy (shadow_dlight_frozen_info[i], r_framedata.shadow_dlight_info[i], sizeof (shadow_dlight_frozen_info[i]));
	}
	shadow_dlight_frozen_valid = true;

	R_Shadow_RestoreState (&prev_state);

	GL_EndGroup ();
}

void R_Shadow_DrawDebug (void)
{
	int mode = (int)(r_shadowmap_debug.value > 0.f ? r_shadowmap_debug.value : r_shadow_debug.value);
	if (mode != 1)
		return;
	if (!glprogs.shadow_debug)
		return;
	if (mode == 1 && !shadow_depth_tex)
		return;

	GL_BeginGroup ("Shadow map debug");

	GL_UseProgram (glprogs.shadow_debug);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	R_Shadow_BindShadowMapRaw (GL_TEXTURE0);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}

void R_Shadow_DrawDepthDebugQuad (void)
{
	if (r_shadow_debug_depthview.value <= 0.f)
		return;
	if (!glprogs.shadow_depth_debug)
		return;
	if (!shadow_depth_tex)
		return;

	GL_BeginGroup ("Shadow map depth view");

	GL_UseProgram (glprogs.shadow_depth_debug);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_Uniform1fFunc (0, r_shadow_debug_depthview_invert.value > 0.f ? 1.f : 0.f);
	GL_Uniform1fFunc (1, 0.f);
	GL_Uniform1fFunc (2, 0.f);
	if (shadow_debug_tex)
	{
		GL_BindNative (GL_TEXTURE0 + TEXUNIT_SHADOW, GL_TEXTURE_2D, shadow_debug_tex);
		GL_BindSamplerFunc (TEXUNIT_SHADOW, shadow_raw_sampler);
	}
	else
	{
		R_Shadow_BindShadowMapRaw (GL_TEXTURE0 + TEXUNIT_SHADOW);
	}
	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}
