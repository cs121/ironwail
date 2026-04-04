/*
Copyright (C) 2024

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
#include "mat_material.h"
#include "mat_material_parse.h"
#include "miniz.h"
#include <math.h>
#include <stdarg.h>

#define MATERIAL_HASH_SIZE 256
#define MATERIAL_LIST_LIMIT 64
#define MAT_TEXMOD_PI 3.14159265358979323846f
#define MATERIAL_UNKNOWN_LIMIT 256

typedef struct mat_material_entry_s
{
	material_t	*material;
	unsigned int		hash;
	struct mat_material_entry_s *next;
} mat_material_entry_t;

typedef struct mat_material_warn_s
{
	char *token;
} mat_material_warn_t;

typedef struct mat_material_keyword_def_s
{
	const char *keyword;
	material_keyword_scope_t scope;
	material_keyword_status_t status;
	const char *notes;
} material_keyword_def_t;

typedef struct mat_material_unknown_s
{
	char *token;
	material_keyword_scope_t scope;
	unsigned int count;
	char *first_context;
	char *first_source;
	unsigned int first_line;
} mat_material_unknown_t;

static mat_material_entry_t *mat_material_hash[MATERIAL_HASH_SIZE];
static material_t **mat_material_list;
static mat_material_warn_t *mat_material_warned;
static mat_material_unknown_t *mat_material_unknowns;
static qboolean mat_material_loaded;
static qboolean mat_material_unknown_overflow;

static const char *Material_SortName (mat_sort_key_t key)
{
	switch (key)
	{
	case MAT_SORT_SKY:
		return "sky";
	case MAT_SORT_OPAQUE:
		return "opaque";
	case MAT_SORT_SEE_THROUGH:
		return "seeThrough";
	case MAT_SORT_DECAL:
		return "decal";
	case MAT_SORT_BANNER:
		return "banner";
	case MAT_SORT_UNDERWATER:
		return "underwater";
	case MAT_SORT_ADDITIVE:
		return "additive";
	case MAT_SORT_NEAREST:
		return "nearest";
	default:
		return "unknown";
	}
}

static const material_keyword_def_t mat_material_keyword_table[] =
{
	{ "qer_editorimage", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Editor preview texture." },
	{ "surfaceparm", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Surface flags; see surfaceparm scope." },
	{ "cull", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_PARTIAL, "Modes: back/front/none." },
	{ "sort", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_PARTIAL, "Keys: sky/opaque/seeThrough/decal/banner/underwater/additive/nearest or numeric." },
	{ "polygonOffset", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional boolean or factor/units pair." },
	{ "emissive", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "bloom", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "emissive_scale", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "bloom_scale", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "skyParms", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 sky parameters (deferred; non-MVP)." },
	{ "fogParms", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 fog parameters (deferred; non-MVP)." },
	{ "deformVertexes", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 vertex deformation (deferred; non-MVP)." },
	{ "q3map_*", MATERIAL_KEYWORD_SCOPE_TOPLEVEL, MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3Map compile-time directives (deferred; non-MVP)." },

	{ "map", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Supports $lightmap/$white/$black and textures." },
	{ "clampmap", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Clamp-wrapped texture." },
	{ "animMap", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "FPS + frame list only." },
	{ "rgbGen", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "Modes: identity/vertex/const/wave." },
	{ "alphaGen", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "Modes: identity/vertex/const/wave." },
	{ "blendFunc", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "Supports add/filter/blend/premult or explicit factors." },
	{ "depthWrite", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional boolean." },
	{ "depthFunc", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "Modes: lequal/equal/always." },
	{ "alphaFunc", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 alpha test (deferred; non-MVP)." },
	{ "tcGen", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "Modes: base/environment/lightmap." },
	{ "tcMod", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_PARTIAL, "Types: scroll/scale/rotate/turb/stretch." },
	{ "normalMap", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional per-stage normal texture path." },
	{ "specularMap", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional per-stage specular texture path." },
	{ "ormMap", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional per-stage occlusion/roughness/metallic texture path." },
	{ "normalScale", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional per-stage normal scaling factor (default 1)." },
	{ "specPower", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional per-stage specular power scalar (default 1)." },
	{ "specIntensity", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Optional per-stage specular intensity scalar (default 1)." },
	{ "emissive", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Stage-level emissive toggle." },
	{ "bloom", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Stage-level bloom toggle." },
	{ "emissiveScale", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Stage-level emissive scale." },
	{ "bloomScale", MATERIAL_KEYWORD_SCOPE_STAGE, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Stage-level bloom scale." },

	{ "solid", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Surface solid." },
	{ "nonsolid", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Surface non-solid." },
	{ "playerclip", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Blocks players." },
	{ "monsterclip", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Blocks monsters." },
	{ "trans", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Marks transparent." },
	{ "sky", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Sky surface." },
	{ "fog", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Fog surface." },
	{ "nodraw", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "No draw surface." },
	{ "stone", MATERIAL_KEYWORD_SCOPE_SURFACEPARM, MATERIAL_KEYWORD_STATUS_IMPLEMENTED, "Footstep hint." }
};

static qboolean mat_material_keyword_seen[countof (mat_material_keyword_table)];

cvar_t r_materials = { "r_materials", "1", CVAR_ARCHIVE };
cvar_t r_material_debug = { "r_material_debug", "0", CVAR_ARCHIVE };
cvar_t r_tcgen_debug = { "r_tcgen_debug", "0", CVAR_ARCHIVE };
cvar_t r_sun_visibility = { "r_sun_visibility", "0.35", CVAR_ARCHIVE };
cvar_t r_material_debug_parse = { "r_material_debug_parse", "0", CVAR_ARCHIVE };
cvar_t r_particles_material_strict = { "r_particles_material_strict", "0", CVAR_ARCHIVE };
static cvar_t r_reloadmaterials = { "r_reloadmaterials", "0", CVAR_NONE };
static cvar_t r_material_fuzz = { "r_material_fuzz", "0", CVAR_NONE };
static cvar_t r_material_report = { "r_material_report", "0", CVAR_NONE };

char *Material_DupString (const char *value)
{
	size_t len;
	char *out;

	if (!value)
		return NULL;

	len = strlen (value) + 1;
	out = (char *) Z_Malloc (len);
	memcpy (out, value, len);
	return out;
}

static void Material_FreeStageData (material_stage_t *stage)
{
	if (!stage)
		return;
	if (stage->map_path)
	{
		Z_Free (stage->map_path);
		stage->map_path = NULL;
	}
	if (stage->normal_map_path)
	{
		Z_Free (stage->normal_map_path);
		stage->normal_map_path = NULL;
	}
	if (stage->specular_map_path)
	{
		Z_Free (stage->specular_map_path);
		stage->specular_map_path = NULL;
	}
	if (stage->orm_map_path)
	{
		Z_Free (stage->orm_map_path);
		stage->orm_map_path = NULL;
	}
	if (stage->anim_map_frames)
	{
		size_t j;
		for (j = 0; j < VEC_SIZE (stage->anim_map_frames); ++j)
		{
			if (stage->anim_map_frames[j])
				Z_Free (stage->anim_map_frames[j]);
		}
		VEC_FREE (stage->anim_map_frames);
	}
}

static void Material_FreeMaterial (material_t *material)
{
	size_t i;

	if (!material)
		return;

	if (material->stages)
	{
		for (i = 0; i < VEC_SIZE (material->stages); ++i)
			Material_FreeStageData (&material->stages[i]);
		VEC_FREE (material->stages);
		/* stage0 is a value copy of stages[0]; its map_path/anim_map_frames
		   are owned by stages[0] and already freed above. Clear the pointers
		   to prevent any accidental use-after-free. */
		material->stage0.map_path = NULL;
		material->stage0.normal_map_path = NULL;
		material->stage0.specular_map_path = NULL;
		material->stage0.orm_map_path = NULL;
		material->stage0.anim_map_frames = NULL;
	}
	else
	{
		/* No stages[] array: stage0 owns its own allocations. */
		Material_FreeStageData (&material->stage0);
	}

	if (material->editor_image)
		Z_Free (material->editor_image);
	if (material->name)
		Z_Free (material->name);
	if (material->source_file)
		Z_Free (material->source_file);

	Z_Free (material);
}

