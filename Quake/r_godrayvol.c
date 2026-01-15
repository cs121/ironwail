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
#include "r_godrayvol.h"

#define MAX_GODRAY_VOLUMES 256

typedef struct godray_volume_s
{
	vec3_t verts[8];
	vec3_t origin;
	vec3_t axis_t;
	vec3_t axis_b;
	vec3_t axis_r;
	vec3_t ray_dir;
	vec3_t mins;
	vec3_t maxs;
	vec4_t color_density;
	vec4_t misc;
} godray_volume_t;

typedef struct godray_volume_vertex_s
{
	vec3_t pos;
} godray_volume_vertex_t;

static godray_volume_t r_godray_volumes[MAX_GODRAY_VOLUMES];
static int r_godray_volume_count = 0;

static cvar_t r_godray_volumes_enable = { "r_godray_volumes", "0", CVAR_ARCHIVE };
static cvar_t r_godray_volume_length = { "r_godray_volume_length", "256", CVAR_ARCHIVE };
static cvar_t r_godray_volume_density = { "r_godray_volume_density", "0.4", CVAR_ARCHIVE };
static cvar_t r_godray_volume_steps = { "r_godray_volume_steps", "24", CVAR_ARCHIVE };
static cvar_t r_godray_volume_noise_scale = { "r_godray_volume_noise_scale", "0.03", CVAR_ARCHIVE };
static cvar_t r_godray_volume_noise_amount = { "r_godray_volume_noise_amount", "0.6", CVAR_ARCHIVE };
static cvar_t r_godray_volume_intensity = { "r_godray_volume_intensity", "1.0", CVAR_ARCHIVE };
static cvar_t r_godray_volume_color = { "r_godray_volume_color", "1 1 1", CVAR_ARCHIVE };
static cvar_t r_godray_volume_dir = { "r_godray_volume_dir", "0 -1 0", CVAR_ARCHIVE };
static cvar_t r_godray_volume_debug = { "r_godray_volume_debug", "0", CVAR_ARCHIVE };

static void R_GodrayVolume_ParseColor (const char *value, vec3_t color)
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

static qboolean R_GodrayVolume_ParseVector (const char *value, vec3_t out)
{
	return value && sscanf (value, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

static void R_GodrayVolume_Clear (void)
{
	r_godray_volume_count = 0;
}

static void R_GodrayVolume_TransformPoint (const float matrix[16], const vec3_t in, vec3_t out)
{
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2] + matrix[12];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2] + matrix[13];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2] + matrix[14];
}

static void R_GodrayVolume_TransformVector (const float matrix[16], const vec3_t in, vec3_t out)
{
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2];
}

static qboolean R_GodrayVolume_Invert3x3 (const float matrix[16], float out[9])
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

static void R_GodrayVolume_TransformNormal (const float matrix[16], const vec3_t in, vec3_t out)
{
	float inv[9];

	if (R_GodrayVolume_Invert3x3 (matrix, inv))
	{
		out[0] = inv[0] * in[0] + inv[3] * in[1] + inv[6] * in[2];
		out[1] = inv[1] * in[0] + inv[4] * in[1] + inv[7] * in[2];
		out[2] = inv[2] * in[0] + inv[5] * in[1] + inv[8] * in[2];
		return;
	}

	R_GodrayVolume_TransformVector (matrix, in, out);
}

