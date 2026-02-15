#include "quakedef.h"
#include "r_decals.h"
#include "miniz.h"
#include "world.h"
#include <float.h>

#define DECAL_MAX_DEFS 256
#define DECAL_MAX_TEXTURES 8

typedef enum decal_blend_e
{
	DECAL_BLEND_ALPHA = 0,
	DECAL_BLEND_ADD,
	DECAL_BLEND_MODULATE,
	DECAL_BLEND_PREMUL
} decal_blend_t;

typedef enum decal_category_e
{
	DECAL_CAT_GENERIC = 0,
	DECAL_CAT_BULLET,
	DECAL_CAT_BLOOD,
	DECAL_CAT_SCORCH,
	DECAL_CAT_DIRT
} decal_category_t;

typedef struct decal_def_s
{
	char name[64];
	char texture_paths[DECAL_MAX_TEXTURES][MAX_QPATH];
	gltexture_t *textures[DECAL_MAX_TEXTURES];
	int num_textures;
	float size_min, size_max;
	float alpha_min, alpha_max;
	vec3_t color;
	float lifetime;
	float fade;
	int priority;
	int random_rotation;
	decal_blend_t blend;
	decal_category_t category;
} decal_def_t;

typedef struct decalvertex_s
{
	vec3_t pos;
	float uv[2];
} decalvertex_t;

typedef struct decal_instance_s
{
	qboolean active;
	int spawn_id;
	int frame_touched;
	int priority;
	decal_category_t category;
	double spawn_time;
	float lifetime;
	float fade;
	float dist_sq;
	decal_blend_t blend;
	gltexture_t *texture;
	vec3_t origin;
	vec3_t verts[4];
	float alpha;
	qboolean selected;
} decal_instance_t;

typedef struct decal_stats_s
{
	int spawned;
	int dropped_spawn_budget;
	int dropped_pool_overwrite;
	int rendered;
	int skipped_render_budget;
} decal_stats_t;

static cvar_t r_decals = { "r_decals", "1", CVAR_ARCHIVE };
static cvar_t r_decals_max = { "r_decals_max", "512", CVAR_ARCHIVE };
static cvar_t r_decals_drawdist = { "r_decals_drawdist", "1536", CVAR_ARCHIVE };
static cvar_t r_decals_lifetime = { "r_decals_lifetime", "30", CVAR_ARCHIVE };
static cvar_t r_decals_fade = { "r_decals_fade", "6", CVAR_ARCHIVE };
static cvar_t r_decals_bias = { "r_decals_bias", "0.6", CVAR_ARCHIVE };
static cvar_t r_decals_spawn_budget = { "r_decals_spawn_budget", "8", CVAR_ARCHIVE };
static cvar_t r_decals_render_budget_decals = { "r_decals_render_budget_decals", "256", CVAR_ARCHIVE };
static cvar_t r_decals_debug = { "r_decals_debug", "0", CVAR_NONE };
static cvar_t r_decals_on_liquids = { "r_decals_on_liquids", "0", CVAR_ARCHIVE };
static cvar_t r_decals_reload = { "r_decals_reload", "0", CVAR_NONE };

static decal_def_t decal_defs[DECAL_MAX_DEFS];
static int decal_def_count;
static qboolean decal_warned_missing_tex[DECAL_MAX_DEFS][DECAL_MAX_TEXTURES];

static decal_instance_t *decal_pool;
static int decal_capacity;
static int decal_head;
static int decal_spawned_this_frame;
static int decal_next_spawn_id;
static int decal_frame;
static decal_stats_t decal_stats;

#define DECAL_BATCH_MAX 256

static int R_Decals_GetMax (void)
{
	return CLAMP (0, (int)r_decals_max.value, 4096);
}

static void R_Decals_ResetFrameStats (void)
{
	decal_spawned_this_frame = 0;
	memset (&decal_stats, 0, sizeof (decal_stats));
}

static void R_Decals_ClampCvar (cvar_t *var, int minval, int maxval)
{
	int value = (int)var->value;
	int clamped = CLAMP (minval, value, maxval);
	if (value != clamped)
		Cvar_SetValueQuick (var, (float)clamped);
}

static void R_Decals_Max_Changed (cvar_t *var)
{
	R_Decals_ClampCvar (var, 0, 4096);
}

static void R_Decals_Budget_Changed (cvar_t *var)
{
	R_Decals_ClampCvar (var, 0, 2048);
}

static void R_Decals_Reload_f (cvar_t *var)
{
	if (var->value <= 0)
		return;
	Cvar_SetValueQuick (var, 0.f);
	R_Decals_ReloadScripts ();
}