static void Material_Reset (void)
{
	size_t i;

	for (i = 0; i < VEC_SIZE (mat_material_list); ++i)
		Material_FreeMaterial (mat_material_list[i]);

	VEC_FREE (mat_material_list);

	for (i = 0; i < countof (mat_material_hash); ++i)
	{
		mat_material_entry_t *entry = mat_material_hash[i];
		while (entry)
		{
			mat_material_entry_t *next = entry->next;
			Z_Free (entry);
			entry = next;
		}
		mat_material_hash[i] = NULL;
	}

	for (i = 0; i < VEC_SIZE (mat_material_warned); ++i)
	{
		if (mat_material_warned[i].token)
			Z_Free (mat_material_warned[i].token);
	}
	VEC_FREE (mat_material_warned);

	for (i = 0; i < VEC_SIZE (mat_material_unknowns); ++i)
	{
		if (mat_material_unknowns[i].token)
			Z_Free (mat_material_unknowns[i].token);
		if (mat_material_unknowns[i].first_context)
			Z_Free (mat_material_unknowns[i].first_context);
		if (mat_material_unknowns[i].first_source)
			Z_Free (mat_material_unknowns[i].first_source);
	}
	VEC_FREE (mat_material_unknowns);
	mat_material_unknown_overflow = false;

	memset (mat_material_keyword_seen, 0, sizeof (mat_material_keyword_seen));
}

static unsigned int Material_Hash (const char *name)
{
	return COM_HashString (name) % MATERIAL_HASH_SIZE;
}

static qboolean Material_TokenWarned (const char *token)
{
	size_t i;

	for (i = 0; i < VEC_SIZE (mat_material_warned); ++i)
	{
		if (!q_strcasecmp (mat_material_warned[i].token, token))
			return true;
	}

	return false;
}

static void Material_AddWarned (const char *token)
{
	mat_material_warn_t warn;

	memset (&warn, 0, sizeof (warn));
	warn.token = Material_DupString (token);
	VEC_PUSH (mat_material_warned, warn);
}

static void Material_WarnOnce (const char *warn_key, const char *token, const char *context)
{
	if (Material_TokenWarned (warn_key))
		return;

	Material_AddWarned (warn_key);
	Material_ParseAddWarning ();
	Con_Warning ("Material: unknown token '%s' in %s\n", token, context ? context : "material");
}

static void Material_FormatSourceLine (char *buffer, size_t buffer_size, const char *source_file, unsigned int line)
{
	if (!buffer || buffer_size == 0)
		return;

	if (source_file && source_file[0])
	{
		if (line > 0)
			q_snprintf (buffer, buffer_size, "%s:%u", source_file, line);
		else
			q_snprintf (buffer, buffer_size, "%s", source_file);
		return;
	}

	if (line > 0)
		q_snprintf (buffer, buffer_size, "<unknown source>:%u", line);
	else
		q_snprintf (buffer, buffer_size, "<unknown source>");
}

static const char *Material_ScopeName (material_keyword_scope_t scope)
{
	switch (scope)
	{
	case MATERIAL_KEYWORD_SCOPE_TOPLEVEL:
		return "top-level";
	case MATERIAL_KEYWORD_SCOPE_STAGE:
		return "stage";
	case MATERIAL_KEYWORD_SCOPE_SURFACEPARM:
		return "surfaceparm";
	default:
		return "unknown";
	}
}

static const char *Material_StatusName (material_keyword_status_t status)
{
	switch (status)
	{
	case MATERIAL_KEYWORD_STATUS_IMPLEMENTED:
		return "Implemented";
	case MATERIAL_KEYWORD_STATUS_PARTIAL:
		return "Partial";
	case MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED:
		return "Known-but-Unimplemented";
	default:
		return "Unknown";
	}
}

void Material_MarkKeywordSeen (const char *keyword, material_keyword_scope_t scope)
{
	size_t i;

	if (!keyword || !keyword[0])
		return;

	for (i = 0; i < countof (mat_material_keyword_table); ++i)
	{
		if (mat_material_keyword_table[i].scope != scope)
			continue;
		if (!q_strcasecmp (mat_material_keyword_table[i].keyword, keyword))
		{
			mat_material_keyword_seen[i] = true;
			return;
		}
	}
}

static void Material_RecordUnknownToken (const char *token, material_keyword_scope_t scope, const char *context, const char *source_file, unsigned int line)
{
	size_t i;

	if (!token || !token[0])
		return;

	for (i = 0; i < VEC_SIZE (mat_material_unknowns); ++i)
	{
		if (mat_material_unknowns[i].scope == scope && !q_strcasecmp (mat_material_unknowns[i].token, token))
		{
			mat_material_unknowns[i].count++;
			return;
		}
	}

	if (VEC_SIZE (mat_material_unknowns) >= MATERIAL_UNKNOWN_LIMIT)
	{
		mat_material_unknown_overflow = true;
		return;
	}

	{
		mat_material_unknown_t entry;

		memset (&entry, 0, sizeof (entry));
		entry.token = Material_DupString (token);
		entry.scope = scope;
		entry.count = 1;
		entry.first_context = Material_DupString (context ? context : "");
		entry.first_source = Material_DupString (source_file ? source_file : "");
		entry.first_line = line;
		VEC_PUSH (mat_material_unknowns, entry);
	}
}

