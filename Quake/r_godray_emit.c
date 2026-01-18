/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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
#include "mat_shader.h"
#include "r_godray_emit.h"

typedef struct godray_emitter_sort_s
{
	float score;
	int index;
	int order;
} godray_emitter_sort_t;

cvar_t r_godrays_mode = { "r_godrays_mode", "2", CVAR_ARCHIVE };
cvar_t r_godrays_max_emitters = { "r_godrays_max_emitters", "64", CVAR_ARCHIVE };
cvar_t r_godrays_hero_emitters = { "r_godrays_hero_emitters", "2", CVAR_ARCHIVE };
cvar_t r_godray_volume_length = { "r_godray_volume_length", "256", CVAR_ARCHIVE };
cvar_t r_godray_volume_density = { "r_godray_volume_density", "0.4", CVAR_ARCHIVE };
cvar_t r_godray_volume_intensity = { "r_godray_volume_intensity", "1.0", CVAR_ARCHIVE };
cvar_t r_godray_volume_color = { "r_godray_volume_color", "1 1 1", CVAR_ARCHIVE };
cvar_t r_godray_volume_dir = { "r_godray_volume_dir", "0 -1 0", CVAR_ARCHIVE };

static godray_emitter_t r_godray_emitters[MAX_GODRAY_EMITTERS];
static int r_godray_emitter_count = 0;
static godray_emitter_stats_t r_godray_emitter_stats;

static void R_GodrayEmitters_ParseColor (const char *value, vec3_t color)
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

static qboolean R_GodrayEmitters_ParseVector (const char *value, vec3_t out)
{
	return value && sscanf (value, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

static void R_GodrayEmitters_Clear (void)
{
	r_godray_emitter_count = 0;
	memset (&r_godray_emitter_stats, 0, sizeof (r_godray_emitter_stats));
}

static void R_GodrayEmitters_TransformPoint (const float matrix[16], const vec3_t in, vec3_t out)
{
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2] + matrix[12];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2] + matrix[13];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2] + matrix[14];
}

static void R_GodrayEmitters_TransformVector (const float matrix[16], const vec3_t in, vec3_t out)
{
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2];
}

static qboolean R_GodrayEmitters_Invert3x3 (const float matrix[16], float out[9])
{
	const float a = matrix[0];
	const float b = matrix[4];
	const float c = matrix[8];
	const float d = matrix[1];
	const float e = matrix[5];
	const float f = matrix[9];
	const float g = matrix[2];
	const float h = matrix[6];
	const float i = matrix[10];
	const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);

	if (fabsf (det) < 1e-6f)
		return false;

	const float inv_det = 1.f / det;
	out[0] = (e * i - f * h) * inv_det;
	out[1] = (c * h - b * i) * inv_det;
	out[2] = (b * f - c * e) * inv_det;
	out[3] = (f * g - d * i) * inv_det;
	out[4] = (a * i - c * g) * inv_det;
	out[5] = (c * d - a * f) * inv_det;
	out[6] = (d * h - e * g) * inv_det;
	out[7] = (b * g - a * h) * inv_det;
	out[8] = (a * e - b * d) * inv_det;
	return true;
}

static void R_GodrayEmitters_TransformNormal (const float matrix[16], const vec3_t in, vec3_t out)
{
	float inv[9];

	if (R_GodrayEmitters_Invert3x3 (matrix, inv))
	{
		out[0] = inv[0] * in[0] + inv[3] * in[1] + inv[6] * in[2];
		out[1] = inv[1] * in[0] + inv[4] * in[1] + inv[7] * in[2];
		out[2] = inv[2] * in[0] + inv[5] * in[1] + inv[8] * in[2];
		return;
	}

	R_GodrayEmitters_TransformVector (matrix, in, out);
}