static void R_Decals_BuildBasis (const vec3_t normal, vec3_t tangent, vec3_t bitangent)
{
	vec3_t up = { 0.f, 0.f, 1.f };
	if (fabsf (normal[2]) > 0.95f)
		VectorSet (up, 1.f, 0.f, 0.f);
	CrossProduct (up, normal, tangent);
	VectorNormalizeFast (tangent);
	CrossProduct (normal, tangent, bitangent);
	VectorNormalizeFast (bitangent);
}

static float R_Decals_Random01 (void)
{
	return (float)rand () / (float)RAND_MAX;
}

static float R_Decals_RandomRange (float minval, float maxval)
{
	if (maxval <= minval)
		return minval;
	return minval + R_Decals_Random01 () * (maxval - minval);
}

static void R_Decals_SanitizeDef (decal_def_t *def)
{
	if (def->size_max < def->size_min)
	{
		float tmp = def->size_min;
		def->size_min = def->size_max;
		def->size_max = tmp;
	}
	if (def->alpha_max < def->alpha_min)
	{
		float tmp = def->alpha_min;
		def->alpha_min = def->alpha_max;
		def->alpha_max = tmp;
	}
	def->size_min = q_max (1.f, def->size_min);
	def->size_max = q_max (def->size_min, def->size_max);
	def->alpha_min = CLAMP (0.f, def->alpha_min, 1.f);
	def->alpha_max = CLAMP (def->alpha_min, def->alpha_max, 1.f);
	def->priority = CLAMP (0, def->priority, 16);
}

static void R_Decals_EmitQuad (const vec3_t center, const vec3_t right, const vec3_t up, vec3_t out_verts[4])
{
	VectorSubtract (center, right, out_verts[0]); VectorSubtract (out_verts[0], up, out_verts[0]);
	VectorSubtract (center, right, out_verts[1]); VectorAdd (out_verts[1], up, out_verts[1]);
	VectorAdd (center, right, out_verts[2]); VectorAdd (out_verts[2], up, out_verts[2]);
	VectorAdd (center, right, out_verts[3]); VectorSubtract (out_verts[3], up, out_verts[3]);
}

static const char *R_Decal_CategoryName (decal_category_t cat)
{
	switch (cat)
	{
	case DECAL_CAT_BULLET: return "bullet";
	case DECAL_CAT_BLOOD: return "blood";
	case DECAL_CAT_SCORCH: return "scorch";
	case DECAL_CAT_DIRT: return "dirt";
	default: return "generic";
	}
}

static int R_Decal_DefaultPriority (decal_category_t cat)
{
	switch (cat)
	{
	case DECAL_CAT_BULLET: return 4;
	case DECAL_CAT_SCORCH: return 3;
	case DECAL_CAT_DIRT: return 2;
	case DECAL_CAT_BLOOD: return 1;
	default: return 2;
	}
}

static float R_Decal_DefaultSize (decal_category_t cat)
{
	switch (cat)
	{
	case DECAL_CAT_BULLET: return 7.f;
	case DECAL_CAT_SCORCH: return 18.f;
	case DECAL_CAT_DIRT: return 10.f;
	case DECAL_CAT_BLOOD: return 9.f;
	default: return 8.f;
	}
}

static decal_category_t R_Decal_ParseCategory (const char *token)
{
	if (!q_strcasecmp (token, "bullet")) return DECAL_CAT_BULLET;
	if (!q_strcasecmp (token, "blood")) return DECAL_CAT_BLOOD;
	if (!q_strcasecmp (token, "scorch")) return DECAL_CAT_SCORCH;
	if (!q_strcasecmp (token, "dirt")) return DECAL_CAT_DIRT;
	return DECAL_CAT_GENERIC;
}

static decal_blend_t R_Decal_ParseBlend (const char *token)
{
	if (!q_strcasecmp (token, "add")) return DECAL_BLEND_ADD;
	if (!q_strcasecmp (token, "modulate")) return DECAL_BLEND_MODULATE;
	if (!q_strcasecmp (token, "premul")) return DECAL_BLEND_PREMUL;
	return DECAL_BLEND_ALPHA;
}

static int R_Decal_FindDefIndex (const char *name)
{
	int i;
	for (i = 0; i < decal_def_count; ++i)
		if (!q_strcasecmp (decal_defs[i].name, name))
			return i;
	return -1;
}