static void Material_ReportAppend (char **buffer, size_t *len, size_t *cap, const char *fmt, ...)
{
	char temp[2048];
	va_list args;
	size_t add;

	if (!buffer || !len || !cap)
		return;

	va_start (args, fmt);
	q_vsnprintf (temp, sizeof (temp), fmt, args);
	va_end (args);

	add = strlen (temp);
	if (*len + add + 1 > *cap)
	{
		size_t newcap = *cap ? *cap : 1024;
		char *newbuf;
		while (newcap < *len + add + 1)
			newcap *= 2;
		newbuf = (char *) q_realloc(*buffer, newcap);
		if (!newbuf)
			return;
		*buffer = newbuf;
		*cap = newcap;
	}

	memcpy (*buffer + *len, temp, add);
	*len += add;
	(*buffer)[*len] = '\0';
}

typedef struct mat_material_keyword_row_s
{
	const material_keyword_def_t *def;
	qboolean seen;
} material_keyword_row_t;

static int Material_CompareKeywordRows (const void *a, const void *b)
{
	const material_keyword_row_t *left = (const material_keyword_row_t *) a;
	const material_keyword_row_t *right = (const material_keyword_row_t *) b;
	int scope_cmp = (int)left->def->scope - (int)right->def->scope;

	if (scope_cmp != 0)
		return scope_cmp;
	return q_strcasecmp (left->def->keyword, right->def->keyword);
}

static int Material_CompareUnknowns (const void *a, const void *b)
{
	const mat_material_unknown_t *const *left = (const mat_material_unknown_t *const *) a;
	const mat_material_unknown_t *const *right = (const mat_material_unknown_t *const *) b;
	int scope_cmp = (int)(*left)->scope - (int)(*right)->scope;

	if (scope_cmp != 0)
		return scope_cmp;
	return q_strcasecmp ((*left)->token, (*right)->token);
}

static int Material_CompareUnknownCountDesc (const void *a, const void *b)
{
	const mat_material_unknown_t *const *left = (const mat_material_unknown_t *const *) a;
	const mat_material_unknown_t *const *right = (const mat_material_unknown_t *const *) b;

	if ((*left)->count != (*right)->count)
		return (*left)->count > (*right)->count ? -1 : 1;
	return q_strcasecmp ((*left)->token, (*right)->token);
}

static void Material_ReportKeywords (char **buffer, size_t *len, size_t *cap, const material_keyword_row_t *rows, size_t row_count, material_keyword_status_t status)
{
	size_t i;
	int printed = 0;

	Material_ReportAppend (buffer, len, cap, "## %s\n", Material_StatusName (status));

	for (i = 0; i < row_count; ++i)
	{
		const material_keyword_row_t *row = &rows[i];
		if (row->def->status != status)
			continue;
		printed++;
		Material_ReportAppend (buffer, len, cap, "- `%s` (%s) â€” Seen: %s",
			row->def->keyword,
			Material_ScopeName (row->def->scope),
			row->seen ? "yes" : "no");
		if (row->def->notes && row->def->notes[0])
			Material_ReportAppend (buffer, len, cap, ". Notes: %s", row->def->notes);
		Material_ReportAppend (buffer, len, cap, "\n");
	}

	if (!printed)
		Material_ReportAppend (buffer, len, cap, "_None._\n");

	Material_ReportAppend (buffer, len, cap, "\n");
}

static void Material_WriteReport (void)
{
	const char *path = "docs/material_report.md";
	const size_t keyword_count = countof (mat_material_keyword_table);
	material_keyword_row_t *rows = NULL;
	mat_material_unknown_t **unknown_rows = NULL;
	size_t unknown_count = VEC_SIZE (mat_material_unknowns);
	char *report = NULL;
	size_t len = 0;
	size_t cap = 0;
	size_t i;

	rows = (material_keyword_row_t *) q_malloc(keyword_count * sizeof (*rows));
	if (!rows)
		return;

	for (i = 0; i < keyword_count; ++i)
	{
		rows[i].def = &mat_material_keyword_table[i];
		rows[i].seen = mat_material_keyword_seen[i];
	}
	qsort (rows, keyword_count, sizeof (*rows), Material_CompareKeywordRows);

	Material_ReportAppend (&report, &len, &cap, "# Material Shader Keyword Report\n\n");
	Material_ReportAppend (&report, &len, &cap, "Tracked keywords are grouped by implementation status and scope.\n\n");

	Material_ReportKeywords (&report, &len, &cap, rows, keyword_count, MATERIAL_KEYWORD_STATUS_IMPLEMENTED);
	Material_ReportKeywords (&report, &len, &cap, rows, keyword_count, MATERIAL_KEYWORD_STATUS_PARTIAL);
	Material_ReportKeywords (&report, &len, &cap, rows, keyword_count, MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED);

	Material_ReportAppend (&report, &len, &cap, "## Unknown-Seen\n");
	if (unknown_count > 0)
	{
		unknown_rows = (mat_material_unknown_t **) q_malloc(unknown_count * sizeof (*unknown_rows));
		if (unknown_rows)
		{
			for (i = 0; i < unknown_count; ++i)
				unknown_rows[i] = &mat_material_unknowns[i];
			qsort (unknown_rows, unknown_count, sizeof (*unknown_rows), Material_CompareUnknowns);

			for (i = 0; i < unknown_count; ++i)
			{
				const mat_material_unknown_t *entry = unknown_rows[i];
				char location[MAX_QPATH * 2];
				Material_FormatSourceLine (location, sizeof (location), entry->first_source, entry->first_line);
				Material_ReportAppend (&report, &len, &cap,
					"- `%s` (%s) â€” Count: %u. First seen in %s (material: %s)\n",
					entry->token,
					Material_ScopeName (entry->scope),
					entry->count,
					location,
					entry->first_context && entry->first_context[0] ? entry->first_context : "<unknown>");
			}
		}
		else
		{
			for (i = 0; i < unknown_count; ++i)
			{
				const mat_material_unknown_t *entry = &mat_material_unknowns[i];
				char location[MAX_QPATH * 2];
				Material_FormatSourceLine (location, sizeof (location), entry->first_source, entry->first_line);
				Material_ReportAppend (&report, &len, &cap,
					"- `%s` (%s) â€” Count: %u. First seen in %s (material: %s)\n",
					entry->token,
					Material_ScopeName (entry->scope),
					entry->count,
					location,
					entry->first_context && entry->first_context[0] ? entry->first_context : "<unknown>");
			}
		}
		if (mat_material_unknown_overflow)
			Material_ReportAppend (&report, &len, &cap, "- _Unknown token limit reached; additional tokens omitted._\n");
	}
	else
	{
		Material_ReportAppend (&report, &len, &cap, "_None._\n");
	}
	Material_ReportAppend (&report, &len, &cap, "\n");

	Material_ReportAppend (&report, &len, &cap, "## Wishlist / reference (Q3 keywords)\n");
	Material_ReportAppend (&report, &len, &cap, "- blendFunc (add, filter, blend, custom factors)\n");
	Material_ReportAppend (&report, &len, &cap, "- rgbGen modes (identity, identityLighting, entity, oneMinusEntity, vertex, exactVertex, lightingDiffuse)\n");
	Material_ReportAppend (&report, &len, &cap, "- alphaGen modes (identity, entity, oneMinusEntity, vertex, lightingSpecular, portal, wave)\n");
	Material_ReportAppend (&report, &len, &cap, "- tcGen / tcMod (base, lightmap, environment, vector; scroll/scale/rotate/stretch/transform/turb)\n");
	Material_ReportAppend (&report, &len, &cap, "- animMap / clampmap\n");
	Material_ReportAppend (&report, &len, &cap, "- depthFunc / depthWrite / alphaFunc\n");
	Material_ReportAppend (&report, &len, &cap, "- cull / sort / polygonOffset\n");
	Material_ReportAppend (&report, &len, &cap, "- deformVertexes\n");
	Material_ReportAppend (&report, &len, &cap, "- skyParms / fogParms / q3map_*\n");

	if (report && len > 0)
		COM_WriteFile (path, report, (int)len);

	q_free(unknown_rows);
	q_free(rows);
	q_free(report);
}