static void R_GodrayVolume_TransformAABB (const float matrix[16], const vec3_t mins, const vec3_t maxs,
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
		R_GodrayVolume_TransformPoint (matrix, corners[i], transformed);
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

static qboolean R_GodrayVolume_SurfaceMaterial (const qmodel_t *model, const msurface_t *surf,
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

static qboolean R_GodrayVolume_AddSurface (const qmodel_t *model, const msurface_t *surf,
	const shader_material_t *material, const vec3_t base_color, const vec3_t base_dir,
	float ray_length, float density, float intensity, float noise_scale, float noise_amount,
	const float matrix[16], qboolean transform_surface)
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
	vec3_t up;
	vec3_t corners[8];
	float min_u = 0.f, max_u = 0.f;
	float min_v = 0.f, max_v = 0.f;
	vec3_t base_corners[4];
	float scale = material ? material->godray_scale : 1.f;

	if (r_godray_volume_count >= MAX_GODRAY_VOLUMES)
		return false;

	if (transform_surface)
		R_GodrayVolume_TransformAABB (matrix, surf->mins, surf->maxs, world_mins, world_maxs);
	else
	{
		VectorCopy (surf->mins, world_mins);
		VectorCopy (surf->maxs, world_maxs);
	}

	if (R_CullBox (world_mins, world_maxs))
		return true;

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
		R_GodrayVolume_TransformNormal (matrix, normal, transformed_normal);
		VectorNormalize (transformed_normal);
		VectorCopy (transformed_normal, normal);

		VectorScale (surf->plane->normal, surf->plane->dist, plane_point);
		R_GodrayVolume_TransformPoint (matrix, plane_point, plane_point);
		dist = DotProduct (normal, plane_point);
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

	if (fabsf (normal[2]) < 0.95f)
	{
		up[0] = 0.f;
		up[1] = 0.f;
		up[2] = 1.f;
	}
	else
	{
		up[0] = 0.f;
		up[1] = 1.f;
		up[2] = 0.f;
	}
	CrossProduct (up, normal, axis_t);
	VectorNormalize (axis_t);
	CrossProduct (normal, axis_t, axis_b);
	VectorNormalize (axis_b);

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

	VectorMA (center_on_plane, min_u, axis_t, base_corners[0]);
	VectorMA (base_corners[0], min_v, axis_b, base_corners[0]);
	VectorMA (center_on_plane, max_u, axis_t, base_corners[1]);
	VectorMA (base_corners[1], min_v, axis_b, base_corners[1]);
	VectorMA (center_on_plane, max_u, axis_t, base_corners[2]);
	VectorMA (base_corners[2], max_v, axis_b, base_corners[2]);
	VectorMA (center_on_plane, min_u, axis_t, base_corners[3]);
	VectorMA (base_corners[3], max_v, axis_b, base_corners[3]);

	godray_volume_t *volume = &r_godray_volumes[r_godray_volume_count++];

	VectorCopy (base_corners[0], volume->verts[0]);
	VectorCopy (base_corners[1], volume->verts[1]);
	VectorCopy (base_corners[2], volume->verts[2]);
	VectorCopy (base_corners[3], volume->verts[3]);
	for (int i = 0; i < 4; ++i)
	{
		VectorMA (base_corners[i], ray_length, axis_r, volume->verts[i + 4]);
	}

	VectorCopy (center_on_plane, volume->origin);
	VectorCopy (axis_t, volume->axis_t);
	VectorCopy (axis_b, volume->axis_b);
	VectorCopy (axis_r, volume->axis_r);
	VectorCopy (ray_dir, volume->ray_dir);
	volume->mins[0] = min_u;
	volume->mins[1] = min_v;
	volume->mins[2] = 0.f;
	volume->maxs[0] = max_u;
	volume->maxs[1] = max_v;
	volume->maxs[2] = ray_length;
	volume->color_density[0] = base_color[0] * scale;
	volume->color_density[1] = base_color[1] * scale;
	volume->color_density[2] = base_color[2] * scale;
	volume->color_density[3] = density * scale;
	volume->misc[0] = noise_scale;
	volume->misc[1] = noise_amount;
	volume->misc[2] = intensity;
	volume->misc[3] = 0.f;

	return true;
}

static void R_GodrayVolume_AddEntity (entity_t *ent, const vec3_t base_color, const vec3_t base_dir,
	float ray_length, float density, float intensity, float noise_scale, float noise_amount)
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

		if (!R_GodrayVolume_SurfaceMaterial (model, surf, &material))
			continue;

		if (!R_GodrayVolume_AddSurface (model, surf, material, base_color, base_dir,
			ray_length, density, intensity, noise_scale, noise_amount, matrix, transform_surface))
			return;
	}
}

void R_GodrayVolume_Init (void)
{
	Cvar_RegisterVariable (&r_godray_volumes_enable);
	Cvar_RegisterVariable (&r_godray_volume_length);
	Cvar_RegisterVariable (&r_godray_volume_density);
	Cvar_RegisterVariable (&r_godray_volume_steps);
	Cvar_RegisterVariable (&r_godray_volume_noise_scale);
	Cvar_RegisterVariable (&r_godray_volume_noise_amount);
	Cvar_RegisterVariable (&r_godray_volume_intensity);
	Cvar_RegisterVariable (&r_godray_volume_color);
	Cvar_RegisterVariable (&r_godray_volume_dir);
	Cvar_RegisterVariable (&r_godray_volume_debug);
}