static decal_def_t *R_Decal_GetOrCreateDef (const char *name)
{
	int idx = R_Decal_FindDefIndex (name);
	decal_def_t *def;
	if (idx >= 0)
		return &decal_defs[idx];
	if (decal_def_count >= DECAL_MAX_DEFS)
		return NULL;
	def = &decal_defs[decal_def_count++];
	memset (def, 0, sizeof (*def));
	q_strlcpy (def->name, name, sizeof (def->name));
	def->size_min = def->size_max = 8.f;
	def->alpha_min = def->alpha_max = 0.8f;
	VectorSet (def->color, 1.f, 1.f, 1.f);
	def->random_rotation = true;
	def->blend = DECAL_BLEND_ALPHA;
	def->category = DECAL_CAT_GENERIC;
	def->priority = 2;
	return def;
}

static void R_Decals_AddDefaultDef (const char *name, const char *texture, decal_category_t category,
	float size_min, float size_max, float alpha_min, float alpha_max,
	float lifetime, float fade, int priority)
{
	decal_def_t *def;

	def = R_Decal_GetOrCreateDef (name);
	if (!def)
		return;

	q_strlcpy (def->texture_paths[0], texture, sizeof (def->texture_paths[0]));
	def->num_textures = 1;
	def->category = category;
	def->size_min = size_min;
	def->size_max = size_max;
	def->alpha_min = alpha_min;
	def->alpha_max = alpha_max;
	def->lifetime = lifetime;
	def->fade = fade;
	def->priority = priority;
}

static void R_Decals_AddDefaultDefs (void)
{
	R_Decals_AddDefaultDef ("bullet_hole_default", "decals/bullet/bhole_01", DECAL_CAT_BULLET, 5.f, 8.f, 0.65f, 0.9f, 32.f, 8.f, 4);
	R_Decals_AddDefaultDef ("bullet_hole_metal", "decals/bullet/bhole_metal_01", DECAL_CAT_BULLET, 4.f, 7.f, 0.6f, 0.85f, 28.f, 6.f, 4);
	R_Decals_AddDefaultDef ("blood_splat_small", "decals/blood/splat_01", DECAL_CAT_BLOOD, 5.f, 10.f, 0.45f, 0.75f, 22.f, 6.f, 1);
	R_Decals_AddDefaultDef ("blood_splat_large", "decals/blood/splat_02", DECAL_CAT_BLOOD, 10.f, 16.f, 0.35f, 0.65f, 26.f, 8.f, 1);
	R_Decals_AddDefaultDef ("scorch_small", "decals/scorch/scorch_01", DECAL_CAT_SCORCH, 12.f, 20.f, 0.45f, 0.7f, 36.f, 10.f, 3);
	R_Decals_AddDefaultDef ("scorch_large", "decals/scorch/scorch_02", DECAL_CAT_SCORCH, 20.f, 32.f, 0.35f, 0.6f, 40.f, 12.f, 3);
	R_Decals_AddDefaultDef ("dirt_hit", "decals/dirt/dirt_01", DECAL_CAT_DIRT, 8.f, 14.f, 0.4f, 0.65f, 24.f, 6.f, 2);
	R_Decals_AddDefaultDef ("plaster_hit", "decals/dirt/plaster_01", DECAL_CAT_DIRT, 7.f, 12.f, 0.45f, 0.7f, 24.f, 6.f, 2);
	R_Decals_AddDefaultDef ("concrete_hit", "decals/dirt/concrete_01", DECAL_CAT_DIRT, 7.f, 12.f, 0.45f, 0.7f, 24.f, 6.f, 2);
}