static qboolean Material_ShouldWriteReport (void)
{
#if defined(_DEBUG) || defined(DEBUG)
	return true;
#else
	return r_material_report.value > 0.f;
#endif
}

static const char *Material_MapTypeName (mat_map_type_t map_type)
{
	switch (map_type)
	{
	case MAT_MAP_CLAMPMAP:
		return "clampmap";
	case MAT_MAP_LIGHTMAP:
		return "lightmap";
	case MAT_MAP_WHITE:
		return "white";
	case MAT_MAP_BLACK:
		return "black";
	case MAT_MAP_MAP:
	default:
		return "map";
	}
}

static void Material_LogUnknownSummary (void)
{
	mat_material_unknown_t **unknown_rows = NULL;
	size_t unknown_count = VEC_SIZE (mat_material_unknowns);
	size_t i;
	size_t limit = 10;

	if (!unknown_count)
		return;

	unknown_rows = (mat_material_unknown_t **) q_malloc(unknown_count * sizeof (*unknown_rows));
	if (!unknown_rows)
		return;

	for (i = 0; i < unknown_count; ++i)
		unknown_rows[i] = &mat_material_unknowns[i];

	qsort (unknown_rows, unknown_count, sizeof (*unknown_rows), Material_CompareUnknownCountDesc);

	Con_Printf ("Material: Top %zu unknown keywords\n", q_min (limit, unknown_count));
	for (i = 0; i < unknown_count && i < limit; ++i)
	{
		const mat_material_unknown_t *entry = unknown_rows[i];
		Con_Printf ("  %s (%s) x%u\n", entry->token, Material_ScopeName (entry->scope), entry->count);
	}

	q_free(unknown_rows);
}

static void Material_LogSummary (size_t parsed_total)
{
	size_t warning_count = Material_ParseGetWarnings ();
	size_t error_count = Material_ParseGetErrors ();
	size_t unknown_count = VEC_SIZE (mat_material_unknowns);

	Con_Printf ("Material: parsed %zu shaders (%zu errors, %zu warnings, %zu unknown unique)\n",
		parsed_total, error_count, warning_count, unknown_count);
	if (mat_material_unknown_overflow)
		Con_Printf ("Material: unknown token limit reached; additional tokens omitted\n");
	Material_LogUnknownSummary ();
}

static void Material_DebugDumpShaders (void)
{
	size_t count = VEC_SIZE (mat_material_list);
	size_t i;

	Con_Printf ("Material: Debug dump (%zu shaders)\n", count);
	for (i = 0; i < count; ++i)
	{
		const material_t *material = mat_material_list[i];
		size_t stage_count = VEC_SIZE (material->stages);

		Con_Printf ("Material: %s\n", material->name);
		Con_Printf ("  flags: surface=0x%08x render=0x%08x content=0x%08x\n",
			material->surfaceparms, material->render_flags, material->content_flags);

		if (stage_count == 0 && (material->stage0.map_path || material->stage0.map_type != MAT_MAP_MAP))
			stage_count = 1;
		Con_Printf ("  stages: %zu\n", stage_count);

		for (size_t stage_index = 0; stage_index < stage_count; ++stage_index)
		{
			const material_stage_t *stage = material->stages ? &material->stages[stage_index] : &material->stage0;
			const char *map_name = stage->map_path && stage->map_path[0] ? stage->map_path : Material_MapTypeName (stage->map_type);

			Con_Printf ("    stage %zu: %s\n", stage_index, map_name);
		}
	}
}

static void Material_DebugDumpUnknowns (void)
{
	size_t unknown_count = VEC_SIZE (mat_material_unknowns);
	size_t i;

	Con_Printf ("Material: Unknown keywords (%zu)\n", unknown_count);
	for (i = 0; i < unknown_count; ++i)
	{
		const mat_material_unknown_t *entry = &mat_material_unknowns[i];
		char location[MAX_QPATH * 2];

		Material_FormatSourceLine (location, sizeof (location), entry->first_source, entry->first_line);
		Con_Printf ("  %s (%s) at %s\n", entry->token, Material_ScopeName (entry->scope), location);
	}
}

static void Mat_MatrixIdentity (mat_texmatrix_t *out)
{
	if (!out)
		return;
	memset (out, 0, sizeof (*out));
	out->m[0][0] = 1.f;
	out->m[1][1] = 1.f;
	out->m[2][2] = 1.f;
}

static void Mat_MatrixMultiply (mat_texmatrix_t *out, const mat_texmatrix_t *a, const mat_texmatrix_t *b)
{
	mat_texmatrix_t result;
	int r;
	int c;

	for (r = 0; r < 3; ++r)
	{
		for (c = 0; c < 3; ++c)
		{
			result.m[r][c] = a->m[r][0] * b->m[0][c] + a->m[r][1] * b->m[1][c] + a->m[r][2] * b->m[2][c];
		}
	}

	*out = result;
}

static void Mat_MatrixTranslate (mat_texmatrix_t *out, float s, float t)
{
	Mat_MatrixIdentity (out);
	out->m[0][2] = s;
	out->m[1][2] = t;
}

static void Mat_MatrixScale (mat_texmatrix_t *out, float s, float t)
{
	Mat_MatrixIdentity (out);
	out->m[0][0] = s;
	out->m[1][1] = t;
}

static void Mat_MatrixRotate (mat_texmatrix_t *out, float degrees)
{
	float radians = degrees * (float)M_PI / 180.f;
	float c = cosf (radians);
	float s = sinf (radians);

	Mat_MatrixIdentity (out);
	out->m[0][0] = c;
	out->m[0][1] = -s;
	out->m[1][0] = s;
	out->m[1][1] = c;
}

static void Mat_MatrixAroundCenter (mat_texmatrix_t *out, const mat_texmatrix_t *inner)
{
	mat_texmatrix_t tmp;
	mat_texmatrix_t translate_to;
	mat_texmatrix_t translate_back;

	Mat_MatrixTranslate (&translate_to, -0.5f, -0.5f);
	Mat_MatrixTranslate (&translate_back, 0.5f, 0.5f);
	Mat_MatrixMultiply (&tmp, inner, &translate_to);
	Mat_MatrixMultiply (out, &translate_back, &tmp);
}