void R_GodrayVolume_BuildList (void)
{
	vec3_t color;
	vec3_t dir;
	float ray_length;
	float density;
	float intensity;
	float noise_scale;
	float noise_amount;
	entity_t **ents;
	int count;

	R_GodrayVolume_Clear ();
	if (r_godray_volumes_enable.value <= 0.f)
		return;
	if (r_shaders.value <= 0.f)
		return;

	R_GodrayVolume_ParseColor (r_godray_volume_color.string, color);
	if (!R_GodrayVolume_ParseVector (r_godray_volume_dir.string, dir))
	{
		dir[0] = 0.f;
		dir[1] = -1.f;
		dir[2] = 0.f;
	}

	ray_length = q_max (0.f, r_godray_volume_length.value);
	density = q_max (0.f, r_godray_volume_density.value);
	intensity = q_max (0.f, r_godray_volume_intensity.value);
	noise_scale = q_max (0.0001f, r_godray_volume_noise_scale.value);
	noise_amount = CLAMP (0.f, r_godray_volume_noise_amount.value, 1.f);

	if (ray_length <= 0.1f || density <= 0.f || intensity <= 0.f)
		return;

	ents = R_GetVisEntities (mod_brush, false, &count);
	for (int i = 0; i < count; ++i)
		R_GodrayVolume_AddEntity (ents[i], color, dir, ray_length, density, intensity, noise_scale, noise_amount);

	ents = R_GetVisEntities (mod_brush, true, &count);
	for (int i = 0; i < count; ++i)
		R_GodrayVolume_AddEntity (ents[i], color, dir, ray_length, density, intensity, noise_scale, noise_amount);
}

void R_GodrayVolume_Render (void)
{
	static const GLushort box_indices[36] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		0, 1, 5, 0, 5, 4,
		1, 2, 6, 1, 6, 5,
		2, 3, 7, 2, 7, 6,
		3, 0, 4, 3, 4, 7
	};
	GLuint buf;
	GLbyte *ofs;

	if (r_godray_volumes_enable.value <= 0.f)
		return;
	if (!glprogs.godray_volume)
		return;
	if (r_godray_volume_count <= 0)
		return;

	GL_BeginGroup ("Godray volumes");
	GL_UseProgram (glprogs.godray_volume);
	GL_SetState (GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (1));

	int steps = (int)Q_rint (r_godray_volume_steps.value);
	steps = CLAMP (8, steps, 64);
	GL_Uniform1iFunc (8, steps);

	for (int i = 0; i < r_godray_volume_count; ++i)
	{
		const godray_volume_t *volume = &r_godray_volumes[i];
		godray_volume_vertex_t verts[8];
		vec3_t origin_vs;
		vec3_t axis_t_vs;
		vec3_t axis_b_vs;
		vec3_t axis_r_vs;
		vec3_t ray_dir_vs;

		for (int v = 0; v < 8; ++v)
			VectorCopy (volume->verts[v], verts[v].pos);

		R_GodrayVolume_TransformPoint (r_matview, volume->origin, origin_vs);
		R_GodrayVolume_TransformVector (r_matview, volume->axis_t, axis_t_vs);
		R_GodrayVolume_TransformVector (r_matview, volume->axis_b, axis_b_vs);
		R_GodrayVolume_TransformVector (r_matview, volume->axis_r, axis_r_vs);
		R_GodrayVolume_TransformVector (r_matview, volume->ray_dir, ray_dir_vs);
		VectorNormalize (axis_t_vs);
		VectorNormalize (axis_b_vs);
		VectorNormalize (axis_r_vs);
		VectorNormalize (ray_dir_vs);

		GL_Uniform3fFunc (0, origin_vs[0], origin_vs[1], origin_vs[2]);
		GL_Uniform3fFunc (1, axis_t_vs[0], axis_t_vs[1], axis_t_vs[2]);
		GL_Uniform3fFunc (2, axis_b_vs[0], axis_b_vs[1], axis_b_vs[2]);
		GL_Uniform3fFunc (3, axis_r_vs[0], axis_r_vs[1], axis_r_vs[2]);
		GL_Uniform3fFunc (4, volume->mins[0], volume->mins[1], volume->mins[2]);
		GL_Uniform3fFunc (5, volume->maxs[0], volume->maxs[1], volume->maxs[2]);
		GL_Uniform4fFunc (6, volume->color_density[0], volume->color_density[1],
			volume->color_density[2], volume->color_density[3]);
		GL_Uniform4fFunc (7, volume->misc[0], volume->misc[1], volume->misc[2], volume->misc[3]);
		GL_Uniform3fFunc (9, ray_dir_vs[0], ray_dir_vs[1], ray_dir_vs[2]);
		GL_Uniform1fFunc (10, r_godray_volume_debug.value);

		GL_Upload (GL_ARRAY_BUFFER, verts, sizeof (verts), &buf, &ofs);
		GL_BindBuffer (GL_ARRAY_BUFFER, buf);
		GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (verts[0]), ofs + offsetof (godray_volume_vertex_t, pos));

		GL_Upload (GL_ELEMENT_ARRAY_BUFFER, box_indices, sizeof (box_indices), &buf, &ofs);
		GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
		glDrawElements (GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, ofs);
	}

	GL_EndGroup ();
}
