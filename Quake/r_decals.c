/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#include "quakedef.h"

typedef enum
{
	DECAL_BLEND_ALPHA,
	DECAL_BLEND_ADD,
	DECAL_BLEND_MUL
} decalblend_t;

typedef struct
{
	char name[64];
	char category[32];
	char texture_path[MAX_QPATH];
	gltexture_t *texture;
	float size_min, size_max;
	float alpha_min, alpha_max;
	vec3_t color;
	float lifetime;
	float fade;
	int random_rotation;
	int priority;
	decalblend_t blend;
	qboolean valid;
} decaldef_t;

typedef struct
{
	vec3_t pos;
	float uv[2];
	byte color[4];
} decalvert_t;

typedef struct
{
	qboolean active;
	int def_index;
	double spawn_time;
	double die_time;
	int priority;
	decalblend_t blend;
	gltexture_t *texture;
	int first_vert;
	int num_verts;
} decalinst_t;

#define MAX_DECAL_DEFS 128
#define MAX_DECAL_INSTANCES 1024
#define MAX_DECAL_VERTS 24576
#define MAX_DECAL_INDEXES (MAX_DECAL_VERTS * 3)
#define MAX_POLY_VERTS 64

static decaldef_t decal_defs[MAX_DECAL_DEFS];
static int num_decal_defs;

static decalinst_t decal_instances[MAX_DECAL_INSTANCES];
static decalvert_t decal_verts[MAX_DECAL_VERTS];
static decalvert_t decal_draw_verts[MAX_DECAL_VERTS];
static GLushort decal_indexes[MAX_DECAL_INDEXES];
static int decal_vert_cursor;
static int decal_inst_count;

static cvar_t r_decals = {"r_decals", "1", CVAR_ARCHIVE};
static cvar_t r_decals_max = {"r_decals_max", "256", CVAR_ARCHIVE};

static void R_Decals_CompactVerts (void)
{
	int i;
	int write_cursor = 0;

	for (i = 0; i < MAX_DECAL_INSTANCES; ++i)
	{
		decalinst_t *inst = &decal_instances[i];

		if (!inst->active || inst->num_verts <= 0)
			continue;

		if (inst->first_vert != write_cursor)
			memmove (&decal_verts[write_cursor], &decal_verts[inst->first_vert], sizeof (decalvert_t) * inst->num_verts);

		inst->first_vert = write_cursor;
		write_cursor += inst->num_verts;
	}

	decal_vert_cursor = write_cursor;
}

static void R_Decals_ResetRuntime (void)
{
	memset (decal_instances, 0, sizeof (decal_instances));
	decal_vert_cursor = 0;
	decal_inst_count = 0;
}

static decalblend_t R_DecalParseBlend (const char *s)
{
	if (!q_strcasecmp (s, "add"))
		return DECAL_BLEND_ADD;
	if (!q_strcasecmp (s, "mul"))
		return DECAL_BLEND_MUL;
	return DECAL_BLEND_ALPHA;
}

static qboolean R_DecalLoadTexture (decaldef_t *def)
{
	int w, h;
	enum srcformat fmt;
	byte *data;
	char texname[96];

	data = Image_LoadImage (def->texture_path, &w, &h, &fmt);
	if (!data)
		return false;

	q_snprintf (texname, sizeof (texname), "decal:%s", def->name);
	def->texture = TexMgr_LoadImage (NULL, texname, w, h, fmt, data, def->texture_path, 0,
		TEXPREF_ALPHA | TEXPREF_NOPICMIP | TEXPREF_CLAMP);
	return def->texture != NULL;
}

static void R_DecalFinalizeDef (decaldef_t *def)
{
	if (!def->name[0] || !def->texture_path[0] || !def->category[0])
		return;

	if (def->size_max < def->size_min)
		def->size_max = def->size_min;
	if (def->alpha_max < def->alpha_min)
		def->alpha_max = def->alpha_min;
	if (def->lifetime <= 0.f)
		def->lifetime = 10.f;
	if (def->fade < 0.f)
		def->fade = 0.f;
	if (def->fade > def->lifetime)
		def->fade = def->lifetime;

	def->valid = R_DecalLoadTexture (def);
}