static void Material_Register (material_t *material)
{
	mat_material_entry_t *entry;
	unsigned int bucket;

	entry = (mat_material_entry_t *) Z_Malloc (sizeof (*entry));
	entry->material = material;
	entry->hash = Material_Hash (material->name);
	bucket = entry->hash;
	entry->next = mat_material_hash[bucket];
	mat_material_hash[bucket] = entry;

	VEC_PUSH (mat_material_list, material);
}

static void Material_Unregister (const material_t *material)
{
	unsigned int bucket;
	mat_material_entry_t **entry;
	size_t i;

	if (!material)
		return;

	bucket = Material_Hash (material->name);
	entry = &mat_material_hash[bucket];
	while (*entry)
	{
		if ((*entry)->material == material)
		{
			mat_material_entry_t *next = (*entry)->next;
			Z_Free (*entry);
			*entry = next;
			break;
		}
		entry = &(*entry)->next;
	}

	for (i = 0; i < VEC_SIZE (mat_material_list); ++i)
	{
		if (mat_material_list[i] == material)
		{
			mat_material_list[i] = VEC_LAST (mat_material_list);
			VEC_POP (mat_material_list);
			break;
		}
	}
}

static const material_t *Material_FindInternal (const char *name)
{
	unsigned int bucket;
	mat_material_entry_t *entry;

	if (!name || !name[0])
		return NULL;

	bucket = Material_Hash (name);
	for (entry = mat_material_hash[bucket]; entry; entry = entry->next)
	{
		if (!q_strcasecmp (entry->material->name, name))
			return entry->material;
	}

	return NULL;
}

static int Material_ReadFile (const char *path, const byte *data, size_t size, const char *label)
{
	char *buffer;
	int parsed;

	if (!data || size == 0)
		return 0;

	buffer = (char *) Z_Malloc (size + 1);
	memcpy (buffer, data, size);
	buffer[size] = '\0';
	parsed = Material_ParseFile (path, buffer, label);
	Z_Free (buffer);

	return parsed;
}

static size_t Material_LoadFromDirectory (const searchpath_t *search)
{
	char script_dir[MAX_OSPATH];
	findfile_t *find;
	size_t parsed_total = 0;

	if ((size_t) q_snprintf (script_dir, sizeof (script_dir), "%s/materials", search->filename) >= sizeof (script_dir))
		return 0;

	for (find = Sys_FindFirst (script_dir, "material"); find; find = Sys_FindNext (find))
	{
		char fullpath[MAX_OSPATH];
		char relpath[MAX_QPATH];
		byte *buffer;
		int handle;
		int size;

		if (find->attribs & FA_DIRECTORY)
			continue;

		q_snprintf (relpath, sizeof (relpath), "materials/%s", find->name);
		q_snprintf (fullpath, sizeof (fullpath), "%s/%s", script_dir, find->name);

		size = (int) Sys_FileOpenRead (fullpath, &handle);
		if (size <= 0)
		{
			if (handle >= 0)
				Sys_FileClose (handle);
			continue;
		}

		buffer = (byte *) q_malloc(size + 1);
		if (!buffer)
		{
			Sys_FileClose (handle);
			continue;
		}

		if (Sys_FileRead (handle, buffer, size) != size)
		{
			q_free(buffer);
			Sys_FileClose (handle);
			continue;
		}

		Sys_FileClose (handle);
		buffer[size] = '\0';

		parsed_total += (size_t)Material_ReadFile (relpath, buffer, (size_t) size, relpath);

		q_free(buffer);
	}

	return parsed_total;
}

static size_t Material_LoadFromPack (const searchpath_t *search)
{
	int i;
	size_t parsed_total = 0;
	pack_t *pak = search->pack;

	for (i = 0; i < pak->numfiles; ++i)
	{
		const char *name = pak->files[i].name;
		size_t size = pak->files[i].filelen;
		byte *buffer = NULL;

		if (q_strncasecmp (name, "materials/", 8))
			continue;
		if (q_strcasecmp (COM_FileGetExtension (name), "material"))
			continue;

		if (pak->is_pk3)
		{
			size_t extracted_size = 0;
			void *extracted = mz_zip_reader_extract_to_heap ((mz_zip_archive *) pak->zip, pak->files[i].filepos, &extracted_size, 0);
			if (!extracted || extracted_size == 0)
				continue;
			buffer = (byte *) extracted;
			size = extracted_size;
		}
		else
		{
			buffer = (byte *) q_malloc(size + 1);
			if (!buffer)
				continue;
			Sys_FileSeek (pak->handle, pak->files[i].filepos);
			if (Sys_FileRead (pak->handle, buffer, (int) size) != (int) size)
			{
				q_free(buffer);
				continue;
			}
		}

		if (buffer)
		{
			buffer[size] = '\0';
			parsed_total += (size_t)Material_ReadFile (name, buffer, size, name);

			if (pak->is_pk3)
				MZ_FREE (buffer);
			else
				q_free(buffer);
		}
	}

	return parsed_total;
}

static void Material_LoadAll (void)
{
	searchpath_t *search;
	searchpath_t **paths = NULL;
	size_t count = 0;
	size_t i;
	size_t parsed_total = 0;

	if (mat_material_loaded && r_reloadmaterials.value <= 0.f)
		return;

	Material_Reset ();
	Material_ParseResetStats ();
	mat_material_loaded = true;

	if (r_materials.value <= 0.f)
		return;

	for (search = com_searchpaths; search; search = search->next)
	{
		VEC_PUSH (paths, search);
	}

	count = VEC_SIZE (paths);
	for (i = count; i > 0; --i)
	{
		search = paths[i - 1];
		if (*search->filename)
			parsed_total += Material_LoadFromDirectory (search);
		else if (search->pack)
			parsed_total += Material_LoadFromPack (search);
	}

	VEC_FREE (paths);

	if (Material_ShouldWriteReport ())
		Material_WriteReport ();

	Material_LogSummary (parsed_total);
	if (r_material_debug_parse.value > 0.f)
	{
		Material_DebugDumpShaders ();
		Material_DebugDumpUnknowns ();
	}
}

static void Material_Reload_f (cvar_t *var)
{
	if (var->value <= 0.f)
		return;
	Con_Printf ("Reloading materials\n");
	Cvar_SetValue ("r_reloadmaterials", 0.f);
	Material_LoadAll ();
}

static void Material_Fuzz_f (cvar_t *var)
{
	if (var->value <= 0.f)
		return;
	Material_DebugFuzzParse ();
	Cvar_SetValue ("r_material_fuzz", 0.f);
}

static void Material_List_f (void)
{
	size_t count = Material_Count ();
	size_t limit = MATERIAL_LIST_LIMIT;
	size_t i;

	if (Cmd_Argc () > 1)
		limit = (size_t) q_max (0, Q_atoi (Cmd_Argv (1)));

	Con_Printf ("Loaded shaders: %zu\n", count);
	for (i = 0; i < count && i < limit; ++i)
	{
		const material_t *material = Material_GetByIndex (i);
		Con_Printf ("%s\n", material->name);
	}
}

