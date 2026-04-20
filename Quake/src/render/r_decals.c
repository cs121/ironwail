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
#include "glquake.h"
#include "r_framegraph.h"

typedef enum
{
	DECAL_BLEND_ALPHA,
	DECAL_BLEND_ADD,
	DECAL_BLEND_MUL
} decalblend_t;

typedef enum
{
	DECAL_BLOOD_IMPACT,
	DECAL_BLOOD_STREAK,
	DECAL_BLOOD_POOL,
	DECAL_BLOOD_COUNT
} decalbloodtype_t;

typedef struct
{
	char name[64];
	char category[32];
	char texture_path[MAX_QPATH];
	gltexture_t *texture;
	float atlas_u0, atlas_v0;
	float atlas_u1, atlas_v1;
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
	float retention_score;
	decalblend_t blend;
	gltexture_t *texture;
	int first_vert;
	int num_verts;
	vec3_t center;
	float cull_radius;
} decalinst_t;

typedef struct
{
	int first_index;
	int index_count;
	decalblend_t blend;
	gltexture_t *texture;
} decaldrawcmd_t;

typedef struct
{
	float pos[3];
	float u;
	float v;
	uint32_t color;
	uint32_t pad0;
	uint32_t pad1;
} decalgpuvert_t;

typedef struct
{
	uint32_t first_vert;
	uint32_t num_verts;
	float fade_alpha;
	uint32_t light_rgba;
	float atlas[4];
} decalgpuinst_t;

typedef struct
{
	int first_instance;
	int instance_count;
	decalblend_t blend;
	gltexture_t *texture;
} decalinstcmd_t;

COMPILE_TIME_ASSERT (decal_gpu_vert_std430_size, sizeof (decalgpuvert_t) == 32);
COMPILE_TIME_ASSERT (decal_gpu_inst_std430_size, sizeof (decalgpuinst_t) == 32);

typedef struct
{
	int active;
	int visible;
	int batch_count;
	int culled_frustum;
	int culled_distance;
	int culled_small;
	int draw_calls;
	int instanced_draws;
	int upload_bytes;
} decalstats_t;

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
static decalgpuvert_t decal_gpu_verts[MAX_DECAL_VERTS];
static decalgpuinst_t decal_gpu_instances[MAX_DECAL_INSTANCES];
static GLushort decal_indexes[MAX_DECAL_INDEXES];
static int decal_vert_cursor;
static int decal_inst_count;

static int decal_free_list[MAX_DECAL_INSTANCES];
static int decal_free_count;
static int decal_evict_cursor;
static double decal_stats_last_print;
static decalstats_t decal_stats;

static cvar_t r_decals = {"r_decals", "1", CVAR_ARCHIVE};
static cvar_t r_decals_max = {"r_decals_max", "256", CVAR_ARCHIVE};
static cvar_t r_decals_debug = {"r_decals_debug", "0", CVAR_NONE};
static cvar_t r_decals_instanced = {"r_decals_instanced", "1", CVAR_ARCHIVE};

#define DECAL_MAX_VIEW_DIST 4096.f
#define DECAL_MIN_SCREEN_RADIUS_PX 0.5f
#define DECAL_BLOOD_STREAK_CHANCE 0.35f
#define DECAL_BLOOD_POOL_CHANCE 0.20f
#define DECAL_TRI_VERTS_PER_INSTANCE ((MAX_POLY_VERTS - 2) * 3)

static byte R_ModulateLitByte (byte value, float light)
{
	const float clamped_light = CLAMP (0.f, light, 4.f);
	/* Keep decal base tint dominant; dynamic lighting should be a subtle shift. */
	const float soft_light = CLAMP (0.75f, 1.f + (clamped_light - 1.f) * 0.35f, 2.0f);
	return (byte)CLAMP (0.f, floorf ((float)value * soft_light + 0.5f), 255.f);
}

static void R_ModulateLitRGB (byte *dst, const byte *src, const vec3_t light)
{
	dst[0] = R_ModulateLitByte (src[0], light[0]);
	dst[1] = R_ModulateLitByte (src[1], light[1]);
	dst[2] = R_ModulateLitByte (src[2], light[2]);
}

static uint32_t R_PackLitColorRGBA (const vec3_t light)
{
	const byte r = R_ModulateLitByte (255, light[0]);
	const byte g = R_ModulateLitByte (255, light[1]);
	const byte b = R_ModulateLitByte (255, light[2]);

	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (255u << 24);
}

typedef struct
{
	const char *category;
	const char *debug_category;
	vec3_t align_dir;
	qboolean has_align_dir;
	float rotation;
	qboolean use_rotation;
	int priority_boost;
	float retention_boost;
} decalspawnopts_t;

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

static void R_Decals_FreeInstance (int idx)
{
	decalinst_t *inst;

	if (idx < 0 || idx >= MAX_DECAL_INSTANCES)
		return;

	inst = &decal_instances[idx];
	if (!inst->active)
		return;

	inst->active = false;
	inst->num_verts = 0;
	inst->first_vert = 0;
	decal_inst_count = q_max (0, decal_inst_count - 1);

	if (decal_free_count < MAX_DECAL_INSTANCES)
		decal_free_list[decal_free_count++] = idx;
}

static void R_Decals_ResetRuntime (void)
{
	int i;

	memset (decal_instances, 0, sizeof (decal_instances));
	decal_vert_cursor = 0;
	decal_inst_count = 0;
	decal_free_count = 0;
	decal_evict_cursor = 0;
	memset (&decal_stats, 0, sizeof (decal_stats));

	for (i = MAX_DECAL_INSTANCES - 1; i >= 0; --i)
		decal_free_list[decal_free_count++] = i;
}

static decalblend_t R_DecalParseBlend (const char *s)
{
	if (!q_strcasecmp (s, "add"))
		return DECAL_BLEND_ADD;
	if (!q_strcasecmp (s, "mul"))
		return DECAL_BLEND_MUL;
	return DECAL_BLEND_ALPHA;
}

