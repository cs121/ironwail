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
cvar_t r_fogvol_noise = { "r_fogvol_noise", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noisemode = { "r_fogvol_noisemode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_debug = { "r_fogvol_debug", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes = { "r_fogvol_testvolumes", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_physblend = { "r_fogvol_physblend", "1", CVAR_ARCHIVE };

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
	Cvar_RegisterVariable (&r_fogvol_noise);
	Cvar_RegisterVariable (&r_fogvol_noisemode);
	Cvar_RegisterVariable (&r_fogvol_debug);
	Cvar_RegisterVariable (&r_fogvol_testvolumes);
	Cvar_RegisterVariable (&r_fogvol_physblend);
}

void R_FogVol_Clear (void)
{
	r_fogvolume_count = 0;
}

static void R_FogVol_ClearEntities (void)
{
	r_fogvolume_entity_count = 0;
}

static void R_FogVol_AddVolume (const fog_volume_t *volume)
{
	if (r_fogvolume_count >= MAX_FOGVOLUMES)
		return;
	r_fogvolumes[r_fogvolume_count++] = *volume;
}

static void R_FogVol_AddEntityVolume (const fog_volume_t *volume)
{
	if (r_fogvolume_entity_count >= MAX_FOGVOLUMES)
		return;
	r_fogvolume_entities[r_fogvolume_entity_count++] = *volume;
}

static void R_FogVol_ParseColor (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
	{
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
		if (!R_FogVol_ProjectAABBToScreenRect (v, &x0, &y0, &x1, &y1, true))
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
	float inv_viewproj[16];
	GLuint src_tex;
	GLuint dst_tex;
	GLuint dst_fbo;
	GLuint depth_tex;
	qboolean dst_is_composite;
	qboolean has_drawn = false;

	if (r_fogvol.value <= 0.f)
		return;
	if (!glprogs.fogvol)
		return;
	if (r_fogvolume_count <= 0)
		return;
	if (framebufs.composite.color_tex == 0 || framebufs.fogvol.color_tex == 0)
		return;
	if (framebufs.composite.depth_stencil_tex == 0)
		return;
	if (!R_FogVol_MatrixInverse4x4 (r_matviewproj, inv_viewproj))
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
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 2, buf, (GLintptr)ofs, sizeof (fog_volume_gpu_t) * r_fogvolume_count);

	GL_BeginGroup ("Fog volumes");
	GL_UseProgram (glprogs.fogvol);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_Uniform1iFunc (0, steps);
	GL_Uniform1iFunc (1, r_fogvol_noise.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (2, mode);
	GL_Uniform1iFunc (5, (int)Q_rint (r_fogvol_noisemode.value));
	GL_Uniform1iFunc (6, r_fogvol_physblend.value > 0.f ? 1 : 0);
	GL_UniformMatrix4fvFunc (4, 1, GL_FALSE, inv_viewproj);
	GL_Uniform3fFunc (8, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	GL_Uniform4fFunc (9, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);

	glEnable (GL_SCISSOR_TEST);
	glViewport (glx, gly, glwidth, glheight);
	depth_tex = framebufs.composite.depth_stencil_tex;
	src_tex = framebufs.composite.color_tex;
	dst_tex = framebufs.fogvol.color_tex;
	dst_fbo = framebufs.fogvol.fbo;
	dst_is_composite = false;

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;

		if (!v->enabled)
			continue;
		if (!R_FogVol_ProjectAABBToScreenRect (v, &x0, &y0, &x1, &y1, true))
			continue;

		if (mode == 1)
		{
			vec3_t color;
			VectorCopy (v->color, color);
			R_DebugDrawWireBox (v->mins, v->maxs, color, true);
		}

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, dst_fbo);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		glReadBuffer (GL_COLOR_ATTACHMENT0);

		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, src_tex);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, depth_tex);
		glScissor (x0, y0, x1 - x0, y1 - y0);
		GL_Uniform1iFunc (3, i);
		glDrawArrays (GL_TRIANGLES, 0, 3);

		{
			GLuint tmp_tex = src_tex;
			src_tex = dst_tex;
			dst_tex = tmp_tex;
			dst_is_composite = !dst_is_composite;
			dst_fbo = dst_is_composite ? framebufs.composite.fbo : framebufs.fogvol.fbo;
		}
		has_drawn = true;
	}
	glDisable (GL_SCISSOR_TEST);

	if (has_drawn && src_tex != framebufs.composite.color_tex)
	{
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.fogvol.fbo);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
		GL_BlitFramebufferFunc (0, 0, glwidth, glheight,
			0, 0, glwidth, glheight,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	if (mode == 1)
		R_DebugFlushGeometry ();

	GL_EndGroup ();
}