static void R_Decals_LoadScript (const char *path)
{
	char *data = (char *) COM_LoadMallocFile (path, NULL);
	const char *c;
	decaldef_t *def = NULL;

	if (!data)
		return;

	c = data;
	while ((c = COM_Parse (c)))
	{
		if (!com_token[0])
			break;

		if (!q_strcasecmp (com_token, "decal"))
		{
			if (!(c = COM_Parse (c)) || !com_token[0] || num_decal_defs >= MAX_DECAL_DEFS)
				break;
			def = &decal_defs[num_decal_defs++];
			memset (def, 0, sizeof (*def));
			q_strlcpy (def->name, com_token, sizeof (def->name));
			VectorSet (def->color, 1.f, 1.f, 1.f);
			def->size_min = def->size_max = 8.f;
			def->alpha_min = def->alpha_max = 1.f;
			def->lifetime = 15.f;
			def->fade = 5.f;
			if (!(c = COM_Parse (c)) || strcmp (com_token, "{"))
				def = NULL;
			continue;
		}

		if (!def)
			continue;

		if (!strcmp (com_token, "}"))
		{
			R_DecalFinalizeDef (def);
			def = NULL;
			continue;
		}

		if (!q_strcasecmp (com_token, "texture"))
		{
			if ((c = COM_ParseEx (c, CPE_ALLOWTRUNC)) && com_token[0])
				q_strlcpy (def->texture_path, com_token, sizeof (def->texture_path));
		}
		else if (!q_strcasecmp (com_token, "size"))
		{
			if ((c = COM_Parse (c))) def->size_min = atof (com_token);
			if ((c = COM_Parse (c))) def->size_max = atof (com_token);
		}
		else if (!q_strcasecmp (com_token, "alpha"))
		{
			if ((c = COM_Parse (c))) def->alpha_min = atof (com_token);
			if ((c = COM_Parse (c))) def->alpha_max = atof (com_token);
		}
		else if (!q_strcasecmp (com_token, "color"))
		{
			if ((c = COM_Parse (c))) def->color[0] = atof (com_token);
			if ((c = COM_Parse (c))) def->color[1] = atof (com_token);
			if ((c = COM_Parse (c))) def->color[2] = atof (com_token);
		}
		else if (!q_strcasecmp (com_token, "lifetime"))
		{
			if ((c = COM_Parse (c))) def->lifetime = atof (com_token);
		}
		else if (!q_strcasecmp (com_token, "fade"))
		{
			if ((c = COM_Parse (c))) def->fade = atof (com_token);
		}
		else if (!q_strcasecmp (com_token, "blend"))
		{
			if ((c = COM_Parse (c))) def->blend = R_DecalParseBlend (com_token);
		}
		else if (!q_strcasecmp (com_token, "random_rotation"))
		{
			if ((c = COM_Parse (c))) def->random_rotation = atoi (com_token) != 0;
		}
		else if (!q_strcasecmp (com_token, "priority"))
		{
			if ((c = COM_Parse (c))) def->priority = atoi (com_token);
		}
		else if (!q_strcasecmp (com_token, "category"))
		{
			if ((c = COM_ParseEx (c, CPE_ALLOWTRUNC)) && com_token[0])
				q_strlcpy (def->category, com_token, sizeof (def->category));
		}
	}

	if (def)
		R_DecalFinalizeDef (def);

	free (data);
}

static void R_Decals_LoadScripts (void)
{
	num_decal_defs = 0;
	memset (decal_defs, 0, sizeof (decal_defs));
	R_Decals_LoadScript ("decals.shader");
	R_Decals_LoadScript ("scripts/decals.shader");
}

static decaldef_t *R_FindDecalDefByCategory (const char *category)
{
	int i;
	for (i = 0; i < num_decal_defs; ++i)
	{
		if (!decal_defs[i].valid)
			continue;
		if (!q_strcasecmp (decal_defs[i].category, category))
			return &decal_defs[i];
	}
	return NULL;
}

static int R_DecalAllocInstance (int priority)
{
	int max_inst = CLAMP (0, (int) r_decals_max.value, MAX_DECAL_INSTANCES);
	int i, pick = -1;

	for (i = 0; i < max_inst; ++i)
	{
		if (!decal_instances[i].active)
			return i;
	}

	for (i = 0; i < max_inst; ++i)
	{
		if (pick < 0 || decal_instances[i].priority < decal_instances[pick].priority ||
			(decal_instances[i].priority == decal_instances[pick].priority && decal_instances[i].spawn_time < decal_instances[pick].spawn_time))
			pick = i;
	}

	if (pick >= 0 && decal_instances[pick].priority <= priority)
	{
		if (decal_instances[pick].active)
		{
			decal_instances[pick].active = false;
			decal_inst_count = q_max (0, decal_inst_count - 1);
		}
		return pick;
	}
	return -1;
}