static const char *R_DecalSkipUTF8BOM (const char *data)
{
	if (!data)
		return data;

	if ((unsigned char)data[0] == 0xEF
		&& (unsigned char)data[1] == 0xBB
		&& (unsigned char)data[2] == 0xBF)
		return data + 3;

	return data;
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
	if (def->size_min <= 0.f)
		def->size_min = 1.f;
	if (def->size_max <= 0.f)
		def->size_max = def->size_min;
	if (def->alpha_max < def->alpha_min)
		def->alpha_max = def->alpha_min;
	if (def->lifetime <= 0.f)
		def->lifetime = 10.f;
	if (def->fade < 0.f)
		def->fade = 0.f;
	if (def->fade > def->lifetime)
		def->fade = def->lifetime;

	def->atlas_u0 = CLAMP (0.f, def->atlas_u0, 1.f);
	def->atlas_v0 = CLAMP (0.f, def->atlas_v0, 1.f);
	def->atlas_u1 = CLAMP (0.f, def->atlas_u1, 1.f);
	def->atlas_v1 = CLAMP (0.f, def->atlas_v1, 1.f);
	if (def->atlas_u1 <= def->atlas_u0)
		def->atlas_u1 = q_min (1.f, def->atlas_u0 + 0.001f);
	if (def->atlas_v1 <= def->atlas_v0)
		def->atlas_v1 = q_min (1.f, def->atlas_v0 + 0.001f);

	def->valid = R_DecalLoadTexture (def);
}