static void R_Decals_ParseFile (const char *path, const char *buffer)
{
	const char *data = buffer;
	while ((data = COM_Parse (data)))
	{
		decal_def_t *def;
		if (q_strcasecmp (com_token, "decal"))
			continue;
		data = COM_Parse (data);
		if (!data)
			break;
		def = R_Decal_GetOrCreateDef (com_token);
		if (!def)
			break;
		data = COM_Parse (data);
		if (!data || strcmp (com_token, "{"))
			break;
		while ((data = COM_Parse (data)))
		{
			if (!strcmp (com_token, "}"))
				break;
			if (!q_strcasecmp (com_token, "texture"))
			{
				data = COM_Parse (data);
				if (!data) break;
				q_strlcpy (def->texture_paths[0], com_token, sizeof (def->texture_paths[0]));
				def->num_textures = 1;
			}
			else if (!q_strcasecmp (com_token, "size"))
			{
				data = COM_Parse (data); if (!data) break; def->size_min = atof (com_token);
				data = COM_Parse (data); if (!data) break; def->size_max = atof (com_token);
			}
			else if (!q_strcasecmp (com_token, "alpha"))
			{
				data = COM_Parse (data); if (!data) break; def->alpha_min = atof (com_token);
				data = COM_Parse (data); if (!data) break; def->alpha_max = atof (com_token);
			}
			else if (!q_strcasecmp (com_token, "color"))
			{
				int i;
				for (i = 0; i < 3; ++i) { data = COM_Parse (data); if (!data) break; def->color[i] = atof (com_token); }
			}
			else if (!q_strcasecmp (com_token, "lifetime")) { data = COM_Parse (data); if (!data) break; def->lifetime = atof (com_token); }
			else if (!q_strcasecmp (com_token, "fade")) { data = COM_Parse (data); if (!data) break; def->fade = atof (com_token); }
			else if (!q_strcasecmp (com_token, "blend")) { data = COM_Parse (data); if (!data) break; def->blend = R_Decal_ParseBlend (com_token); }
			else if (!q_strcasecmp (com_token, "random_rotation")) { data = COM_Parse (data); if (!data) break; def->random_rotation = atof (com_token) != 0.f; }
			else if (!q_strcasecmp (com_token, "priority")) { data = COM_Parse (data); if (!data) break; def->priority = (int)atof (com_token); }
			else if (!q_strcasecmp (com_token, "category")) { data = COM_Parse (data); if (!data) break; def->category = R_Decal_ParseCategory (com_token); }
		}
	}
	Con_DPrintf ("decal script parsed: %s\n", path);
}

static qboolean R_Decals_IsScriptName (const char *name)
{
	if (q_strcasecmp (COM_FileGetExtension (name), "shader"))
		return false;
	if (!q_strncasecmp (name, "scripts/decals_", 15))
		return true;
	return !q_strcasecmp (name, "scripts/decals.shader");
}

static void R_Decals_LoadFromPack (searchpath_t *search)
{
	int i;
	pack_t *pak = search->pack;
	for (i = 0; i < pak->numfiles; ++i)
	{
		const char *name = pak->files[i].name;
		size_t size = pak->files[i].filelen;
		byte *buffer = NULL;
		if (!R_Decals_IsScriptName (name))
			continue;
		if (pak->is_pk3)
		{
			size_t extracted_size = 0;
			buffer = mz_zip_reader_extract_to_heap ((mz_zip_archive *)pak->zip, pak->files[i].filepos, &extracted_size, 0);
			size = extracted_size;
		}
		else
		{
			buffer = (byte *)malloc (size + 1);
			if (!buffer)
				continue;
			Sys_FileSeek (pak->handle, pak->files[i].filepos);
			if (Sys_FileRead (pak->handle, buffer, (int)size) != (int)size)
			{ free (buffer); continue; }
		}
		if (!buffer || !size)
			continue;
		buffer[size] = 0;
		R_Decals_ParseFile (name, (char *)buffer);
		if (pak->is_pk3) MZ_FREE (buffer); else free (buffer);
	}
}

static void R_Decals_LoadFromDir (searchpath_t *search)
{
	char script_dir[MAX_OSPATH];
	findfile_t *find;
	if ((size_t)q_snprintf (script_dir, sizeof (script_dir), "%s/scripts", search->filename) >= sizeof (script_dir))
		return;
	for (find = Sys_FindFirst (script_dir, "shader"); find; find = Sys_FindNext (find))
	{
		char relpath[MAX_QPATH], fullpath[MAX_OSPATH];
		byte *buffer;
		int handle, size;
		if (find->attribs & FA_DIRECTORY)
			continue;
		q_snprintf (relpath, sizeof (relpath), "scripts/%s", find->name);
		if (!R_Decals_IsScriptName (relpath))
			continue;
		q_snprintf (fullpath, sizeof (fullpath), "%s/%s", script_dir, find->name);
		size = Sys_FileOpenRead (fullpath, &handle);
		if (size <= 0) { if (handle >= 0) Sys_FileClose (handle); continue; }
		buffer = (byte *)malloc (size + 1);
		if (!buffer) { Sys_FileClose (handle); continue; }
		if (Sys_FileRead (handle, buffer, size) != size) { free(buffer); Sys_FileClose (handle); continue; }
		Sys_FileClose (handle);
		buffer[size] = 0;
		R_Decals_ParseFile (relpath, (char *)buffer);
		free (buffer);
	}
}