static void Material_Print_f (void)
{
	const material_t *material;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("Usage: materialprint <name>\n");
		return;
	}

	material = Material_Find (Cmd_Argv (1));
	if (!material)
	{
		Con_Printf ("No material named '%s'\n", Cmd_Argv (1));
		return;
	}

	Material_Print (material);
}

static void Material_FuzzCommand_f (void)
{
	Material_DebugFuzzParse ();
}


mat_particle_stage_support_t Material_ClassifyParticleStage (const material_stage_t *stage,
	mat_particle_policy_t policy, char *reason, size_t reason_size)
{
	int i;
	qboolean strict = (policy == MAT_PARTICLE_POLICY_STRICT);

	if (reason && reason_size)
		reason[0] = '\0';

	if (!stage)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "missing stage", reason_size);
		return MAT_PARTICLE_STAGE_HARD_FAIL;
	}

	if (stage->map_type == MAT_MAP_LIGHTMAP)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "map $lightmap unsupported for particles", reason_size);
		return strict ? MAT_PARTICLE_STAGE_HARD_FAIL : MAT_PARTICLE_STAGE_SKIPPED;
	}

	if (stage->map_type != MAT_MAP_MAP && stage->map_type != MAT_MAP_CLAMPMAP &&
		stage->map_type != MAT_MAP_WHITE && stage->map_type != MAT_MAP_BLACK)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "unsupported map type", reason_size);
		return strict ? MAT_PARTICLE_STAGE_HARD_FAIL : MAT_PARTICLE_STAGE_SKIPPED;
	}

	if (stage->rgbgen != MAT_RGBGEN_IDENTITY && stage->rgbgen != MAT_RGBGEN_VERTEX &&
		stage->rgbgen != MAT_RGBGEN_CONST && stage->rgbgen != MAT_RGBGEN_WAVE)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "unsupported rgbGen", reason_size);
		return strict ? MAT_PARTICLE_STAGE_HARD_FAIL : MAT_PARTICLE_STAGE_SKIPPED;
	}

	if (stage->alphagen != MAT_ALPHAGEN_IDENTITY && stage->alphagen != MAT_ALPHAGEN_VERTEX &&
		stage->alphagen != MAT_ALPHAGEN_CONST && stage->alphagen != MAT_ALPHAGEN_WAVE)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "unsupported alphaGen", reason_size);
		return strict ? MAT_PARTICLE_STAGE_HARD_FAIL : MAT_PARTICLE_STAGE_SKIPPED;
	}

	if (stage->tcgen != MAT_TCGEN_BASE)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "tcGen must be base", reason_size);
		return strict ? MAT_PARTICLE_STAGE_HARD_FAIL : MAT_PARTICLE_STAGE_SKIPPED;
	}

	if (stage->tcmod_count > countof (stage->tcmods))
	{
		if (reason && reason_size)
			q_strlcpy (reason, "tcMod count overflow", reason_size);
		return MAT_PARTICLE_STAGE_HARD_FAIL;
	}
	if (stage->tcmod_overflow)
	{
		if (reason && reason_size)
			q_strlcpy (reason, "tcMod limit exceeded", reason_size);
		return MAT_PARTICLE_STAGE_HARD_FAIL;
	}

	for (i = 0; i < stage->tcmod_count; ++i)
	{
		mat_tcmod_type_t type = stage->tcmods[i].type;
		if (type != MAT_TCMOD_SCROLL && type != MAT_TCMOD_SCALE && type != MAT_TCMOD_ROTATE &&
			type != MAT_TCMOD_TURB && type != MAT_TCMOD_STRETCH)
		{
			if (reason && reason_size)
				q_strlcpy (reason, "unsupported tcMod type", reason_size);
			return strict ? MAT_PARTICLE_STAGE_HARD_FAIL : MAT_PARTICLE_STAGE_SKIPPED;
		}
	}

	if (stage->blend_mode == MAT_BLEND_CUSTOM)
	{
		if (stage->blend_src < 0 || stage->blend_dst < 0)
		{
			if (reason && reason_size)
				q_strlcpy (reason, "invalid custom blend factors", reason_size);
			return MAT_PARTICLE_STAGE_HARD_FAIL;
		}
	}

	return MAT_PARTICLE_STAGE_SUPPORTED;
}

qboolean Material_StageSupportsParticleMVP (const material_stage_t *stage, char *reason, size_t reason_size)
{
	mat_particle_policy_t policy = r_particles_material_strict.value > 0.f
		? MAT_PARTICLE_POLICY_STRICT
		: MAT_PARTICLE_POLICY_TOLERANT;

	return Material_ClassifyParticleStage (stage, policy, reason, reason_size) == MAT_PARTICLE_STAGE_SUPPORTED;
}

void Material_Init (void)
{
	Cvar_RegisterVariable (&r_materials);
	Cvar_RegisterVariable (&r_material_debug);
	Cvar_RegisterVariable (&r_tcgen_debug);
	Cvar_RegisterVariable (&r_sun_visibility);
	Cvar_RegisterVariable (&r_material_debug_parse);
	Cvar_RegisterVariable (&r_particles_material_strict);
	Cvar_RegisterVariable (&r_reloadmaterials);
	Cvar_RegisterVariable (&r_material_fuzz);
	Cvar_RegisterVariable (&r_material_report);
	Cvar_SetCallback (&r_reloadmaterials, Material_Reload_f);
	Cvar_SetCallback (&r_material_fuzz, Material_Fuzz_f);

	Cmd_AddCommand ("materiallist", Material_List_f);
	Cmd_AddCommand ("materialprint", Material_Print_f);
	Cmd_AddCommand ("materialfuzz", Material_FuzzCommand_f);
}

void Material_Shutdown (void)
{
	Material_Reset ();
	mat_material_loaded = false;
}

void Material_Reload (void)
{
	Material_LoadAll ();
}

size_t Material_Count (void)
{
	Material_LoadAll ();
	return VEC_SIZE (mat_material_list);
}

const material_t *Material_GetByIndex (size_t index)
{
	Material_LoadAll ();
	if (index >= VEC_SIZE (mat_material_list))
		return NULL;
	return mat_material_list[index];
}

void Material_Canonicalize (const char *name, char *out, size_t out_size)
{
	size_t i;

	if (!out || out_size == 0)
		return;

	q_strlcpy (out, name ? name : "", out_size);
	for (i = 0; out[i]; ++i)
	{
		if (out[i] == '\\')
			out[i] = '/';
	}
	COM_StripExtension (out, out, out_size);
	q_strlwr (out);
}

const material_t *Material_Find (const char *name)
{
	char canonical[MAX_QPATH];

	if (!name || !name[0])
		return NULL;

	Material_LoadAll ();
	Material_Canonicalize (name, canonical, sizeof (canonical));
	return Material_FindInternal (canonical);
}