static void R_Decals_LoadScript (const char *path)
{
	char *data = (char *) COM_LoadMallocFile (path, NULL);
	const char *c;
	decaldef_t *def = NULL;

	if (!data)
		return;

	c = R_DecalSkipUTF8BOM (data);
	while ((c = COM_Parse (c)))
	{
		if (!com_token[0])
			break;

		if (!q_strcasecmp (com_token, "decal") || !q_strcasecmp (com_token, "decaldef"))
		{
			if (!(c = COM_Parse (c)) || !com_token[0] || num_decal_defs >= MAX_DECAL_DEFS)
				break;
			def = &decal_defs[num_decal_defs++];
			memset (def, 0, sizeof (*def));
			q_strlcpy (def->name, com_token, sizeof (def->name));
			VectorSet (def->color, 1.f, 1.f, 1.f);
			def->atlas_u0 = 0.f;
			def->atlas_v0 = 0.f;
			def->atlas_u1 = 1.f;
			def->atlas_v1 = 1.f;
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
		else if (!q_strcasecmp (com_token, "atlas_rect") || !q_strcasecmp (com_token, "uvrect"))
		{
			if ((c = COM_Parse (c))) def->atlas_u0 = atof (com_token);
			if ((c = COM_Parse (c))) def->atlas_v0 = atof (com_token);
			if ((c = COM_Parse (c))) def->atlas_u1 = atof (com_token);
			if ((c = COM_Parse (c))) def->atlas_v1 = atof (com_token);
		}
	}

	if (def)
		R_DecalFinalizeDef (def);

	q_free(data);
}

static void R_Decals_LoadScripts (void)
{
	num_decal_defs = 0;
	memset (decal_defs, 0, sizeof (decal_defs));
	R_Decals_LoadScript ("decals.material");
	R_Decals_LoadScript ("materials/decals.material");
}

static decaldef_t *R_FindDecalDefByCategory (const char *category)
{
	/* Collect all valid matching defs, then pick one at random for variety. */
	decaldef_t *matches[MAX_DECAL_DEFS];
	int match_count = 0;
	int i;
	for (i = 0; i < num_decal_defs; ++i)
	{
		if (!decal_defs[i].valid)
			continue;
		if (!q_strcasecmp (decal_defs[i].category, category))
			matches[match_count++] = &decal_defs[i];
	}
	if (match_count == 0)
		return NULL;
	return matches[rand () % match_count];
}

static decaldef_t *R_FindAnyValidDecalDef (void)
{
	int i;

	for (i = 0; i < num_decal_defs; ++i)
	{
		if (!decal_defs[i].valid)
			continue;
		return &decal_defs[i];
	}

	return NULL;
}

static decaldef_t *R_FindDecalDefWithFallback (const char *category, qboolean *used_fallback)
{
	decaldef_t *def;

	if (used_fallback)
		*used_fallback = false;

	if (!category || !category[0])
		return NULL;

	def = R_FindDecalDefByCategory (category);
	if (def)
		return def;

	if (!q_strcasecmp (category, "bullet"))
	{
		def = R_FindDecalDefByCategory ("scorch");
		if (!def)
			def = R_FindDecalDefByCategory ("dirt");
	}
	else if (!q_strcasecmp (category, "scorch") || !q_strcasecmp (category, "dirt"))
	{
		def = R_FindDecalDefByCategory ("bullet");
	}
	else if (!q_strcasecmp (category, "blood_impact")
		|| !q_strcasecmp (category, "blood_streak")
		|| !q_strcasecmp (category, "blood_pool"))
	{
		def = R_FindDecalDefByCategory ("blood");
	}

	if (!def)
		def = R_FindAnyValidDecalDef ();

	if (def && used_fallback)
		*used_fallback = true;

	return def;
}

static qboolean R_DecalHasCategory (const char *category)
{
	int i;

	if (!category || !category[0])
		return false;

	for (i = 0; i < num_decal_defs; ++i)
	{
		if (!decal_defs[i].valid)
			continue;
		if (!q_strcasecmp (decal_defs[i].category, category))
			return true;
	}

	return false;
}

static float R_DecalComputeRetentionScore (const decaldef_t *def, float radius, float alpha, int priority_boost, float retention_boost)
{
	float score = 0.f;

	if (!def)
		return 0.f;

	score += (float)(def->priority + priority_boost) * 32.f;
	score += q_max (0.f, radius) * 2.0f;
	score += q_max (0.f, def->lifetime) * 0.5f;
	score += q_max (0.f, alpha) * 4.0f;
	score += retention_boost;

	return score;
}

static const char *R_DecalBloodCategoryName (decalbloodtype_t type)
{
	switch (type)
	{
	case DECAL_BLOOD_IMPACT:
		return "blood_impact";
	case DECAL_BLOOD_STREAK:
		return "blood_streak";
	case DECAL_BLOOD_POOL:
		return "blood_pool";
	default:
		break;
	}
	return "blood";
}

static int R_DecalAllocInstance (int priority, float retention_score)
{
	int max_inst = CLAMP (0, (int) r_decals_max.value, MAX_DECAL_INSTANCES);
	int idx;
	int i;
	int pick = -1;
	int pick_priority = 0;
	float pick_retention = FLT_MAX;
	double pick_spawn_time = 0.0;

	while (decal_free_count > 0)
	{
		idx = decal_free_list[--decal_free_count];
		if (idx < 0 || idx >= max_inst)
			continue;
		if (!decal_instances[idx].active)
			return idx;
	}

	if (max_inst <= 0)
		return -1;

	for (i = 0; i < max_inst; ++i)
	{
		decalinst_t *inst = &decal_instances[(decal_evict_cursor + i) % max_inst];

		if (!inst->active)
			return (int)(inst - decal_instances);

		if (inst->priority > priority)
			continue;
		if (inst->priority == priority && inst->retention_score > retention_score)
			continue;

		if (pick < 0 || inst->priority < pick_priority ||
			(inst->priority == pick_priority && inst->retention_score < pick_retention) ||
			(inst->priority == pick_priority && inst->retention_score == pick_retention && inst->spawn_time < pick_spawn_time))
		{
			pick = (int)(inst - decal_instances);
			pick_priority = inst->priority;
			pick_retention = inst->retention_score;
			pick_spawn_time = inst->spawn_time;
		}
	}

	if (pick >= 0)
	{
		decal_evict_cursor = (pick + 1) % max_inst;
		R_Decals_FreeInstance (pick);
		return pick;
	}

	return -1;
}

static void R_DecalComputeBounds (int first_vert, int num_verts, vec3_t out_center, float *out_radius)
{
	int i;
	vec3_t mins, maxs, delta;

	if (num_verts <= 0)
	{
		VectorClear (out_center);
		*out_radius = 1.f;
		return;
	}

	VectorSet (mins, FLT_MAX, FLT_MAX, FLT_MAX);
	VectorSet (maxs, -FLT_MAX, -FLT_MAX, -FLT_MAX);

	for (i = 0; i < num_verts; ++i)
	{
		float vx = decal_verts[first_vert + i].pos[0];
		float vy = decal_verts[first_vert + i].pos[1];
		float vz = decal_verts[first_vert + i].pos[2];
		mins[0] = q_min (mins[0], vx);
		mins[1] = q_min (mins[1], vy);
		mins[2] = q_min (mins[2], vz);
		maxs[0] = q_max (maxs[0], vx);
		maxs[1] = q_max (maxs[1], vy);
		maxs[2] = q_max (maxs[2], vz);
	}

	out_center[0] = (mins[0] + maxs[0]) * 0.5f;
	out_center[1] = (mins[1] + maxs[1]) * 0.5f;
	out_center[2] = (mins[2] + maxs[2]) * 0.5f;
	VectorSubtract (maxs, out_center, delta);
	*out_radius = q_max (VectorLength (delta), 1.f);
}

static qboolean R_DecalSphereOutsideFrustum (const vec3_t center, float radius)
{
	int i;
	for (i = 0; i < 4; ++i)
	{
		if (DotProduct (center, frustum[i].normal) - frustum[i].dist <= -radius)
			return true;
	}
	return false;
}

static qboolean R_DecalSphereBeyondDistance (const vec3_t center, float radius, float *out_dist)
{
	vec3_t to_center;
	float dist2;
	float limit = DECAL_MAX_VIEW_DIST + radius;

	VectorSubtract (center, r_refdef.vieworg, to_center);
	dist2 = DotProduct (to_center, to_center);
	if (out_dist)
		*out_dist = sqrtf (q_max (dist2, 0.f));

	return dist2 > limit * limit;
}

static qboolean R_DecalIsTooSmallOnScreen (float radius, float dist_to_view)
{
	float tan_half_fovy;
	float pixel_radius;
	float view_height = (float)q_max (1, r_refdef.vrect.height);

	if (dist_to_view <= 1.f)
		return false;

	tan_half_fovy = tanf (DEG2RAD (r_refdef.fov_y) * 0.5f);
	if (tan_half_fovy <= 0.0001f)
		return false;

	pixel_radius = (radius / (dist_to_view * tan_half_fovy)) * (view_height * 0.5f);
	return pixel_radius < DECAL_MIN_SCREEN_RADIUS_PX;
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
			v.uv[0] = a->uv[0] + (b->uv[0] - a->uv[0]) * t;
			v.uv[1] = a->uv[1] + (b->uv[1] - a->uv[1]) * t;
			for (k = 0; k < 4; ++k)
				v.color[k] = (byte)(a->color[k] + (b->color[k] - a->color[k]) * t);
			out[out_count++] = v;
		}
	}
	return out_count;
}