static void R_GodrayEmitters_TransformAABB (const float matrix[16], const vec3_t mins, const vec3_t maxs,
	vec3_t out_mins, vec3_t out_maxs)
{
	vec3_t corners[8];
	vec3_t transformed;

	corners[0][0] = mins[0]; corners[0][1] = mins[1]; corners[0][2] = mins[2];
	corners[1][0] = maxs[0]; corners[1][1] = mins[1]; corners[1][2] = mins[2];
	corners[2][0] = maxs[0]; corners[2][1] = maxs[1]; corners[2][2] = mins[2];
	corners[3][0] = mins[0]; corners[3][1] = maxs[1]; corners[3][2] = mins[2];
	corners[4][0] = mins[0]; corners[4][1] = mins[1]; corners[4][2] = maxs[2];
	corners[5][0] = maxs[0]; corners[5][1] = mins[1]; corners[5][2] = maxs[2];
	corners[6][0] = maxs[0]; corners[6][1] = maxs[1]; corners[6][2] = maxs[2];
	corners[7][0] = mins[0]; corners[7][1] = maxs[1]; corners[7][2] = maxs[2];

	for (int i = 0; i < 8; ++i)
	{
		R_GodrayEmitters_TransformPoint (matrix, corners[i], transformed);
		if (i == 0)
		{
			VectorCopy (transformed, out_mins);
			VectorCopy (transformed, out_maxs);
		}
		else
		{
			for (int j = 0; j < 3; ++j)
			{
				out_mins[j] = q_min (out_mins[j], transformed[j]);
				out_maxs[j] = q_max (out_maxs[j], transformed[j]);
			}
		}
	}
}

static qboolean R_GodrayEmitters_SurfaceMaterial (const qmodel_t *model, const msurface_t *surf,
	const shader_material_t **out_material)
{
	int texnum;
	texture_t *tex;

	if (!model || !surf || !surf->texinfo)
		return false;

	texnum = surf->texinfo->texnum;
	if (texnum < 0 || texnum >= model->numtextures)
		return false;
	tex = model->textures[texnum];
	if (!tex || !tex->shader)
		return false;
	if ((tex->shader_flags & MAT_SHADERFLAG_GODRAY) == 0u)
		return false;

	if (out_material)
		*out_material = tex->shader;
	return true;
}

static float R_GodrayEmitters_ComputeScore (const godray_emitter_t *emitter)
{
	vec3_t delta;
	float dist;
	float color_strength;

	VectorSubtract (emitter->center_ws, r_refdef.vieworg, delta);
	dist = q_max (VectorLength (delta), 1.f);
	color_strength = (emitter->color[0] + emitter->color[1] + emitter->color[2]) * (1.f / 3.f);
	return (emitter->intensity * q_max (color_strength, 0.01f)) / dist;
}

static int R_GodrayEmitters_CompareScores (const void *lhs, const void *rhs)
{
	const godray_emitter_sort_t *a = (const godray_emitter_sort_t *)lhs;
	const godray_emitter_sort_t *b = (const godray_emitter_sort_t *)rhs;

	if (a->score > b->score)
		return -1;
	if (a->score < b->score)
		return 1;
	return a->order - b->order;
}

