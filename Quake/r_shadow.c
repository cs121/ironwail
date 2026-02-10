/*
Copyright (C) 2024

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#include "quakedef.h"
#include <float.h>

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

	Con_Printf (
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
extern cvar_t r_shadow_dlights;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_dlight_distance;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_dlight_pcf_taps;
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

	if (shadowmap_size > 0)
	{
		float texel_x = (extents[0] * 2.f) / (float)shadowmap_size;
		float texel_y = (extents[1] * 2.f) / (float)shadowmap_size;
		if (texel_x > 0.f)
			center_ls[0] = floorf (center_ls[0] / texel_x) * texel_x;
		if (texel_y > 0.f)
			center_ls[1] = floorf (center_ls[1] / texel_y) * texel_y;
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
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	{
		const float border[4] = { 1.f, 1.f, 1.f, 1.f };
		glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
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
	shadow_fbo = 0;
	shadow_depth_tex = 0;
	shadowmap_size = 0;
	shadow_dlight_fbo = 0;
	shadow_dlight_depth_tex = 0;
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
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	{
		const float border[4] = { 1.f, 1.f, 1.f, 1.f };
		glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

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

void R_Shadow_SunPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadow_sun.value > 0.f;
	vec4_t sun_dir;

	r_framedata.shadow_debug[0] = 0.f;
	IdentityMatrix (r_framedata.shadow_viewproj);
	VectorSet (r_framedata.shadow_sun_dir, 0.f, 0.f, -1.f);
	r_framedata.shadow_sun_dir[3] = 0.f;
	if (!enabled || !glprogs.shadow_depth)
		return;

	R_ResizeShadowMapIfNeeded ();
	if (!shadow_depth_tex || !shadow_fbo)
		return;

	R_Shadow_BuildViewProj (r_framedata.shadow_viewproj, sun_dir);
	r_framedata.shadow_debug[0] = 1.f;
	VectorCopy (sun_dir, r_framedata.shadow_sun_dir);
	r_framedata.shadow_sun_dir[3] = 0.f;
	R_UploadFrameData ();

	GL_BeginGroup ("Shadow map (sun)");

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_fbo);
	glViewport (0, 0, shadowmap_size, shadowmap_size);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);

	GL_UseProgram (glprogs.shadow_depth);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_FRONT | GLS_ATTRIBS (6));
	R_Shadow_LogClearDebug ("R_Shadow_Sun", GL_DEPTH_BUFFER_BIT);
	glClear (GL_DEPTH_BUFFER_BIT);

	{
		int count = 0;
		entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
		R_DrawBrushModels_Shadow (ents, count);
	}
	{
		int count = 0;
		entity_t **ents = R_GetVisEntities (mod_alias, false, &count);
		R_DrawAliasModels_Shadow (ents, count);
	}

	GL_EndGroup ();
}

void R_Shadow_DlightPass (void)
{
	qboolean enabled = r_shadows.value > 0.f && r_shadow_dlights.value > 0.f;
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

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_dlight_fbo);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
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
		R_Shadow_LogClearDebug ("R_Shadow_Dlight", GL_DEPTH_BUFFER_BIT);
		glClear (GL_DEPTH_BUFFER_BIT);

		GL_UseProgram (glprogs.shadow_depth);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_FRONT | GLS_ATTRIBS (6));

		{
			int count = 0;
			entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
			R_DrawBrushModels_Shadow (ents, count);
		}
		{
			int count = 0;
			entity_t **ents = R_GetVisEntities (mod_alias, false, &count);
			R_DrawAliasModels_Shadow (ents, count);
		}
	}

	GL_SetScissorEnabled (false);

	memcpy (r_framedata.shadow_viewproj, sun_viewproj, sizeof (sun_viewproj));

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