static void R_Decals_LoadScripts (void)
{
	searchpath_t *search;
	searchpath_t **paths = NULL;
	size_t count, i;
	decal_def_count = 0;
	memset (decal_warned_missing_tex, 0, sizeof (decal_warned_missing_tex));
	R_Decals_AddDefaultDefs ();
	for (search = com_searchpaths; search; search = search->next)
		VEC_PUSH (paths, search);
	count = VEC_SIZE (paths);
	for (i = count; i > 0; --i)
	{
		search = paths[i - 1];
		if (*search->filename) R_Decals_LoadFromDir (search);
		else if (search->pack) R_Decals_LoadFromPack (search);
	}
	for (i = 0; i < (size_t)decal_def_count; ++i)
		R_Decals_SanitizeDef (&decal_defs[i]);
	VEC_FREE (paths);
}

static gltexture_t *R_Decals_LoadTexture (decal_def_t *def, int def_index)
{
	int width, height;
	enum srcformat fmt;
	byte *data;
	if (def->num_textures <= 0)
		return NULL;
	if (!def->textures[0])
	{
		data = Image_LoadImage (def->texture_paths[0], &width, &height, &fmt);
		if (data)
		{
			def->textures[0] = TexMgr_LoadImage (NULL, def->texture_paths[0], width, height, fmt, data, def->texture_paths[0], 0,
				TEXPREF_ALPHA | TEXPREF_MIPMAP | TEXPREF_CLAMP);
			free (data);
		}
		else if (!decal_warned_missing_tex[def_index][0])
		{
			Con_Warning ("Decal texture missing: %s (%s)\n", def->texture_paths[0], def->name);
			decal_warned_missing_tex[def_index][0] = true;
		}
	}
	return def->textures[0];
}

static decal_instance_t *R_Decals_FindOverwriteSlot (int priority)
{
	int i;
	decal_instance_t *best = NULL;
	for (i = 0; i < decal_capacity; ++i)
	{
		decal_instance_t *d = &decal_pool[i];
		if (!d->active)
			return d;
		if (d->priority <= priority)
		{
			if (!best || d->spawn_id < best->spawn_id)
				best = d;
		}
	}
	return best;
}

static void R_Decals_InitInstance (decal_instance_t *d, const decal_def_t *def, const vec3_t center, const vec3_t verts[4], float alpha)
{
	memset (d, 0, sizeof (*d));
	d->active = true;
	d->priority = def->priority;
	d->category = def->category;
	d->blend = def->blend;
	d->spawn_time = cl.time;
	d->spawn_id = ++decal_next_spawn_id;
	d->frame_touched = decal_frame;
	VectorCopy (center, d->origin);
	memcpy (d->verts, verts, sizeof (d->verts));
	d->lifetime = def->lifetime > 0.f ? def->lifetime : q_max (1.f, r_decals_lifetime.value);
	d->fade = def->fade > 0.f ? def->fade : q_max (0.f, r_decals_fade.value);
	d->alpha = CLAMP (0.f, alpha, 1.f);
}

static void R_Decals_GetRotationAxes (const decal_def_t *def, float *out_c, float *out_s)
{
	if (def->random_rotation)
	{
		float angle = R_Decals_Random01 () * 2.f * (float)M_PI;
		*out_c = cosf (angle);
		*out_s = sinf (angle);
		return;
	}
	*out_c = 1.f;
	*out_s = 0.f;
}

static void R_Decals_BuildOrthonormalQuad (const vec3_t org, const vec3_t normal, const decal_def_t *def, float size_opt, vec3_t center, vec3_t out_verts[4])
{
	vec3_t nrm, tangent, bitangent;
	vec3_t right, up;
	float half;
	float c, s;

	VectorCopy (normal, nrm);
	if (VectorLengthSquared (nrm) < 0.01f)
		VectorSet (nrm, 0.f, 0.f, 1.f);
	VectorNormalizeFast (nrm);
	R_Decals_BuildBasis (nrm, tangent, bitangent);
	R_Decals_GetRotationAxes (def, &c, &s);

	half = (size_opt > 0.f) ? size_opt * 0.5f : R_Decals_RandomRange (def->size_min, def->size_max) * 0.5f;
	VectorMA (org, r_decals_bias.value, nrm, center);
	for (int i = 0; i < 3; ++i)
	{
		right[i] = (tangent[i] * c + bitangent[i] * s) * half;
		up[i] = (bitangent[i] * c - tangent[i] * s) * half;
	}
	R_Decals_EmitQuad (center, right, up, out_verts);
}

static float R_Decals_GetSpawnAlpha (const decal_def_t *def, const float *rgba_opt)
{
	float alpha = R_Decals_RandomRange (def->alpha_min, def->alpha_max);
	if (rgba_opt)
		alpha *= rgba_opt[3];
	return alpha;
}