static qboolean R_GodrayEmitters_AddSurface (const qmodel_t *model, const msurface_t *surf,
	const shader_material_t *material, const vec3_t base_color, const vec3_t base_dir,
	float ray_length, float density, float intensity, const float matrix[16], qboolean transform_surface)
{
	vec3_t world_mins;
	vec3_t world_maxs;
	vec3_t normal;
	float dist;
	vec3_t plane_point;
	vec3_t center;
	vec3_t center_on_plane;
	vec3_t axis_t;
	vec3_t axis_b;
	vec3_t axis_r;
	vec3_t ray_dir;
	vec3_t corners[8];
	float min_u = 0.f, max_u = 0.f;
	float min_v = 0.f, max_v = 0.f;
	float scale = material ? material->godray_scale : 1.f;

	r_godray_emitter_stats.found++;

	if (r_godray_emitter_count >= MAX_GODRAY_EMITTERS)
		return false;

	if (transform_surface)
		R_GodrayEmitters_TransformAABB (matrix, surf->mins, surf->maxs, world_mins, world_maxs);
	else
	{
		VectorCopy (surf->mins, world_mins);
		VectorCopy (surf->maxs, world_maxs);
	}

	if (R_CullBox (world_mins, world_maxs))
	{
		r_godray_emitter_stats.culled++;
		return true;
	}

	VectorCopy (surf->plane->normal, normal);
	dist = surf->plane->dist;
	if (surf->flags & SURF_PLANEBACK)
	{
		VectorScale (normal, -1.f, normal);
		dist = -dist;
	}

	if (transform_surface)
	{
		vec3_t transformed_normal;
		R_GodrayEmitters_TransformNormal (matrix, normal, transformed_normal);
		VectorNormalize (transformed_normal);
		VectorCopy (transformed_normal, normal);

		VectorScale (surf->plane->normal, surf->plane->dist, plane_point);
		R_GodrayEmitters_TransformPoint (matrix, plane_point, plane_point);
		dist = DotProduct (normal, plane_point);
	}

	if (surf->texinfo)
	{
		VectorCopy (surf->texinfo->vecs[0], axis_t);
		VectorCopy (surf->texinfo->vecs[1], axis_b);
		if (transform_surface)
		{
			vec3_t transformed_axis_t;
			vec3_t transformed_axis_b;
			R_GodrayEmitters_TransformVector (matrix, axis_t, transformed_axis_t);
			R_GodrayEmitters_TransformVector (matrix, axis_b, transformed_axis_b);
			VectorCopy (transformed_axis_t, axis_t);
			VectorCopy (transformed_axis_b, axis_b);
		}
	}
	else
	{
		PerpendicularVector (axis_t, normal);
		CrossProduct (normal, axis_t, axis_b);
	}

	VectorMA (axis_t, -DotProduct (axis_t, normal), normal, axis_t);
	if (VectorNormalize (axis_t) < 0.001f)
	{
		PerpendicularVector (axis_t, normal);
		VectorNormalize (axis_t);
	}
	VectorMA (axis_b, -DotProduct (axis_b, normal), normal, axis_b);
	VectorMA (axis_b, -DotProduct (axis_b, axis_t), axis_t, axis_b);
	if (VectorNormalize (axis_b) < 0.001f || fabsf (DotProduct (axis_b, axis_t)) > 0.95f)
	{
		CrossProduct (normal, axis_t, axis_b);
		VectorNormalize (axis_b);
	}

	VectorAdd (world_mins, world_maxs, center);
	VectorScale (center, 0.5f, center);
	VectorMA (center, -(DotProduct (center, normal) - dist), normal, center_on_plane);

	VectorCopy (base_dir, ray_dir);
	if (VectorLength (ray_dir) > 0.001f)
	{
		float proj = DotProduct (ray_dir, normal);
		VectorMA (ray_dir, -proj, normal, ray_dir);
	}
	if (VectorLength (ray_dir) < 0.001f)
		VectorCopy (normal, ray_dir);
	VectorNormalize (ray_dir);

	VectorCopy (base_dir, axis_r);
	if (VectorLength (axis_r) < 0.001f)
		VectorCopy (normal, axis_r);
	VectorNormalize (axis_r);
	if (fabsf (DotProduct (axis_r, normal)) < 0.1f)
		VectorCopy (normal, axis_r);
	if (DotProduct (axis_r, normal) < 0.f)
		VectorScale (axis_r, -1.f, axis_r);

	corners[0][0] = world_mins[0]; corners[0][1] = world_mins[1]; corners[0][2] = world_mins[2];
	corners[1][0] = world_maxs[0]; corners[1][1] = world_mins[1]; corners[1][2] = world_mins[2];
	corners[2][0] = world_maxs[0]; corners[2][1] = world_maxs[1]; corners[2][2] = world_mins[2];
	corners[3][0] = world_mins[0]; corners[3][1] = world_maxs[1]; corners[3][2] = world_mins[2];
	corners[4][0] = world_mins[0]; corners[4][1] = world_mins[1]; corners[4][2] = world_maxs[2];
	corners[5][0] = world_maxs[0]; corners[5][1] = world_mins[1]; corners[5][2] = world_maxs[2];
	corners[6][0] = world_maxs[0]; corners[6][1] = world_maxs[1]; corners[6][2] = world_maxs[2];
	corners[7][0] = world_mins[0]; corners[7][1] = world_maxs[1]; corners[7][2] = world_maxs[2];

	for (int i = 0; i < 8; ++i)
	{
		vec3_t delta;
		float u, v;
		VectorSubtract (corners[i], center_on_plane, delta);
		u = DotProduct (delta, axis_t);
		v = DotProduct (delta, axis_b);
		if (i == 0)
		{
			min_u = max_u = u;
			min_v = max_v = v;
		}
		else
		{
			min_u = q_min (min_u, u);
			max_u = q_max (max_u, u);
			min_v = q_min (min_v, v);
			max_v = q_max (max_v, v);
		}
	}

	godray_emitter_t *emitter = &r_godray_emitters[r_godray_emitter_count++];
	VectorCopy (center_on_plane, emitter->center_ws);
	VectorCopy (normal, emitter->normal_ws);
	VectorCopy (ray_dir, emitter->ray_dir_ws);
	emitter->ray_length = ray_length;
	emitter->density = density * scale;
	emitter->intensity = intensity * scale;
	emitter->flags = 0u;
	VectorCopy (base_color, emitter->color);
	VectorScale (emitter->color, scale, emitter->color);
	VectorCopy (axis_t, emitter->axis_t);
	VectorCopy (axis_b, emitter->axis_b);
	VectorCopy (axis_r, emitter->axis_r);
	emitter->mins[0] = min_u;
	emitter->mins[1] = min_v;
	emitter->mins[2] = 0.f;
	emitter->maxs[0] = max_u;
	emitter->maxs[1] = max_v;
	emitter->maxs[2] = ray_length;

	return true;
}