static int R_GetSurfVertex (const msurface_t *surf, int idx, vec3_t out)
{
	int edgeidx = cl.worldmodel->surfedges[surf->firstedge + idx];
	const medge_t *edge;
	const mvertex_t *vert;

	if (edgeidx >= 0)
	{
		edge = &cl.worldmodel->edges[edgeidx];
		vert = &cl.worldmodel->vertexes[edge->v[0]];
	}
	else
	{
		edge = &cl.worldmodel->edges[-edgeidx];
		vert = &cl.worldmodel->vertexes[edge->v[1]];
	}
	VectorCopy (vert->position, out);
	return 1;
}

static int R_ClipPolyAxis (const decalvert_t *in, int count, decalvert_t *out, int axis, float sign, float limit)
{
	int i, out_count = 0;
	for (i = 0; i < count; ++i)
	{
		const decalvert_t *a = &in[i];
		const decalvert_t *b = &in[(i + 1) % count];
		float da = sign * a->pos[axis] - limit;
		float db = sign * b->pos[axis] - limit;
		qboolean ina = da <= 0.f;
		qboolean inb = db <= 0.f;

		if (ina)
			out[out_count++] = *a;
		if (ina != inb)
		{
			float t = da / (da - db);
			decalvert_t v;
			int k;
			for (k = 0; k < 3; ++k)
				v.pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t;
			out[out_count++] = v;
		}
	}
	return out_count;
}

static void R_BuildSurfaceDecalBasis (const msurface_t *surf, float rotation, vec3_t out_sdir, vec3_t out_tdir, vec3_t out_normal)
{
	vec3_t surf_normal, up = {0.f, 0.f, 1.f};
	float c = cosf (rotation), s = sinf (rotation);
	int i;

	VectorCopy (surf->plane->normal, surf_normal);
	if (surf->flags & SURF_PLANEBACK)
		VectorInverse (surf_normal);
	VectorNormalizeFast (surf_normal);

	for (i = 0; i < 3; ++i)
	{
		out_sdir[i] = surf->texinfo->vecs[0][i];
		out_tdir[i] = surf->texinfo->vecs[1][i];
	}

	if (VectorLengthSquared (out_sdir) < 0.0001f)
	{
		if (fabsf (surf_normal[2]) > 0.95f)
			VectorSet (up, 1.f, 0.f, 0.f);
		CrossProduct (up, surf_normal, out_sdir);
	}
	VectorNormalizeFast (out_sdir);

	CrossProduct (surf_normal, out_sdir, out_tdir);
	if (VectorLengthSquared (out_tdir) < 0.0001f)
	{
		CrossProduct (out_sdir, surf_normal, out_tdir);
	}
	VectorNormalizeFast (out_tdir);

	CrossProduct (out_tdir, surf_normal, out_sdir);
	VectorNormalizeFast (out_sdir);

	if (rotation != 0.f)
	{
		for (i = 0; i < 3; ++i)
		{
			float ns = out_sdir[i] * c + out_tdir[i] * s;
			float nt = out_tdir[i] * c - out_sdir[i] * s;
			out_sdir[i] = ns;
			out_tdir[i] = nt;
		}
	}

	VectorCopy (surf_normal, out_normal);
}