void R_Decals_Add (const char *name, const vec3_t org, const vec3_t normal, const float *rgba_opt, float size_opt, int flags)
{
	int def_index;
	decal_def_t *def;
	decal_instance_t *d;
	vec3_t center;
	vec3_t verts[4];
	float alpha;
	(void)flags;

	if (!r_decals.value || !name || !name[0] || !decal_pool || !decal_capacity)
		return;
	if (decal_spawned_this_frame >= (int)r_decals_spawn_budget.value)
	{
		decal_stats.dropped_spawn_budget++;
		return;
	}
	def_index = R_Decal_FindDefIndex (name);
	if (def_index < 0)
		def_index = R_Decal_FindDefIndex ("bullet_hole_default");
	if (def_index < 0)
		return;
	def = &decal_defs[def_index];
	d = R_Decals_FindOverwriteSlot (def->priority);
	if (!d)
	{
		decal_stats.dropped_pool_overwrite++;
		return;
	}
	if (d->active)
		decal_stats.dropped_pool_overwrite++;

	R_Decals_BuildOrthonormalQuad (org, normal, def, size_opt, center, verts);
	alpha = R_Decals_GetSpawnAlpha (def, rgba_opt);
	R_Decals_InitInstance (d, def, center, verts, alpha);
	d->texture = R_Decals_LoadTexture (def, def_index);
	if (!d->texture)
	{
		d->active = false;
		return;
	}
	decal_stats.spawned++;
	decal_spawned_this_frame++;
}

static qboolean R_Decals_TraceNormal (const vec3_t point, vec3_t out_normal)
{
	trace_t trace;
	vec3_t end;
	if (!cl.worldmodel)
		return false;
	VectorMA (point, 32.f, vpn, end);
	memset (&trace, 0, sizeof (trace));
	trace.fraction = 1.f;
	SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0.f, 1.f, end, point, &trace);
	if (trace.fraction < 1.f && VectorLengthSquared (trace.plane.normal) > 0.001f)
	{
		VectorCopy (trace.plane.normal, out_normal);
		return true;
	}
	VectorSet (out_normal, 0.f, 0.f, 1.f);
	return false;
}

void R_AddBulletDecal (const vec3_t point)
{
	vec3_t n;
	R_Decals_TraceNormal (point, n);
	R_Decals_Add ("bullet_hole_default", point, n, NULL, 0.f, 0);
}

void R_AddBloodDecal (const vec3_t point, const vec3_t dir)
{
	vec3_t n;
	if ((rand () & 3) != 0)
		return;
	VectorScale (dir, -1.f, n);
	if (VectorLengthSquared (n) < 0.01f)
		R_Decals_TraceNormal (point, n);
	R_Decals_Add ((rand() & 1) ? "blood_splat_small" : "blood_splat_large", point, n, NULL, 0.f, 0);
}

void R_AddScorchDecal (const vec3_t point)
{
	vec3_t n;
	R_Decals_TraceNormal (point, n);
	R_Decals_Add ((rand() & 1) ? "scorch_small" : "scorch_large", point, n, NULL, 0.f, 0);
}

static int R_Decal_CompareRender (const void *a, const void *b)
{
	const decal_instance_t *da = *(const decal_instance_t * const *)a;
	const decal_instance_t *db = *(const decal_instance_t * const *)b;
	if (da->priority != db->priority)
		return (db->priority - da->priority);
	if (da->dist_sq < db->dist_sq) return -1;
	if (da->dist_sq > db->dist_sq) return 1;
	if (da->spawn_id > db->spawn_id) return -1;
	if (da->spawn_id < db->spawn_id) return 1;
	return 0;
}

static void R_Decals_SetBlend (decal_blend_t blend, qboolean showtris)
{
	if (showtris)
	{
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (2));
		return;
	}
	switch (blend)
	{
	default:
	case DECAL_BLEND_ALPHA: GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (2)); break;
	case DECAL_BLEND_ADD: GL_SetState (GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (2)); break;
	case DECAL_BLEND_MODULATE: GL_SetState (GLS_BLEND_MULTIPLY | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (2)); break;
	case DECAL_BLEND_PREMUL: GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (2)); break;
	}
}