const material_t *Material_FindForTextureName (const char *texname, const char *mapname)
{
	char candidate[MAX_QPATH];
	const material_t *material;

	if (!texname || !texname[0])
		return NULL;

	if (mapname && mapname[0])
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s/%s", mapname, texname);
		material = Material_Find (candidate);
		if (material)
			return material;
	}

	q_snprintf (candidate, sizeof (candidate), "textures/%s", texname);
	material = Material_Find (candidate);
	if (material)
		return material;

	return Material_Find (texname);
}

const char *Material_GetStage0Map (const material_t *material, const char *texname)
{
	if (!material || (material->stage0.map_type != MAT_MAP_MAP && material->stage0.map_type != MAT_MAP_CLAMPMAP))
		return NULL;
	if (!material->stage0.map_path || !material->stage0.map_path[0])
		return NULL;

	if (texname && texname[0])
	{
		char canonical_tex[MAX_QPATH];
		Material_Canonicalize (texname, canonical_tex, sizeof (canonical_tex));
		if (q_strcasecmp (material->name, canonical_tex))
			return NULL;
	}

	return material->stage0.map_path;
}

unsigned int Material_GetTextureFlags (const material_t *material)
{
	unsigned int flags = 0u;

	if (!material)
		return flags;

	if (material->render_flags & MAT_RENDER_NODRAW)
		flags |= MATERIAL_FLAG_NODRAW;
	if (material->render_flags & MAT_RENDER_SKY)
		flags |= MATERIAL_FLAG_SKY;
	if (material->render_flags & MAT_RENDER_TRANS)
		flags |= MATERIAL_FLAG_TRANS;
	if (material->render_flags & MAT_RENDER_ALPHAOCCLUDE)
		flags |= MATERIAL_FLAG_ALPHAOCCLUDE;
	if (material->render_flags & MAT_RENDER_FOG)
		flags |= MATERIAL_FLAG_FOG;
	if (material->surfaceparms & MAT_SURFPARM_SOLID)
		flags |= MATERIAL_FLAG_SOLID;
	if (material->surfaceparms & MAT_SURFPARM_NONSOLID)
		flags |= MATERIAL_FLAG_NONSOLID;
	if (material->surfaceparms & MAT_SURFPARM_PLAYERCLIP)
		flags |= MATERIAL_FLAG_PLAYERCLIP;
	if (material->surfaceparms & MAT_SURFPARM_MONSTERCLIP)
		flags |= MATERIAL_FLAG_MONSTERCLIP;
	if (material->surfaceparms & MAT_SURFPARM_STONE)
		flags |= MATERIAL_FLAG_STONE;
	if (material->emissive_enable)
		flags |= MATERIAL_FLAG_EMISSIVE;
	if (material->bloom_enable)
		flags |= MATERIAL_FLAG_BLOOM;

	return flags;
}

void Material_ApplyToTexture (texture_t *tex, const char *mapname)
{
	unsigned int flags;
	const material_t *material = NULL;
	char candidate[MAX_QPATH];

	if (!tex)
		return;
	if (r_materials.value <= 0.f)
		return;

	if (mapname && mapname[0])
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s/%s", mapname, tex->name);
		material = Material_Find (candidate);
		if (material)
			tex->material_map = Material_GetStage0Map (material, candidate);
	}

	if (!material)
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s", tex->name);
		material = Material_Find (candidate);
		if (material)
			tex->material_map = Material_GetStage0Map (material, candidate);
	}

	if (!material)
	{
		material = Material_Find (tex->name);
		if (material)
			tex->material_map = Material_GetStage0Map (material, tex->name);
	}

	if (!material)
		return;

	flags = Material_GetTextureFlags (material);

	tex->material = material;
	tex->material_flags = flags;

	if ((material->render_flags & MAT_RENDER_SKY) && tex->type != TEXTYPE_SKY)
		tex->type = TEXTYPE_SKY;
	if ((material->render_flags & MAT_RENDER_TRANS) && r_material_debug.value >= 1.f)
		Con_DPrintf ("Material: surfaceparm trans on %s (TODO: blend path)\n", tex->name);

	if (r_material_debug.value >= 1.f)
	{
		if (tex->material_map)
			Con_Printf ("Material: %s overrides %s\n", tex->name, tex->material_map);
	}
}

void Material_Print (const material_t *material)
{
	static const char *const cull_names[] = { "back", "front", "none" };
	static const char *const blend_names[] = { "replace", "alpha", "add", "mult", "premult", "custom" };
	static const char *const depth_names[] = { "lequal", "less", "equal", "greater", "gequal", "always", "never" };
	static const char *const map_names[] = { "map", "clampmap", "lightmap", "white", "black" };

	if (!material)
		return;

	Con_Printf ("Shader: %s\n", material->name);
	if (material->source_file && material->source_file[0])
		Con_Printf ("  source: %s\n", material->source_file);
	if (material->editor_image)
		Con_Printf ("  editor image: %s\n", material->editor_image);
	Con_Printf ("  surfaceparms: 0x%08x\n", material->surfaceparms);
	Con_Printf ("  render flags: 0x%08x\n", material->render_flags);
	Con_Printf ("  content flags: 0x%08x\n", material->content_flags);
	Con_Printf ("  cull: %s\n", cull_names[q_min ((int)material->cull_mode, (int)countof (cull_names) - 1)]);
	Con_Printf ("  sort: %s\n", Material_SortName (material->sort_key));
	if (material->polygon_offset)
		Con_Printf ("  polygon offset: on (factor %.2f units %.2f)\n",
			material->polygon_offset_factor, material->polygon_offset_units);
	else
		Con_Printf ("  polygon offset: off\n");
	Con_Printf ("  emissive: %s (scale %.2f)\n", material->emissive_enable ? "on" : "off", material->emissive_scale);
	Con_Printf ("  bloom: %s (scale %.2f)\n", material->bloom_enable ? "on" : "off", material->bloom_scale);
	if (material->stage0.map_path
		|| material->stage0.map_type != MAT_MAP_MAP
		|| material->stage0.normal_map_path
		|| material->stage0.specular_map_path
		|| material->stage0.orm_map_path
		|| material->stage0.normal_scale != 1.f
		|| material->stage0.spec_power != 1.f
		|| material->stage0.spec_intensity != 1.f)
	{
		const char *map_name = map_names[q_min ((int)material->stage0.map_type, (int)countof (map_names) - 1)];
		Con_Printf ("  stage0 map: %s (%s)\n", material->stage0.map_path ? material->stage0.map_path : "<builtin>", map_name);
		Con_Printf ("  stage0 normal map: %s\n", material->stage0.normal_map_path ? material->stage0.normal_map_path : "<none>");
		Con_Printf ("  stage0 specular map: %s\n", material->stage0.specular_map_path ? material->stage0.specular_map_path : "<none>");
		Con_Printf ("  stage0 orm map: %s\n", material->stage0.orm_map_path ? material->stage0.orm_map_path : "<none>");
		Con_Printf ("  stage0 normal/spec: normalScale %.2f specPower %.2f specIntensity %.2f\n",
			material->stage0.normal_scale, material->stage0.spec_power, material->stage0.spec_intensity);
		Con_Printf ("  stage0 blend: %s\n", blend_names[q_min ((int)material->stage0.blend_mode, (int)countof (blend_names) - 1)]);
		Con_Printf ("  stage0 depth: %s (write %s)\n",
			depth_names[q_min ((int)material->stage0.depth_func, (int)countof (depth_names) - 1)],
			material->stage0.depth_write ? "on" : "off");
	}
}