static void R_GodrayEmitters_AddEntity (entity_t *ent, const vec3_t base_color, const vec3_t base_dir,
	float ray_length, float density, float intensity)
{
	qmodel_t *model;
	float matrix[16];
	qboolean transform_surface;
	int start;
	int end;

	if (!ent || !Mod_IsKnownModel (ent->model))
		return;
	model = ent->model;
	if (!model || model->type != mod_brush)
		return;

	transform_surface = (ent != &cl_entities[0]) || ent->angles[0] || ent->angles[1] || ent->angles[2]
		|| ent->origin[0] || ent->origin[1] || ent->origin[2] || ent->scale != ENTSCALE_ENCODE (1.0f);
	if (transform_surface)
	{
		vec3_t matrix_angles;
		VectorCopy (ent->angles, matrix_angles);
		R_EntityMatrix (matrix, ent->origin, matrix_angles, ent->scale);
	}

	start = model->firstmodelsurface;
	end = start + model->nummodelsurfaces;
	if (start < 0 || end > model->numsurfaces || model->nummodelsurfaces <= 0)
	{
		start = 0;
		end = model->numsurfaces;
	}

	for (int i = start; i < end; ++i)
	{
		msurface_t *surf = &model->surfaces[i];
		const shader_material_t *material = NULL;
		vec3_t color;
		vec3_t dir;
		float scale = 1.f;
		float surface_length = ray_length;
		float surface_intensity = intensity;

		if (!R_GodrayEmitters_SurfaceMaterial (model, surf, &material))
			continue;

		if (material)
		{
			scale = material->godray_scale;
			surface_length = material->godray_length_set ? material->godray_length : ray_length;
			surface_intensity = material->godray_intensity_set ? material->godray_intensity : intensity;
			if (material->godray_color_set)
				VectorCopy (material->godray_color, color);
			else
				VectorCopy (base_color, color);
			if (material->godray_dir_set)
				VectorCopy (material->godray_dir, dir);
			else
				VectorCopy (base_dir, dir);
			VectorNormalize (dir);
			scale = q_max (0.f, scale);
		}
		else
		{
			VectorCopy (base_color, color);
			VectorCopy (base_dir, dir);
		}

		if (surface_length <= 0.1f || density <= 0.f || surface_intensity <= 0.f || scale <= 0.f)
			continue;

		if (!R_GodrayEmitters_AddSurface (model, surf, material, color, dir, surface_length, density,
			surface_intensity, matrix, transform_surface))
			return;
	}
}

void R_GodrayEmitters_Init (void)
{
	Cvar_RegisterVariable (&r_godrays_mode);
	Cvar_RegisterVariable (&r_godrays_max_emitters);
	Cvar_RegisterVariable (&r_godrays_hero_emitters);
	Cvar_RegisterVariable (&r_godray_volume_length);
	Cvar_RegisterVariable (&r_godray_volume_density);
	Cvar_RegisterVariable (&r_godray_volume_intensity);
	Cvar_RegisterVariable (&r_godray_volume_color);
	Cvar_RegisterVariable (&r_godray_volume_dir);
}