static void R_Decals_FlushBatch (const decalvertex_t *verts, const GLushort *idx, int count, qboolean showtris, gltexture_t *tex, decal_blend_t blend)
{
	GLuint buf;
	GLbyte *ofs;

	if (count <= 0)
		return;
	GL_Bind (GL_TEXTURE0, showtris ? whitetexture : tex);
	R_Decals_SetBlend (blend, showtris);
	GL_Upload (GL_ARRAY_BUFFER, verts, sizeof (decalvertex_t) * count * 4, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (decalvertex_t), ofs + offsetof (decalvertex_t, pos));
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof (decalvertex_t), ofs + offsetof (decalvertex_t, uv));
	GL_Upload (GL_ELEMENT_ARRAY_BUFFER, idx, sizeof (GLushort) * count * 6, &buf, &ofs);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
	glDrawElements (GL_TRIANGLES, count * 6, GL_UNSIGNED_SHORT, ofs);
}

static void R_Decals_WriteBatchEntry (const decal_instance_t *d, decalvertex_t *verts, GLushort *idx, int batch_index)
{
	int k;
	for (k = 0; k < 4; ++k)
	{
		VectorCopy (d->verts[k], verts[batch_index * 4 + k].pos);
		verts[batch_index * 4 + k].uv[0] = (k == 0 || k == 1) ? 0.f : 1.f;
		verts[batch_index * 4 + k].uv[1] = (k == 0 || k == 3) ? 1.f : 0.f;
	}
	idx[batch_index * 6 + 0] = batch_index * 4 + 0;
	idx[batch_index * 6 + 1] = batch_index * 4 + 1;
	idx[batch_index * 6 + 2] = batch_index * 4 + 2;
	idx[batch_index * 6 + 3] = batch_index * 4 + 0;
	idx[batch_index * 6 + 4] = batch_index * 4 + 2;
	idx[batch_index * 6 + 5] = batch_index * 4 + 3;
}

static qboolean R_Decals_PrepareVisible (decal_instance_t *d, double now, float drawdist, float drawdist_sq)
{
	float age;
	float fade_start;
	vec3_t dv;

	if (!d->active)
		return false;
	age = (float)(now - d->spawn_time);
	if (age >= d->lifetime)
	{
		d->active = false;
		return false;
	}
	VectorSubtract (r_origin, d->origin, dv);
	d->dist_sq = DotProduct (dv, dv);
	if (drawdist > 0.f && d->dist_sq > drawdist_sq)
		return false;
	fade_start = q_max (0.f, d->lifetime - d->fade);
	if (age > fade_start && d->fade > 0.f)
		d->alpha = CLAMP (0.f, 1.f - (age - fade_start) / d->fade, 1.f);
	return d->alpha > 0.f;
}

void R_DrawDecals (qboolean showtris)
{
	decal_instance_t *visible[4096];
	decalvertex_t verts[4 * DECAL_BATCH_MAX];
	GLushort idx[6 * DECAL_BATCH_MAX];
	int vis_count = 0, draw_count = 0;
	int i, batch_count = 0;
	double now = cl.time;
	float drawdist = q_max (0.f, r_decals_drawdist.value);
	float drawdist_sq = drawdist * drawdist;
	int budget = CLAMP (0, (int)r_decals_render_budget_decals.value, 4096);
	gltexture_t *current_tex = NULL;
	decal_blend_t current_blend = DECAL_BLEND_ALPHA;

	if (!r_decals.value || !decal_pool || budget <= 0)
		return;

	for (i = 0; i < decal_capacity; ++i)
	{
		decal_instance_t *d = &decal_pool[i];
		if (R_Decals_PrepareVisible (d, now, drawdist, drawdist_sq))
			visible[vis_count++] = d;
	}

	if (!vis_count)
		return;
	qsort (visible, vis_count, sizeof (visible[0]), R_Decal_CompareRender);
	if (vis_count > budget)
		decal_stats.skipped_render_budget += (vis_count - budget);

	GL_BeginGroup ("Decals");
	GL_UseProgram (glprogs.sprites[softemu == SOFTEMU_COARSE && !showtris]);
	GL_PolygonOffset (OFFSET_DECAL);

	for (i = 0; i < vis_count && draw_count < budget; ++i)
	{
		decal_instance_t *d = visible[i];
		if (!d->texture)
			continue;
		if (!current_tex || current_tex != d->texture || current_blend != d->blend || batch_count >= DECAL_BATCH_MAX)
		{
			if (batch_count > 0)
				R_Decals_FlushBatch (verts, idx, batch_count, showtris, current_tex, current_blend);
			batch_count = 0;
			current_tex = d->texture;
			current_blend = d->blend;
		}
		R_Decals_WriteBatchEntry (d, verts, idx, batch_count);
		++batch_count;
		++draw_count;
	}
	if (batch_count > 0)
		R_Decals_FlushBatch (verts, idx, batch_count, showtris, current_tex, current_blend);
	GL_PolygonOffset (OFFSET_NONE);
	GL_EndGroup ();
	decal_stats.rendered = draw_count;
	if (r_decals_debug.value >= 1.f)
	{
		Con_DPrintf ("decals: active=%d spawned=%d drop_spawn=%d drop_pool=%d rendered=%d skip_budget=%d\n",
			vis_count, decal_stats.spawned, decal_stats.dropped_spawn_budget, decal_stats.dropped_pool_overwrite,
			decal_stats.rendered, decal_stats.skipped_render_budget);
	}
}