static int R_ProjectDecalToSurface (const msurface_t *surf, const vec3_t origin, float rotation,
	float radius, float alpha, const vec3_t color, int first_vert)
{
	decalvert_t poly0[MAX_POLY_VERTS], poly1[MAX_POLY_VERTS];
	vec3_t sdir, tdir, normal;
	int i, count = surf->numedges;
	int total_added = 0;
	/*
	 * Keep depth clipping conservative now that we reconstruct decal verts with
	 * their normal-space offset to avoid pushing decals through nearby geometry.
	 */
	float depth = q_max (1.f, radius * 0.25f);

	if (count < 3 || count >= MAX_POLY_VERTS)
		return 0;

	R_BuildSurfaceDecalBasis (surf, rotation, sdir, tdir, normal);

	for (i = 0; i < count; ++i)
	{
		vec3_t world;
		R_GetSurfVertex (surf, i, world);
		VectorSubtract (world, origin, world);
		poly0[i].pos[0] = DotProduct (world, sdir);
		poly0[i].pos[1] = DotProduct (world, tdir);
		poly0[i].pos[2] = DotProduct (world, normal);
	}

	count = R_ClipPolyAxis (poly0, count, poly1, 0, 1.f, radius);
	count = R_ClipPolyAxis (poly1, count, poly0, 0, -1.f, radius);
	count = R_ClipPolyAxis (poly0, count, poly1, 1, 1.f, radius);
	count = R_ClipPolyAxis (poly1, count, poly0, 1, -1.f, radius);
	count = R_ClipPolyAxis (poly0, count, poly1, 2, 1.f, depth);
	count = R_ClipPolyAxis (poly1, count, poly0, 2, -1.f, depth);

	if (count < 3)
		return 0;

	for (i = 0; i < count; ++i)
	{
		decalvert_t *v = &decal_verts[first_vert + i];
		VectorCopy (origin, v->pos);
		VectorMA (v->pos, poly0[i].pos[0], sdir, v->pos);
		VectorMA (v->pos, poly0[i].pos[1], tdir, v->pos);
		VectorMA (v->pos, poly0[i].pos[2], normal, v->pos);
		v->uv[0] = 0.5f + poly0[i].pos[0] / (2.f * radius);
		v->uv[1] = 0.5f + poly0[i].pos[1] / (2.f * radius);
		v->color[0] = (byte) (CLAMP (0.f, color[0], 1.f) * 255.f);
		v->color[1] = (byte) (CLAMP (0.f, color[1], 1.f) * 255.f);
		v->color[2] = (byte) (CLAMP (0.f, color[2], 1.f) * 255.f);
		v->color[3] = (byte) (CLAMP (0.f, alpha, 1.f) * 255.f);
	}

	total_added = count;
	return total_added;
}