static void R_BuildSurfaceDecalBasis (const msurface_t *surf, const vec3_t align_dir, qboolean use_align_dir, float rotation, vec3_t out_sdir, vec3_t out_tdir, vec3_t out_normal)
{
	vec3_t surf_normal, up = {0.f, 0.f, 1.f};
	float c = cosf (rotation), s = sinf (rotation);
	int i;
	qboolean have_aligned_sdir = false;

	VectorCopy (surf->plane->normal, surf_normal);
	if (surf->flags & SURF_PLANEBACK)
		VectorInverse (surf_normal);
	VectorNormalizeFast (surf_normal);

	if (use_align_dir && VectorLengthSquared (align_dir) > 0.0001f)
	{
		vec3_t tangent;
		float along_normal = DotProduct (align_dir, surf_normal);
		VectorMA (align_dir, -along_normal, surf_normal, tangent);
		if (VectorLengthSquared (tangent) > 0.0001f)
		{
			VectorCopy (tangent, out_sdir);
			VectorNormalizeFast (out_sdir);
			have_aligned_sdir = true;
		}
	}

	if (!have_aligned_sdir)
	{
		for (i = 0; i < 3; ++i)
		{
			out_sdir[i] = surf->texinfo->vecs[0][i];
			out_tdir[i] = surf->texinfo->vecs[1][i];
		}
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
	float radius, float alpha, const vec3_t color, const vec3_t align_dir, qboolean use_align_dir, int first_vert)
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

	R_BuildSurfaceDecalBasis (surf, align_dir, use_align_dir, rotation, sdir, tdir, normal);

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

static float R_SurfaceImpactPlaneSignedDistance (const msurface_t *surf, const vec3_t point)
{
	float signed_dist;

	if (!surf || !surf->plane)
		return FLT_MAX;

	/*
	 * SURF_PLANEBACK flips the surface-facing normal; keep dist in the same
	 * orientation so the absolute plane distance remains correct on back faces.
	 */
	signed_dist = DotProduct (point, surf->plane->normal) - surf->plane->dist;
	if (surf->flags & SURF_PLANEBACK)
		signed_dist = -signed_dist;

	return signed_dist;
}

static mleaf_t *R_FindImpactLeaf (const vec3_t origin, const vec3_t normal)
{
	/*
	 * Temp entity impacts are quantized and can land a hair inside solid space.
	 * Probe both sides of the recovered impact normal so we can still find the
	 * nearby render leaf that owns the wall/floor mark surfaces.
	 */
	static const float offsets[] = {0.f, 1.f, -1.f, 2.f, -2.f};
	mleaf_t *fallback = NULL;
	int i;

	for (i = 0; i < (int) countof (offsets); ++i)
	{
		vec3_t sample;
		mleaf_t *leaf;

		VectorCopy (origin, sample);
		if (offsets[i] != 0.f)
			VectorMA (sample, offsets[i], normal, sample);

		leaf = Mod_PointInLeaf (sample, cl.worldmodel);
		if (!leaf || !leaf->nummarksurfaces)
			continue;

		if (leaf->contents != CONTENTS_SOLID)
			return leaf;

		if (!fallback)
			fallback = leaf;
	}

	return fallback;
}

static void R_DecalDebugLogSpawn (qboolean spawned, const char *reason, const char *category,
	const decaldef_t *def, const vec3_t origin, const vec3_t normal,
	float radius, float alpha, const mleaf_t *leaf, int surfaces_considered,
	int surfaces_skip_flags, int surfaces_skip_invalid, int surfaces_skip_dist,
	int surfaces_capacity_break, int projected_surfaces, int projected_verts)
{
	int leaf_index = -1;
	int leaf_marks = 0;

	if (!r_decals_debug.value)
		return;

	if (leaf && cl.worldmodel && cl.worldmodel->leafs)
	{
		leaf_index = (int) (leaf - cl.worldmodel->leafs);
		leaf_marks = leaf->nummarksurfaces;
	}

	Con_Printf (
		"decal %s reason=%s cat=%s def=%s org=(%.1f %.1f %.1f) n=(%.2f %.2f %.2f) radius=%.2f alpha=%.2f "
		"leaf=%d marks=%d surfaces=%d skip_flags=%d skip_invalid=%d skip_dist=%d cap_break=%d projected_surfaces=%d verts=%d\n",
		spawned ? "spawned" : "rejected",
		reason ? reason : "unknown",
		category ? category : "<null>",
		(def && def->name[0]) ? def->name : "<none>",
		origin[0], origin[1], origin[2],
		normal[0], normal[1], normal[2],
		radius, alpha,
		leaf_index, leaf_marks,
		surfaces_considered, surfaces_skip_flags, surfaces_skip_invalid, surfaces_skip_dist,
		surfaces_capacity_break, projected_surfaces, projected_verts);
}

static void R_SpawnImpactDecalSingle (const char *category, const vec3_t origin, const vec3_t normal, const decalspawnopts_t *opts)
{
	decaldef_t *def;
	mleaf_t *leaf;
	vec3_t n;
	vec3_t align_dir = {0.f, 0.f, 0.f};
	float radius, alpha, rot;
	int priority_boost = 0;
	float retention_score = 0.f;
	qboolean has_align_dir = false;
	const char *lookup_category = category;
	const char *debug_category = category;
	int i, inst_idx, first_vert, temp_count = 0;
	decalinst_t *inst;
	const char *fail_reason = NULL;
	qboolean spawned = false;
	int surfaces_considered = 0;
	int surfaces_skip_flags = 0;
	int surfaces_skip_invalid = 0;
	int surfaces_skip_dist = 0;
	int surfaces_capacity_break = 0;
	int projected_surfaces = 0;
	qboolean used_category_fallback = false;

	def = NULL;
	leaf = NULL;
	radius = -1.f;
	alpha = -1.f;

	if (opts)
	{
		if (opts->category && opts->category[0])
			lookup_category = opts->category;
		if (opts->debug_category && opts->debug_category[0])
			debug_category = opts->debug_category;
		if (opts->has_align_dir)
		{
			VectorCopy (opts->align_dir, align_dir);
			has_align_dir = true;
		}
		priority_boost = opts->priority_boost;
	}

	if (!r_decals.value)
	{
		fail_reason = "decals_disabled";
		goto done;
	}
	if (!cl.worldmodel)
	{
		fail_reason = "worldmodel_missing";
		goto done;
	}
	if (!category)
	{
		fail_reason = "category_missing";
		goto done;
	}

	def = R_FindDecalDefWithFallback (lookup_category, &used_category_fallback);
	if (!def)
	{
		fail_reason = "decaldef_missing";
		goto done;
	}

	VectorCopy (normal, n);
	if (VectorLengthSquared (n) < 0.0001f)
	{
		fail_reason = "normal_invalid";
		goto done;
	}
	VectorNormalizeFast (n);
	rot = 0.f;

	if (opts && opts->use_rotation)
		rot = opts->rotation;
	else if (def->random_rotation)
		rot = ((float) rand () / (float) RAND_MAX) * (2.f * M_PI);

	radius = def->size_min + ((float) rand () / (float) RAND_MAX) * (def->size_max - def->size_min);
	radius = q_max (radius, 0.01f);
	alpha = def->alpha_min + ((float) rand () / (float) RAND_MAX) * (def->alpha_max - def->alpha_min);
	retention_score = R_DecalComputeRetentionScore (def, radius, alpha, priority_boost, opts ? opts->retention_boost : 0.f);

	leaf = R_FindImpactLeaf (origin, n);
	if (!leaf || !leaf->nummarksurfaces)
	{
		fail_reason = "impact_leaf_missing";
		goto done;
	}

	first_vert = decal_vert_cursor;
	if (first_vert + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
	{
		R_Decals_CompactVerts ();
		first_vert = decal_vert_cursor;
		if (first_vert + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
		{
			fail_reason = "vertex_pool_exhausted";
			goto done;
		}
	}
	temp_count = 0;

	for (i = 0; i < leaf->nummarksurfaces; ++i)
	{
		msurface_t *surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[i]];
		int flags = SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWSPRITE | SURF_DRAWLAVA | SURF_DRAWSLIME | SURF_DRAWWATER | SURF_DRAWTELE;
		vec3_t surf_normal, proj_origin;
		float signed_dist;
		float d;
		int added;

		surfaces_considered++;

		if (surf->flags & flags)
		{
			surfaces_skip_flags++;
			continue;
		}
		if (!surf->plane || !surf->texinfo)
		{
			surfaces_skip_invalid++;
			continue;
		}

		signed_dist = R_SurfaceImpactPlaneSignedDistance (surf, origin);
		d = fabsf (signed_dist);
		if (d > radius + 2.f)
		{
			surfaces_skip_dist++;
			continue;
		}

		VectorCopy (surf->plane->normal, surf_normal);
		if (surf->flags & SURF_PLANEBACK)
			VectorInverse (surf_normal);
		VectorMA (origin, -signed_dist, surf_normal, proj_origin);

		if (first_vert + temp_count + MAX_POLY_VERTS >= MAX_DECAL_VERTS)
		{
			surfaces_capacity_break = 1;
			break;
		}

		added = R_ProjectDecalToSurface (surf, proj_origin, rot, radius, alpha, def->color, align_dir, has_align_dir, first_vert + temp_count);
		if (added > 0)
			projected_surfaces++;
		temp_count += added;
	}

	if (temp_count <= 0)
	{
		fail_reason = "projection_empty";
		goto done;
	}

	inst_idx = R_DecalAllocInstance (def->priority + priority_boost, retention_score);
	if (inst_idx < 0)
	{
		fail_reason = "instance_pool_exhausted";
		goto done;
	}

	inst = &decal_instances[inst_idx];
	inst->active = true;
	inst->def_index = (int)(def - decal_defs);
	inst->spawn_time = cl.time;
	inst->die_time = cl.time + def->lifetime;
	inst->priority = def->priority + priority_boost;
	inst->retention_score = retention_score;
	inst->blend = def->blend;
	inst->texture = def->texture;
	inst->first_vert = first_vert;
	inst->num_verts = temp_count;
	R_DecalComputeBounds (inst->first_vert, inst->num_verts, inst->center, &inst->cull_radius);
	decal_vert_cursor = first_vert + temp_count;
	decal_inst_count++;
	spawned = true;
	fail_reason = used_category_fallback ? "ok_fallback" : "ok";

done:
	R_DecalDebugLogSpawn (spawned, fail_reason, debug_category, def, origin, normal, radius, alpha,
		leaf, surfaces_considered, surfaces_skip_flags, surfaces_skip_invalid, surfaces_skip_dist,
		surfaces_capacity_break, projected_surfaces, temp_count);
}

static void R_SpawnBloodImpactDecals_Internal (const vec3_t origin, const vec3_t normal, const vec3_t hit_dir, qboolean heavy)
{
	decalspawnopts_t opts;
	vec3_t streak_dir;
	vec3_t streak_align;
	qboolean has_streak_align = false;
	int impacts = 1;
	int i;

	if (!R_DecalHasCategory ("blood")
		&& !R_DecalHasCategory ("blood_impact")
		&& !R_DecalHasCategory ("blood_streak")
		&& !R_DecalHasCategory ("blood_pool"))
	{
		R_SpawnImpactDecalSingle ("blood", origin, normal, NULL);
		return;
	}

	if (hit_dir && VectorLengthSquared (hit_dir) > 0.0001f)
	{
		VectorCopy (hit_dir, streak_dir);
		VectorNormalizeFast (streak_dir);
		VectorCopy (streak_dir, streak_align);
		has_streak_align = true;
	}

	if (heavy)
		impacts = 1 + (rand () & 1);

	memset (&opts, 0, sizeof (opts));
	opts.category = R_DecalHasCategory (R_DecalBloodCategoryName (DECAL_BLOOD_IMPACT)) ?
		R_DecalBloodCategoryName (DECAL_BLOOD_IMPACT) : "blood";
	opts.debug_category = "blood_impact";
	for (i = 0; i < impacts; ++i)
		R_SpawnImpactDecalSingle ("blood", origin, normal, &opts);

	if (heavy || ((float) rand () / (float) RAND_MAX) < DECAL_BLOOD_STREAK_CHANCE)
	{
		memset (&opts, 0, sizeof (opts));
		opts.category = R_DecalHasCategory (R_DecalBloodCategoryName (DECAL_BLOOD_STREAK)) ?
			R_DecalBloodCategoryName (DECAL_BLOOD_STREAK) : "blood";
		opts.debug_category = "blood_streak";
		if (has_streak_align)
		{
			VectorCopy (streak_align, opts.align_dir);
			opts.has_align_dir = true;
		}
		opts.priority_boost = 2;
		opts.retention_boost = 6.f;
		R_SpawnImpactDecalSingle ("blood", origin, normal, &opts);
	}

	if (heavy && ((float) rand () / (float) RAND_MAX) < DECAL_BLOOD_POOL_CHANCE)
	{
		memset (&opts, 0, sizeof (opts));
		opts.category = R_DecalHasCategory (R_DecalBloodCategoryName (DECAL_BLOOD_POOL)) ?
			R_DecalBloodCategoryName (DECAL_BLOOD_POOL) : "blood";
		opts.debug_category = "blood_pool";
		opts.priority_boost = 4;
		opts.retention_boost = 18.f;
		R_SpawnImpactDecalSingle ("blood", origin, normal, &opts);
	}
}

void R_SpawnImpactDecalEx (const char *category, const vec3_t origin, const vec3_t normal, const vec3_t hit_dir, qboolean heavy_blood)
{
	decalspawnopts_t opts;

	if (!category)
	{
		R_SpawnImpactDecalSingle (category, origin, normal, NULL);
		return;
	}

	if (!q_strcasecmp (category, "blood"))
	{
		R_SpawnBloodImpactDecals_Internal (origin, normal, hit_dir, heavy_blood);
		return;
	}

	if (!q_strcasecmp (category, "blood_streak"))
	{
		memset (&opts, 0, sizeof (opts));
		opts.category = "blood_streak";
		opts.debug_category = "blood_streak";
		if (hit_dir && VectorLengthSquared (hit_dir) > 0.0001f)
		{
			VectorCopy (hit_dir, opts.align_dir);
			opts.has_align_dir = true;
		}
		opts.priority_boost = 2;
		opts.retention_boost = 6.f;
		R_SpawnImpactDecalSingle (category, origin, normal, &opts);
		return;
	}

	if (!q_strcasecmp (category, "blood_pool"))
	{
		memset (&opts, 0, sizeof (opts));
		opts.category = "blood_pool";
		opts.debug_category = "blood_pool";
		opts.priority_boost = 4;
		opts.retention_boost = 18.f;
		R_SpawnImpactDecalSingle (category, origin, normal, &opts);
		return;
	}

	R_SpawnImpactDecalSingle (category, origin, normal, NULL);
}

void R_SpawnImpactDecal (const char *category, const vec3_t origin, const vec3_t normal)
{
	R_SpawnImpactDecalEx (category, origin, normal, NULL, false);
}

void R_InitDecals (void)
{
	Cvar_RegisterVariable (&r_decals);
	Cvar_RegisterVariable (&r_decals_max);
	Cvar_RegisterVariable (&r_decals_debug);
	Cvar_RegisterVariable (&r_decals_instanced);
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

static void R_Decals_ExecFrameGraphPass (RenderPassContext *ctx)
{
	(void)ctx;
	R_UpdateDecals ();
}

static const RenderPassDesc s_decals_framegraph_pass = {
	.name = "Update decals",
	.reads = RENDER_RES_NONE,
	.writes = RENDER_RES_DECALS,
	.side_effects = 0,
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = NULL,
	.execute = R_Decals_ExecFrameGraphPass
};

void R_Decals_RegisterFrameGraphPasses (void)
{
	(void)R_FrameGraph_AddPass (&s_decals_framegraph_pass);
}

void R_UpdateDecals (void)
{
	int max_inst = CLAMP (0, (int) r_decals_max.value, MAX_DECAL_INSTANCES);
	int i;

	for (i = max_inst; i < MAX_DECAL_INSTANCES; ++i)
		R_Decals_FreeInstance (i);

	for (i = 0; i < MAX_DECAL_INSTANCES; ++i)
	{
		if (!decal_instances[i].active)
			continue;
		if (decal_instances[i].die_time <= cl.time)
			R_Decals_FreeInstance (i);
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

static int R_DecalsGatherVisible (decalinst_t **draw, int max_inst)
{
	int draw_count = 0;
	int i;

	for (i = 0; i < max_inst; ++i)
	{
		decalinst_t *inst = &decal_instances[i];
		float dist_to_view;

		if (!inst->active)
			continue;

		decal_stats.active++;
		if (!inst->texture || inst->num_verts < 3)
			continue;
		if (inst->def_index < 0 || inst->def_index >= num_decal_defs)
			continue;
		if (R_DecalSphereOutsideFrustum (inst->center, inst->cull_radius))
		{
			decal_stats.culled_frustum++;
			continue;
		}
		if (R_DecalSphereBeyondDistance (inst->center, inst->cull_radius, &dist_to_view))
		{
			decal_stats.culled_distance++;
			continue;
		}
		if (R_DecalIsTooSmallOnScreen (inst->cull_radius, dist_to_view))
		{
			decal_stats.culled_small++;
			continue;
		}

		draw[draw_count++] = inst;
	}

	decal_stats.visible = draw_count;
	return draw_count;
}

static unsigned R_DecalBlendState (decalblend_t blend)
{
	if (blend == DECAL_BLEND_ADD)
		return GLS_BLEND_ADD;
	if (blend == DECAL_BLEND_MUL)
		return GLS_BLEND_MULTIPLY;
	return GLS_BLEND_ALPHA;
}

static qboolean R_DrawDecalsLegacy (decalinst_t **draw, int draw_count)
{
	decaldrawcmd_t cmds[MAX_DECAL_INSTANCES];
	int cmd_count = 0;
	int batch_vcount = 0;
	int batch_icount = 0;
	int current_blend = -1;
	gltexture_t *current_texture = NULL;
	GLuint vbo, ibo;
	GLbyte *vofs, *iofs;
	int i;

	for (i = 0; i < draw_count; ++i)
	{
		int vcount = draw[i]->num_verts;
		int local_icount;
		int j;
		int batch_base;
		decaldef_t *def = &decal_defs[draw[i]->def_index];
		vec3_t decal_light;
		float uscale = def->atlas_u1 - def->atlas_u0;
		float vscale = def->atlas_v1 - def->atlas_v0;
		float fade_alpha = R_DecalFadeAlpha (draw[i], &decal_defs[draw[i]->def_index]);

		if (vcount < 3)
			continue;

		R_SampleReceiverLighting (draw[i]->center, decal_light);

		local_icount = (vcount - 2) * 3;
		if (batch_vcount + vcount > MAX_DECAL_VERTS || batch_icount + local_icount > MAX_DECAL_INDEXES)
			break;

		if (cmd_count == 0
			|| cmds[cmd_count - 1].blend != draw[i]->blend
			|| cmds[cmd_count - 1].texture != draw[i]->texture)
		{
			decaldrawcmd_t *cmd = &cmds[cmd_count++];
			cmd->first_index = batch_icount;
			cmd->index_count = 0;
			cmd->blend = draw[i]->blend;
			cmd->texture = draw[i]->texture;
		}

		batch_base = batch_vcount;

		for (j = 0; j < vcount; ++j)
		{
			const decalvert_t *src = &decal_verts[draw[i]->first_vert + j];
			decalvert_t *dst = &decal_draw_verts[batch_vcount + j];
			*dst = *src;
			dst->uv[0] = def->atlas_u0 + src->uv[0] * uscale;
			dst->uv[1] = def->atlas_v0 + src->uv[1] * vscale;
			R_ModulateLitRGB (dst->color, src->color, decal_light);
			dst->color[3] = (byte) (src->color[3] * fade_alpha);
		}
		batch_vcount += vcount;

		for (j = 2; j < vcount; ++j)
		{
			decal_indexes[batch_icount++] = (GLushort)batch_base;
			decal_indexes[batch_icount++] = (GLushort)(batch_base + j - 1);
			decal_indexes[batch_icount++] = (GLushort)(batch_base + j);
		}
		cmds[cmd_count - 1].index_count += local_icount;
	}

	if (batch_vcount <= 0 || batch_icount <= 0 || cmd_count <= 0)
		return false;

	decal_stats.batch_count = cmd_count;
	decal_stats.upload_bytes = (int)(sizeof (decal_draw_verts[0]) * batch_vcount + sizeof (decal_indexes[0]) * batch_icount);

	GL_UseProgram (glprogs.decal);
	GL_Upload (GL_ARRAY_BUFFER, decal_draw_verts, sizeof (decalvert_t) * batch_vcount, &vbo, &vofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, vbo);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (decalvert_t), vofs + offsetof (decalvert_t, pos));
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof (decalvert_t), vofs + offsetof (decalvert_t, uv));
	GL_VertexAttribPointerFunc (2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (decalvert_t), vofs + offsetof (decalvert_t, color));

	GL_Upload (GL_ELEMENT_ARRAY_BUFFER, decal_indexes, sizeof (GLushort) * batch_icount, &ibo, &iofs);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, ibo);

	for (i = 0; i < cmd_count; ++i)
	{
		unsigned blendstate = R_DecalBlendState (cmds[i].blend);
		intptr_t index_offset = (intptr_t)iofs + (intptr_t)(cmds[i].first_index * sizeof (GLushort));

		if (current_blend != (int)cmds[i].blend)
		{
			{
				const unsigned state = blendstate | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (3) | GLS_INSTANCED_ATTRIBS (0);
				RenderBackendPipelineDesc pipeline_desc;
				RenderBackendDynamicState dynamic_state;
				memset (&pipeline_desc, 0, sizeof (pipeline_desc));
				memset (&dynamic_state, 0, sizeof (dynamic_state));
				pipeline_desc.state_bits = state;
				dynamic_state.blend_state = state;
				dynamic_state.depth_state = state;
				dynamic_state.raster_state = state;
				R_Backend_BindPipeline (&pipeline_desc);
				R_Backend_SetDynamicState (&dynamic_state);
			}
			current_blend = (int)cmds[i].blend;
			current_texture = NULL;
		}

		if (current_texture != cmds[i].texture)
		{
			GL_Bind (GL_TEXTURE0, cmds[i].texture);
			current_texture = cmds[i].texture;
		}

		R_Backend_DrawIndexed (R_BACKEND_PRIMITIVE_TRIANGLES, R_BACKEND_INDEX_TYPE_UINT16, cmds[i].index_count, index_offset);
		decal_stats.draw_calls++;
	}

	return true;
}

static qboolean R_DrawDecalsInstanced (decalinst_t **draw, int draw_count)
{
	const RenderBackendCaps *caps = R_Backend_GetCaps ();
	decalinstcmd_t cmds[MAX_DECAL_INSTANCES];
	GLuint ssbo;
	GLbyte *ssbo_ofs;
	GLuint buffers[2];
	GLintptr offsets[2];
	GLsizeiptr sizes[2];
	int cmd_count = 0;
	int instance_count = 0;
	int current_blend = -1;
	gltexture_t *current_texture = NULL;
	int i;

	if (!caps || !caps->supports_draw_instanced || !glprogs.decal_instanced)
		return false;

	for (i = 0; i < draw_count; ++i)
	{
		decaldef_t *def = &decal_defs[draw[i]->def_index];
		decalgpuinst_t *gpuinst;
		vec3_t decal_light;

		if (draw[i]->num_verts < 3)
			continue;
		if (instance_count >= MAX_DECAL_INSTANCES)
			break;

		R_SampleReceiverLighting (draw[i]->center, decal_light);

		if (cmd_count == 0
			|| cmds[cmd_count - 1].blend != draw[i]->blend
			|| cmds[cmd_count - 1].texture != draw[i]->texture)
		{
			decalinstcmd_t *cmd = &cmds[cmd_count++];
			cmd->first_instance = instance_count;
			cmd->instance_count = 0;
			cmd->blend = draw[i]->blend;
			cmd->texture = draw[i]->texture;
		}

		gpuinst = &decal_gpu_instances[instance_count++];
		gpuinst->first_vert = (uint32_t) draw[i]->first_vert;
		gpuinst->num_verts = (uint32_t) draw[i]->num_verts;
		gpuinst->fade_alpha = R_DecalFadeAlpha (draw[i], def);
		gpuinst->light_rgba = R_PackLitColorRGBA (decal_light);
		gpuinst->atlas[0] = def->atlas_u0;
		gpuinst->atlas[1] = def->atlas_v0;
		gpuinst->atlas[2] = def->atlas_u1;
		gpuinst->atlas[3] = def->atlas_v1;
		cmds[cmd_count - 1].instance_count++;
	}

	if (instance_count <= 0 || cmd_count <= 0 || decal_vert_cursor <= 0)
		return false;

	for (i = 0; i < decal_vert_cursor; ++i)
	{
		const decalvert_t *src = &decal_verts[i];
		decalgpuvert_t *dst = &decal_gpu_verts[i];
		dst->pos[0] = src->pos[0];
		dst->pos[1] = src->pos[1];
		dst->pos[2] = src->pos[2];
		dst->u = src->uv[0];
		dst->v = src->uv[1];
		dst->color = (uint32_t)src->color[0]
			| ((uint32_t)src->color[1] << 8)
			| ((uint32_t)src->color[2] << 16)
			| ((uint32_t)src->color[3] << 24);
		dst->pad0 = 0u;
		dst->pad1 = 0u;
	}

	decal_stats.batch_count = cmd_count;
	decal_stats.upload_bytes = (int)(sizeof (decalgpuvert_t) * decal_vert_cursor + sizeof (decalgpuinst_t) * instance_count);

	GL_UseProgram (glprogs.decal_instanced);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, decal_gpu_verts, sizeof (decalgpuvert_t) * decal_vert_cursor, &ssbo, &ssbo_ofs);
	buffers[0] = ssbo;
	offsets[0] = (GLintptr) ssbo_ofs;
	sizes[0] = sizeof (decalgpuvert_t) * decal_vert_cursor;

	GL_Upload (GL_SHADER_STORAGE_BUFFER, decal_gpu_instances, sizeof (decalgpuinst_t) * instance_count, &ssbo, &ssbo_ofs);
	buffers[1] = ssbo;
	offsets[1] = (GLintptr) ssbo_ofs;
	sizes[1] = sizeof (decalgpuinst_t) * instance_count;

	GL_BindBuffersRange (GL_SHADER_STORAGE_BUFFER, 1, 2, buffers, offsets, sizes);

	for (i = 0; i < cmd_count; ++i)
	{
		unsigned blendstate = R_DecalBlendState (cmds[i].blend);

		if (current_blend != (int)cmds[i].blend)
		{
			{
				const unsigned state = blendstate | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (0) | GLS_INSTANCED_ATTRIBS (0);
				RenderBackendPipelineDesc pipeline_desc;
				RenderBackendDynamicState dynamic_state;
				memset (&pipeline_desc, 0, sizeof (pipeline_desc));
				memset (&dynamic_state, 0, sizeof (dynamic_state));
				pipeline_desc.state_bits = state;
				dynamic_state.blend_state = state;
				dynamic_state.depth_state = state;
				dynamic_state.raster_state = state;
				R_Backend_BindPipeline (&pipeline_desc);
				R_Backend_SetDynamicState (&dynamic_state);
			}
			current_blend = (int)cmds[i].blend;
			current_texture = NULL;
		}

		if (current_texture != cmds[i].texture)
		{
			GL_Bind (GL_TEXTURE0, cmds[i].texture);
			current_texture = cmds[i].texture;
		}

		GL_Uniform1iFunc (0, cmds[i].first_instance);
		R_Backend_DrawInstanced (R_BACKEND_PRIMITIVE_TRIANGLES, 0, DECAL_TRI_VERTS_PER_INSTANCE, cmds[i].instance_count);
		decal_stats.draw_calls++;
		decal_stats.instanced_draws++;
	}

	return true;
}

static void R_DecalsDebugPrintFrameStats (void)
{
	int culled_total;

	if (r_decals_debug.value < 2)
		return;
	if (cl.time < decal_stats_last_print + 0.5)
		return;

	culled_total = decal_stats.culled_frustum + decal_stats.culled_distance + decal_stats.culled_small;
	Con_Printf ("decal stats active=%d visible=%d culled=%d(frustum=%d dist=%d small=%d) batches=%d draws=%d instanced=%d upload=%dB\n",
		decal_stats.active, decal_stats.visible, culled_total,
		decal_stats.culled_frustum, decal_stats.culled_distance, decal_stats.culled_small,
		decal_stats.batch_count, decal_stats.draw_calls, decal_stats.instanced_draws, decal_stats.upload_bytes);
	decal_stats_last_print = cl.time;
}

void R_DrawDecals (void)
{
	decalinst_t *draw[MAX_DECAL_INSTANCES];
	int draw_count = 0;
	int max_inst = CLAMP (0, (int) r_decals_max.value, MAX_DECAL_INSTANCES);
	qboolean rendered = false;

	memset (&decal_stats, 0, sizeof (decal_stats));

	if (!r_decals.value)
		return;

	draw_count = R_DecalsGatherVisible (draw, max_inst);

	if (!draw_count)
	{
		R_DecalsDebugPrintFrameStats ();
		return;
	}

	qsort (draw, draw_count, sizeof (draw[0]), R_DecalSortCmp);

	GL_BeginGroup ("Decals");
	GL_PolygonOffset (OFFSET_DECAL);

	if (r_decals_instanced.value)
		rendered = R_DrawDecalsInstanced (draw, draw_count);
	if (!rendered)
		rendered = R_DrawDecalsLegacy (draw, draw_count);

	GL_PolygonOffset (OFFSET_NONE);
	GL_EndGroup ();

	if (!rendered)
		decal_stats.visible = 0;

	R_DecalsDebugPrintFrameStats ();
}

