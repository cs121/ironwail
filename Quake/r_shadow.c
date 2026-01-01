/*
Copyright (C) 2024

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#include "quakedef.h"
#include <float.h>

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

static GLuint shadow_fbo;
static GLuint shadow_depth_tex;
static int shadowmap_size;

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

void R_InitShadow (void)
{
	shadow_fbo = 0;
	shadow_depth_tex = 0;
	shadowmap_size = 0;
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

	GL_BeginGroup ("Shadow map (sun)");

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, shadow_fbo);
	glViewport (0, 0, shadowmap_size, shadowmap_size);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);

	GL_UseProgram (glprogs.shadow_depth);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_FRONT | GLS_ATTRIBS (6));
	glClear (GL_DEPTH_BUFFER_BIT);

	{
		int count = 0;
		entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
		R_DrawBrushModels_Shadow (ents, count);
	}

	GL_EndGroup ();
}

void R_Shadow_DrawDebug (void)
{
	if ((int)r_shadow_debug.value != 1)
		return;
	if (!shadow_depth_tex || !glprogs.shadow_debug)
		return;

	GL_BeginGroup ("Shadow map debug");

	GL_UseProgram (glprogs.shadow_debug);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, shadow_depth_tex);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}
