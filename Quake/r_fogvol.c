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

cvar_t r_fogvol = { "r_fogvol", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_steps = { "r_fogvol_steps", "32", CVAR_ARCHIVE };
cvar_t r_fogvol_halfres = { "r_fogvol_halfres", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_noise = { "r_fogvol_noise", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_debug = { "r_fogvol_debug", "0", CVAR_ARCHIVE };

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
	Cvar_RegisterVariable (&r_fogvol_noise);
	Cvar_RegisterVariable (&r_fogvol_debug);
}

void R_FogVol_Clear (void)
{
	r_fogvolume_count = 0;
}

static void R_FogVol_AddVolume (const fog_volume_t *volume)
{
	if (r_fogvolume_count >= MAX_FOGVOLUMES)
		return;
	r_fogvolumes[r_fogvolume_count++] = *volume;
}

void R_FogVol_AddTestVolumes (void)
{
	fog_volume_t volume;
	vec3_t origin;
	VectorCopy (r_refdef.vieworg, origin);

	memset (&volume, 0, sizeof (volume));
	VectorSet (volume.color, 0.7f, 0.8f, 1.0f);
	volume.density = 0.15f;
	volume.noiseScale = 0.05f;
	volume.noiseAmount = 0.5f;
	volume.noiseBias = 0.0f;
	VectorSet (volume.velocity, 4.f, 0.f, 0.f);
	volume.maxDistance = 0.f;
	volume.priority = 0;
	volume.enabled = 1;
	VectorSet (volume.mins, origin[0] - 64.f, origin[1] - 64.f, origin[2] - 32.f);
	VectorSet (volume.maxs, origin[0] + 64.f, origin[1] + 64.f, origin[2] + 96.f);
	R_FogVol_AddVolume (&volume);

	memset (&volume, 0, sizeof (volume));
	VectorSet (volume.color, 0.8f, 0.6f, 0.4f);
	volume.density = 0.2f;
	volume.noiseScale = 0.08f;
	volume.noiseAmount = 0.6f;
	volume.noiseBias = 0.0f;
	VectorSet (volume.velocity, -2.f, 1.f, 0.f);
	volume.maxDistance = 0.f;
	volume.priority = 1;
	volume.enabled = 1;
	VectorSet (volume.mins, origin[0] + 96.f, origin[1] - 96.f, origin[2] - 16.f);
	VectorSet (volume.maxs, origin[0] + 192.f, origin[1] + 0.f, origin[2] + 64.f);
	R_FogVol_AddVolume (&volume);
}

void R_FogVol_BuildList (void)
{
	R_FogVol_Clear ();

	if (r_fogvol.value <= 0.f)
		return;

	R_FogVol_AddTestVolumes ();

	if (r_fogvolume_count > 1)
		qsort (r_fogvolumes, r_fogvolume_count, sizeof (fog_volume_t), R_FogVol_ComparePriority);
}

qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1)
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

	if (!GL_NeedsSceneEffects ())
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
	int mode = (int)Q_rint (r_fogvol_debug.value);
	if (mode != 2)
		return;

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		const fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;
		float color[3];
		float alpha = 0.25f;

		if (!v->enabled)
			continue;
		if (!R_FogVol_ProjectAABBToScreenRect (v, &x0, &y0, &x1, &y1))
			continue;

		color[0] = v->color[0];
		color[1] = v->color[1];
		color[2] = v->color[2];
		Draw_FillEx ((float)x0, (float)y0, (float)(x1 - x0), 1.f, color, alpha);
		Draw_FillEx ((float)x0, (float)(y1 - 1), (float)(x1 - x0), 1.f, color, alpha);
		Draw_FillEx ((float)x0, (float)y0, 1.f, (float)(y1 - y0), color, alpha);
		Draw_FillEx ((float)(x1 - 1), (float)y0, 1.f, (float)(y1 - y0), color, alpha);
	}
}

void R_FogVol_Render (void)
{
	int steps;
	GLuint buf;
	GLbyte *ofs;
	fog_volume_gpu_t gpu_volumes[MAX_FOGVOLUMES];
	int mode = (int)Q_rint (r_fogvol_debug.value);

	if (r_fogvol.value <= 0.f)
		return;
	if (!glprogs.fogvol)
		return;
	if (r_fogvolume_count <= 0)
		return;

	steps = (int)Q_rint (r_fogvol_steps.value);
	steps = CLAMP (8, steps, 128);

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

		gpu->noise_params[0] = v->noiseScale;
		gpu->noise_params[1] = v->noiseAmount;
		gpu->noise_params[2] = v->noiseBias;
		gpu->noise_params[3] = v->maxDistance;

		gpu->velocity[0] = v->velocity[0];
		gpu->velocity[1] = v->velocity[1];
		gpu->velocity[2] = v->velocity[2];
		gpu->velocity[3] = 0.f;

		gpu->misc[0] = (float)v->priority;
		gpu->misc[1] = (float)v->enabled;
		gpu->misc[2] = 0.f;
		gpu->misc[3] = 0.f;
	}

	GL_Upload (GL_UNIFORM_BUFFER, gpu_volumes, sizeof (fog_volume_gpu_t) * r_fogvolume_count, &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 2, buf, (GLintptr)ofs, sizeof (fog_volume_gpu_t) * r_fogvolume_count);

	GL_BeginGroup ("Fog volumes");
	GL_UseProgram (glprogs.fogvol);
	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_Uniform1iFunc (0, steps);
	GL_Uniform1iFunc (1, r_fogvol_noise.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (2, mode);

	glEnable (GL_SCISSOR_TEST);
	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;

		if (!v->enabled)
			continue;
		if (!R_FogVol_ProjectAABBToScreenRect (v, &x0, &y0, &x1, &y1))
			continue;

		if (mode == 1)
		{
			vec3_t color;
			VectorCopy (v->color, color);
			R_DebugDrawWireBox (v->mins, v->maxs, color, true);
		}

		glScissor (x0, y0, x1 - x0, y1 - y0);
		GL_Uniform1iFunc (3, i);
		glDrawArrays (GL_TRIANGLES, 0, 3);
	}
	glDisable (GL_SCISSOR_TEST);

	if (mode == 1)
		R_DebugFlushGeometry ();

	GL_EndGroup ();
}