void R_SpawnImpactDecal (const char *category, const vec3_t origin, const vec3_t normal)
{
	decaldef_t *def;
	mleaf_t *leaf;
	vec3_t n;
	float radius, alpha, rot;
	int i, inst_idx, first_vert;
	decalinst_t *inst;

	if (!r_decals.value || !cl.worldmodel || !category)
		return;

	def = R_FindDecalDefByCategory (category);
	if (!def)
		return;

	inst_idx = R_DecalAllocInstance (def->priority);
	if (inst_idx < 0)
		return;

	VectorCopy (normal, n);
	if (VectorLengthSquared (n) < 0.0001f)
		return;
	VectorNormalizeFast (n);
	rot = 0.f;

	if (def->random_rotation)
		rot = ((float) rand () / (float) RAND_MAX) * (2.f * M_PI);

	radius = def->size_min + ((float) rand () / (float) RAND_MAX) * (def->size_max - def->size_min);
	alpha = def->alpha_min + ((float) rand () / (float) RAND_MAX) * (def->alpha_max - def->alpha_min);

	{ vec3_t point; VectorCopy (origin, point); leaf = Mod_PointInLeaf (point, cl.worldmodel); }
	if (!leaf || !leaf->nummarksurfaces)
		return;

	first_vert = decal_vert_cursor;
	if (first_vert + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
	{
		R_Decals_CompactVerts ();
		first_vert = decal_vert_cursor;
		if (first_vert + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
			return;
	}

	for (i = 0; i < leaf->nummarksurfaces; ++i)
	{
		msurface_t *surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[i]];
		int flags = SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWSPRITE | SURF_DRAWLAVA | SURF_DRAWSLIME | SURF_DRAWWATER | SURF_DRAWTELE;
		vec3_t surf_normal;
		float d;
		int added;

		if (surf->flags & flags)
			continue;

		VectorCopy (surf->plane->normal, surf_normal);
		if (surf->flags & SURF_PLANEBACK)
			VectorInverse (surf_normal);
		d = fabsf (DotProduct (origin, surf_normal) - (surf->flags & SURF_PLANEBACK ? -surf->plane->dist : surf->plane->dist));
		if (d > radius + 2.f)
			continue;

		if (decal_vert_cursor + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
			break;

		added = R_ProjectDecalToSurface (surf, origin, rot, radius, alpha, def->color, decal_vert_cursor);
		decal_vert_cursor += added;
	}

	if (decal_vert_cursor == first_vert)
		return;

	inst = &decal_instances[inst_idx];
	inst->active = true;
	inst->def_index = (int)(def - decal_defs);
	inst->spawn_time = cl.time;
	inst->die_time = cl.time + def->lifetime;
	inst->priority = def->priority;
	inst->blend = def->blend;
	inst->texture = def->texture;
	inst->first_vert = first_vert;
	inst->num_verts = decal_vert_cursor - first_vert;
	decal_inst_count++;
}

void R_InitDecals (void)
{
	Cvar_RegisterVariable (&r_decals);
	Cvar_RegisterVariable (&r_decals_max);
	R_Decals_LoadScripts ();
	R_Decals_ResetRuntime ();
}

void R_ClearDecals (void)
{
	R_Decals_ResetRuntime ();
}

void R_ReloadDecals (void)
{
	R_Decals_LoadScripts ();
	R_Decals_ResetRuntime ();
}

void R_UpdateDecals (void)
{
	int i;
	for (i = 0; i < MAX_DECAL_INSTANCES; ++i)
	{
		if (!decal_instances[i].active)
			continue;
		if (decal_instances[i].die_time <= cl.time)
		{
			decal_instances[i].active = false;
			decal_inst_count = q_max (0, decal_inst_count - 1);
		}
	}

	if (decal_vert_cursor + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
		R_Decals_CompactVerts ();
}

static int R_DecalSortCmp (const void *a, const void *b)
{
	const decalinst_t *ia = *(const decalinst_t * const *)a;
	const decalinst_t *ib = *(const decalinst_t * const *)b;
	if (ia->blend != ib->blend)
		return ia->blend - ib->blend;
	if (ia->texture != ib->texture)
		return ia->texture < ib->texture ? -1 : 1;
	return ia->priority - ib->priority;
}

static float R_DecalFadeAlpha (const decalinst_t *inst, const decaldef_t *def)
{
	if (def->fade <= 0.f)
		return 1.f;

	if (cl.time <= inst->die_time - def->fade)
		return 1.f;

	return CLAMP (0.f, (float)((inst->die_time - cl.time) / def->fade), 1.f);
}

void R_DrawDecals (void)
{
	decalinst_t *draw[MAX_DECAL_INSTANCES];
	int draw_count = 0;
	int i;

	if (!r_decals.value)
		return;

	for (i = 0; i < MAX_DECAL_INSTANCES; ++i)
	{
		decalinst_t *inst = &decal_instances[i];
		decaldef_t *def;

		if (!inst->active || !inst->texture)
			continue;
		def = &decal_defs[inst->def_index];
		draw[draw_count++] = inst;
	}

	if (!draw_count)
		return;

	qsort (draw, draw_count, sizeof (draw[0]), R_DecalSortCmp);

	GL_BeginGroup ("Decals");
	GL_UseProgram (glprogs.decal);
	GL_PolygonOffset (OFFSET_DECAL);

	for (i = 0; i < draw_count; ++i)
	{
		GLuint vbo, ibo;
		GLbyte *ofs;
		int vcount = draw[i]->num_verts;
		int j, icount = 0;
		float fade_alpha = R_DecalFadeAlpha (draw[i], &decal_defs[draw[i]->def_index]);
		unsigned blendstate = GLS_BLEND_ALPHA;

		if (draw[i]->blend == DECAL_BLEND_ADD)
			blendstate = GLS_BLEND_ADD;
		else if (draw[i]->blend == DECAL_BLEND_MUL)
			blendstate = GLS_BLEND_MULTIPLY;

		GL_SetState (blendstate | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (3));
		GL_Bind (GL_TEXTURE0, draw[i]->texture);

		for (j = 2; j < vcount; ++j)
		{
			decal_indexes[icount++] = 0;
			decal_indexes[icount++] = (GLushort)(j - 1);
			decal_indexes[icount++] = (GLushort)j;
		}

		for (j = 0; j < vcount; ++j)
		{
			const decalvert_t *src = &decal_verts[draw[i]->first_vert + j];
			decalvert_t *dst = &decal_draw_verts[draw[i]->first_vert + j];
			*dst = *src;
			dst->color[3] = (byte) (src->color[3] * fade_alpha);
		}

		GL_Upload (GL_ARRAY_BUFFER, &decal_draw_verts[draw[i]->first_vert], sizeof (decalvert_t) * vcount, &vbo, &ofs);
		GL_BindBuffer (GL_ARRAY_BUFFER, vbo);
		GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (decalvert_t), ofs + offsetof (decalvert_t, pos));
		GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof (decalvert_t), ofs + offsetof (decalvert_t, uv));
		GL_VertexAttribPointerFunc (2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (decalvert_t), ofs + offsetof (decalvert_t, color));
		GL_Upload (GL_ELEMENT_ARRAY_BUFFER, decal_indexes, sizeof (GLushort) * icount, &ibo, &ofs);
		GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, ibo);
		glDrawElements (GL_TRIANGLES, icount, GL_UNSIGNED_SHORT, ofs);
	}

	GL_PolygonOffset (OFFSET_NONE);
	GL_EndGroup ();
}