void Material_Insert (material_t *material)
{
	material_t *owned;
	const material_t *existing;

	if (!material || !material->name || !material->name[0])
		return;

	existing = Material_FindInternal (material->name);
	if (existing)
	{
		Material_Unregister (existing);
		Material_FreeMaterial ((material_t *) existing);
	}

	owned = (material_t *) Z_Malloc (sizeof (*owned));
	memcpy (owned, material, sizeof (*owned));
	Material_Register (owned);
}

void Material_Remove (const material_t *material)
{
	Material_Unregister (material);
	Material_FreeMaterial ((material_t *) material);
}

void Material_ReportUnknownToken (const char *token, material_keyword_scope_t scope, const char *context, const char *source_file, unsigned int line)
{
	char warn_key[MAX_QPATH * 3];
	char warn_context[MAX_QPATH * 2];
	char warn_location[MAX_QPATH * 2];
	const char *scope_name = Material_ScopeName (scope);

	Material_FormatSourceLine (warn_location, sizeof (warn_location), source_file, line);

	q_snprintf (warn_key, sizeof (warn_key), "%s::%s", scope_name, token);

	if (context && context[0])
		q_snprintf (warn_context, sizeof (warn_context), "%s (%s at %s)", context, scope_name, warn_location);
	else
		q_snprintf (warn_context, sizeof (warn_context), "material (%s at %s)", scope_name, warn_location);

	Material_WarnOnce (warn_key, token, warn_context);
	Material_RecordUnknownToken (token, scope, context, source_file, line);
}

static int Material_TimeBucket (float time, float fps_hint)
{
	float fps = fps_hint > 0.f ? fps_hint : 60.f;
	if (fps < 1.f)
		fps = 1.f;
	return (int)floorf (time * fps);
}

float Material_EvalWaveValue (const mat_wave_t *wave, float time)
{
	float t;
	float phase;
	float value;

	if (!wave)
		return 0.f;

	t = time * wave->freq + wave->phase;

	switch (wave->type)
	{
	case MAT_WAVE_TRIANGLE:
		phase = t - floorf (t + 0.5f);
		value = 1.f - 4.f * fabsf (phase);
		break;
	case MAT_WAVE_SAW:
		phase = t - floorf (t);
		value = phase * 2.f - 1.f;
		break;
	case MAT_WAVE_INVERSESAW:
		phase = t - floorf (t);
		value = 1.f - phase * 2.f;
		break;
	case MAT_WAVE_SIN:
	default:
		value = sinf (t * 2.f * MAT_TEXMOD_PI);
		break;
	}

	return wave->base + value * wave->amp;
}

const mat_texmatrix_t *MaterialStage_EvalTexMatrix (material_stage_t *stage, float time)
{
	mat_texmatrix_t matrix;
	mat_texmatrix_t tmp;
	int bucket;
	int i;

	if (!stage)
		return NULL;

	bucket = Material_TimeBucket (time, 60.f);
	if (stage->texmatrix_time_bucket == bucket)
		return &stage->texmatrix_cache;

	Mat_MatrixIdentity (&matrix);

	for (i = 0; i < stage->tcmod_count; ++i)
	{
		const mat_tcmod_t *mod = &stage->tcmods[i];
		switch (mod->type)
		{
		case MAT_TCMOD_SCROLL:
			Mat_MatrixTranslate (&tmp, mod->args[0] * time, mod->args[1] * time);
			Mat_MatrixMultiply (&matrix, &tmp, &matrix);
			break;
		case MAT_TCMOD_SCALE:
			Mat_MatrixScale (&tmp, mod->args[0], mod->args[1]);
			Mat_MatrixMultiply (&matrix, &tmp, &matrix);
			break;
		case MAT_TCMOD_ROTATE:
		{
			float degrees = mod->args[0] * time;
			mat_texmatrix_t rot;
			Mat_MatrixRotate (&rot, degrees);
			Mat_MatrixAroundCenter (&tmp, &rot);
			Mat_MatrixMultiply (&matrix, &tmp, &matrix);
			break;
		}
		case MAT_TCMOD_TURB:
		{
			float phase = mod->args[2];
			float freq = mod->args[3];
			float value = mod->args[0] + sinf ((time * freq + phase) * 2.f * MAT_TEXMOD_PI) * mod->args[1];
			Mat_MatrixTranslate (&tmp, value, value);
			Mat_MatrixMultiply (&matrix, &tmp, &matrix);
			break;
		}
		case MAT_TCMOD_STRETCH:
		{
			float phase = mod->args[2];
			float freq = mod->args[3];
			float value = mod->args[0] + sinf ((time * freq + phase) * 2.f * MAT_TEXMOD_PI) * mod->args[1];
			mat_texmatrix_t scale;
			Mat_MatrixScale (&scale, value, value);
			Mat_MatrixAroundCenter (&tmp, &scale);
			Mat_MatrixMultiply (&matrix, &tmp, &matrix);
			break;
		}
		default:
			break;
		}
	}

	stage->texmatrix_cache = matrix;
	stage->texmatrix_time_bucket = bucket;
	return &stage->texmatrix_cache;
}

int MaterialStage_EvalAnimMapFrame (material_stage_t *stage, float time)
{
	int bucket;
	int frame_count;
	int frame;

	if (!stage || stage->anim_map_fps <= 0.f)
		return 0;

	frame_count = (int)VEC_SIZE (stage->anim_map_frames);
	if (frame_count <= 0)
		return 0;

	bucket = Material_TimeBucket (time, stage->anim_map_fps);
	if (stage->anim_map_time_bucket == bucket)
		return stage->anim_map_frame;

	frame = bucket % frame_count;
	if (frame < 0)
		frame += frame_count;

	stage->anim_map_time_bucket = bucket;
	stage->anim_map_frame = frame;
	return frame;
}

const char *MaterialStage_GetAnimMapPath (material_stage_t *stage, float time)
{
	int frame;
	int frame_count;

	if (!stage)
		return NULL;

	frame_count = (int)VEC_SIZE (stage->anim_map_frames);
	if (frame_count <= 0)
		return NULL;

	frame = MaterialStage_EvalAnimMapFrame (stage, time);
	if (frame < 0 || frame >= frame_count)
		return NULL;

	return stage->anim_map_frames[frame];
}