void R_GodrayEmitters_BuildList (void)
{
	vec3_t color;
	vec3_t dir;
	float ray_length;
	float density;
	float intensity;
	entity_t **ents;
	int count;

	R_GodrayEmitters_Clear ();
	if (r_shaders.value <= 0.f)
		return;

	R_GodrayEmitters_ParseColor (r_godray_volume_color.string, color);
	if (!R_GodrayEmitters_ParseVector (r_godray_volume_dir.string, dir))
	{
		dir[0] = 0.f;
		dir[1] = -1.f;
		dir[2] = 0.f;
	}

	ray_length = q_max (0.f, r_godray_volume_length.value);
	density = q_max (0.f, r_godray_volume_density.value);
	intensity = q_max (0.f, r_godray_volume_intensity.value);
	if (ray_length <= 0.1f || density <= 0.f || intensity <= 0.f)
		return;

	ents = R_GetVisEntities (mod_brush, false, &count);
	for (int i = 0; i < count; ++i)
		R_GodrayEmitters_AddEntity (ents[i], color, dir, ray_length, density, intensity);

	ents = R_GetVisEntities (mod_brush, true, &count);
	for (int i = 0; i < count; ++i)
		R_GodrayEmitters_AddEntity (ents[i], color, dir, ray_length, density, intensity);

	if (r_godray_emitter_count > 1)
	{
		godray_emitter_sort_t sorted[MAX_GODRAY_EMITTERS];
		godray_emitter_t temp[MAX_GODRAY_EMITTERS];
		for (int i = 0; i < r_godray_emitter_count; ++i)
		{
			sorted[i].score = R_GodrayEmitters_ComputeScore (&r_godray_emitters[i]);
			sorted[i].index = i;
			sorted[i].order = i;
		}
		qsort (sorted, r_godray_emitter_count, sizeof (sorted[0]), R_GodrayEmitters_CompareScores);
		for (int i = 0; i < r_godray_emitter_count; ++i)
			temp[i] = r_godray_emitters[sorted[i].index];
		for (int i = 0; i < r_godray_emitter_count; ++i)
			r_godray_emitters[i] = temp[i];
	}

	int max_emitters = (int)Q_rint (r_godrays_max_emitters.value);
	max_emitters = CLAMP (1, max_emitters, MAX_GODRAY_EMITTERS);
	if (r_godray_emitter_count > max_emitters)
	{
		r_godray_emitter_stats.clamped = r_godray_emitter_count - max_emitters;
		r_godray_emitter_count = max_emitters;
	}
	r_godray_emitter_stats.total = r_godray_emitter_count;
}

const godray_emitter_t *R_GodrayEmitters_GetList (int *out_count)
{
	if (out_count)
		*out_count = r_godray_emitter_count;
	return r_godray_emitters;
}

const godray_emitter_stats_t *R_GodrayEmitters_GetStats (void)
{
	return &r_godray_emitter_stats;
}

void R_GodrayEmitters_GetRenderCounts (int *out_volume_count, int *out_postfx_count)
{
	int total = r_godray_emitter_count;
	int mode = (int)Q_rint (r_godrays_mode.value);
	int hero = (int)Q_rint (r_godrays_hero_emitters.value);
	mode = CLAMP (0, mode, 2);
	hero = CLAMP (0, hero, total);

	if (out_volume_count)
		*out_volume_count = 0;
	if (out_postfx_count)
		*out_postfx_count = 0;

	if (mode == 1)
	{
		if (out_volume_count)
			*out_volume_count = total;
		return;
	}

	if (mode == 0)
	{
		if (out_postfx_count)
			*out_postfx_count = q_min (total, MAX_GODRAY_POSTFX_EMITTERS);
		return;
	}

	if (out_volume_count)
		*out_volume_count = hero;
	if (out_postfx_count)
		*out_postfx_count = q_min (total - hero, MAX_GODRAY_POSTFX_EMITTERS);
}

void R_GodrayEmitters_SetRenderedCounts (int volume_count, int postfx_count)
{
	r_godray_emitter_stats.rendered_volume = volume_count;
	r_godray_emitter_stats.rendered_postfx = postfx_count;
}