void R_DrawDecals_ShowTris (void)
{
	R_DrawDecals (true);
}

void R_Decals_Clear (void)
{
	if (decal_pool && decal_capacity > 0)
		memset (decal_pool, 0, sizeof (decal_pool[0]) * decal_capacity);
	decal_head = 0;
	decal_next_spawn_id = 0;
}

static void R_Decals_EnsurePool (void)
{
	int wanted = R_Decals_GetMax ();
	if (wanted == decal_capacity)
		return;
	free (decal_pool);
	decal_pool = NULL;
	decal_capacity = wanted;
	if (wanted > 0)
		decal_pool = (decal_instance_t *)calloc ((size_t)wanted, sizeof (decal_instance_t));
	R_Decals_Clear ();
}

static void R_Decals_Dump_f (void)
{
	for (int i = 0; i < decal_def_count; ++i)
	{
		decal_def_t *d = &decal_defs[i];
		Con_Printf ("%s tex=%s size=%.1f..%.1f alpha=%.2f..%.2f life=%.1f fade=%.1f pri=%d cat=%s\n",
			d->name, d->num_textures ? d->texture_paths[0] : "<none>", d->size_min, d->size_max,
			d->alpha_min, d->alpha_max, d->lifetime, d->fade, d->priority, R_Decal_CategoryName (d->category));
	}
}

static void R_Decals_Clear_f (void)
{
	R_Decals_Clear ();
}

static void R_Decals_ReloadCmd_f (void)
{
	R_Decals_ReloadScripts ();
}

static void R_Decals_Test_f (void)
{
	vec3_t end, hit, nrm;
	const char *name = (Cmd_Argc () > 1) ? Cmd_Argv (1) : "bullet_hole_default";
	VectorMA (r_refdef.vieworg, 256.f, vpn, end);
	TraceLine (r_refdef.vieworg, end, hit);
	R_Decals_TraceNormal (hit, nrm);
	R_Decals_Add (name, hit, nrm, NULL, 0.f, 0);
}

void R_Decals_Init (void)
{
	Cvar_RegisterVariable (&r_decals);
	Cvar_RegisterVariable (&r_decals_max);
	Cvar_RegisterVariable (&r_decals_drawdist);
	Cvar_RegisterVariable (&r_decals_lifetime);
	Cvar_RegisterVariable (&r_decals_fade);
	Cvar_RegisterVariable (&r_decals_bias);
	Cvar_RegisterVariable (&r_decals_spawn_budget);
	Cvar_RegisterVariable (&r_decals_render_budget_decals);
	Cvar_RegisterVariable (&r_decals_debug);
	Cvar_RegisterVariable (&r_decals_on_liquids);
	Cvar_RegisterVariable (&r_decals_reload);
	Cvar_SetCallback (&r_decals_max, R_Decals_Max_Changed);
	Cvar_SetCallback (&r_decals_spawn_budget, R_Decals_Budget_Changed);
	Cvar_SetCallback (&r_decals_render_budget_decals, R_Decals_Budget_Changed);
	Cvar_SetCallback (&r_decals_reload, R_Decals_Reload_f);

	Cmd_AddCommand ("decals_reload", R_Decals_ReloadCmd_f);
	Cmd_AddCommand ("decals_dump", R_Decals_Dump_f);
	Cmd_AddCommand ("decals_clear", R_Decals_Clear_f);
	Cmd_AddCommand ("decals_test", R_Decals_Test_f);

	R_Decals_EnsurePool ();
	R_Decals_LoadScripts ();
	R_Decals_ResetFrameStats ();
}

void R_Decals_Shutdown (void)
{
	free (decal_pool);
	decal_pool = NULL;
	decal_capacity = 0;
	decal_def_count = 0;
}

void R_Decals_ReloadScripts (void)
{
	Con_Printf ("Reloading decals\n");
	R_Decals_LoadScripts ();
}

void R_Decals_FrameBegin (void)
{
	decal_frame++;
	R_Decals_EnsurePool ();
	R_Decals_ResetFrameStats ();
}
