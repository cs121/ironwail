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
#include "mat_shader.h"
#include "mat_shader_parse.h"
#include "miniz.h"
#include <math.h>
#include <stdarg.h>

#define MAT_SHADER_HASH_SIZE 256
#define MAT_SHADER_LIST_LIMIT 64
#define MAT_TEXMOD_PI 3.14159265358979323846f
#define MAT_SHADER_UNKNOWN_LIMIT 256

typedef struct mat_shader_entry_s
{
	shader_material_t	*material;
	unsigned int		hash;
	struct mat_shader_entry_s *next;
} mat_shader_entry_t;

typedef struct mat_shader_warn_s
{
	char *token;
} mat_shader_warn_t;

typedef struct mat_shader_keyword_def_s
{
	const char *keyword;
	mat_shader_keyword_scope_t scope;
	mat_shader_keyword_status_t status;
	const char *notes;
} mat_shader_keyword_def_t;

typedef struct mat_shader_unknown_s
{
	char *token;
	mat_shader_keyword_scope_t scope;
	unsigned int count;
	char *first_context;
	char *first_source;
	unsigned int first_line;
} mat_shader_unknown_t;

static mat_shader_entry_t *mat_shader_hash[MAT_SHADER_HASH_SIZE];
static shader_material_t **mat_shader_list;
static mat_shader_warn_t *mat_shader_warned;
static mat_shader_unknown_t *mat_shader_unknowns;
static qboolean mat_shader_loaded;
static qboolean mat_shader_unknown_overflow;

static const mat_shader_keyword_def_t mat_shader_keyword_table[] =
{
	{ "qer_editorimage", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Editor preview texture." },
	{ "surfaceparm", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Surface flags; see surfaceparm scope." },
	{ "cull", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Modes: back/front/none." },
	{ "sort", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Keys: opaque/decal/additive/nearest." },
	{ "polygonOffset", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Optional boolean." },
	{ "emissive", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "bloom", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "godray", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "emissive_scale", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "bloom_scale", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "godray_scale", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Non-Q3 extension." },
	{ "skyParms", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 sky parameters." },
	{ "fogParms", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 fog parameters." },
	{ "deformVertexes", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 vertex deformation." },
	{ "q3map_*", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3Map compile-time directives." },

	{ "map", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Supports $lightmap/$white/$black and textures." },
	{ "clampmap", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Clamp-wrapped texture." },
	{ "animMap", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "FPS + frame list only." },
	{ "rgbGen", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Supports identity only." },
	{ "alphaGen", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 alpha generator." },
	{ "blendFunc", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Supports add/filter/blend/premult or explicit factors." },
	{ "depthWrite", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Optional boolean." },
	{ "depthFunc", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Modes: lequal/equal/always." },
	{ "alphaFunc", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED, "Q3 alpha test." },
	{ "tcGen", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Modes: base/environment/lightmap." },
	{ "tcMod", MAT_SHADER_KEYWORD_SCOPE_STAGE, MAT_SHADER_KEYWORD_STATUS_PARTIAL, "Types: scroll/scale/rotate/turb/stretch." },

	{ "solid", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Surface solid." },
	{ "nonsolid", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Surface non-solid." },
	{ "playerclip", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Blocks players." },
	{ "monsterclip", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Blocks monsters." },
	{ "trans", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Marks transparent." },
	{ "alphashadow", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Alpha shadow hint." },
	{ "sky", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Sky surface." },
	{ "fog", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Fog surface." },
	{ "nodraw", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "No draw surface." },
	{ "stone", MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED, "Footstep hint." }
};

static qboolean mat_shader_keyword_seen[countof (mat_shader_keyword_table)];

cvar_t r_shaders = { "r_shaders", "1", CVAR_ARCHIVE };
cvar_t r_shader_debug = { "r_shader_debug", "0", CVAR_ARCHIVE };
cvar_t r_matshader_debug_parse = { "r_matshader_debug_parse", "0", CVAR_ARCHIVE };
static cvar_t r_reloadshaders = { "r_reloadshaders", "0", CVAR_NONE };
static cvar_t r_matshader_fuzz = { "r_matshader_fuzz", "0", CVAR_NONE };
static cvar_t r_matshader_report = { "r_matshader_report", "0", CVAR_NONE };

char *Mat_Shader_DupString (const char *value)
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

static void Mat_Shader_FreeMaterial (shader_material_t *material)
{
	size_t i;

	if (!material)
		return;

	for (i = 0; i < VEC_SIZE (material->stages); ++i)
	{
		mat_shader_stage_t *stage = &material->stages[i];
		if (stage->map_path)
			Z_Free (stage->map_path);
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
	VEC_FREE (material->stages);

	if (material->editor_image)
		Z_Free (material->editor_image);
	if (material->name)
		Z_Free (material->name);
	if (material->source_file)
		Z_Free (material->source_file);

	Z_Free (material);
}

static void Mat_Shader_Reset (void)
{
	size_t i;

	for (i = 0; i < VEC_SIZE (mat_shader_list); ++i)
		Mat_Shader_FreeMaterial (mat_shader_list[i]);

	VEC_FREE (mat_shader_list);

	for (i = 0; i < countof (mat_shader_hash); ++i)
	{
		mat_shader_entry_t *entry = mat_shader_hash[i];
		while (entry)
		{
			mat_shader_entry_t *next = entry->next;
			Z_Free (entry);
			entry = next;
		}
		mat_shader_hash[i] = NULL;
	}

	for (i = 0; i < VEC_SIZE (mat_shader_warned); ++i)
	{
		if (mat_shader_warned[i].token)
			Z_Free (mat_shader_warned[i].token);
	}
	VEC_FREE (mat_shader_warned);

	for (i = 0; i < VEC_SIZE (mat_shader_unknowns); ++i)
	{
		if (mat_shader_unknowns[i].token)
			Z_Free (mat_shader_unknowns[i].token);
		if (mat_shader_unknowns[i].first_context)
			Z_Free (mat_shader_unknowns[i].first_context);
		if (mat_shader_unknowns[i].first_source)
			Z_Free (mat_shader_unknowns[i].first_source);
	}
	VEC_FREE (mat_shader_unknowns);
	mat_shader_unknown_overflow = false;

	memset (mat_shader_keyword_seen, 0, sizeof (mat_shader_keyword_seen));
}

static unsigned int Mat_Shader_Hash (const char *name)
{
	return COM_HashString (name) % MAT_SHADER_HASH_SIZE;
}

static qboolean Mat_Shader_TokenWarned (const char *token)
{
	size_t i;

	for (i = 0; i < VEC_SIZE (mat_shader_warned); ++i)
	{
		if (!q_strcasecmp (mat_shader_warned[i].token, token))
			return true;
	}

	return false;
}

static void Mat_Shader_AddWarned (const char *token)
{
	mat_shader_warn_t warn;

	memset (&warn, 0, sizeof (warn));
	warn.token = Mat_Shader_DupString (token);
	VEC_PUSH (mat_shader_warned, warn);
}

static void Mat_Shader_WarnOnce (const char *warn_key, const char *token, const char *context)
{
	if (Mat_Shader_TokenWarned (warn_key))
		return;

	Mat_Shader_AddWarned (warn_key);
	Mat_Shader_ParseAddWarning ();
	Con_Warning ("MatShader: unknown token '%s' in %s\n", token, context ? context : "shader");
}

static void Mat_Shader_FormatSourceLine (char *buffer, size_t buffer_size, const char *source_file, unsigned int line)
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

static const char *Mat_Shader_ScopeName (mat_shader_keyword_scope_t scope)
{
	switch (scope)
	{
	case MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL:
		return "top-level";
	case MAT_SHADER_KEYWORD_SCOPE_STAGE:
		return "stage";
	case MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM:
		return "surfaceparm";
	default:
		return "unknown";
	}
}

static const char *Mat_Shader_StatusName (mat_shader_keyword_status_t status)
{
	switch (status)
	{
	case MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED:
		return "Implemented";
	case MAT_SHADER_KEYWORD_STATUS_PARTIAL:
		return "Partial";
	case MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED:
		return "Known-but-Unimplemented";
	default:
		return "Unknown";
	}
}

void Mat_Shader_MarkKeywordSeen (const char *keyword, mat_shader_keyword_scope_t scope)
{
	size_t i;

	if (!keyword || !keyword[0])
		return;

	for (i = 0; i < countof (mat_shader_keyword_table); ++i)
	{
		if (mat_shader_keyword_table[i].scope != scope)
			continue;
		if (!q_strcasecmp (mat_shader_keyword_table[i].keyword, keyword))
		{
			mat_shader_keyword_seen[i] = true;
			return;
		}
	}
}

static void Mat_Shader_RecordUnknownToken (const char *token, mat_shader_keyword_scope_t scope, const char *context, const char *source_file, unsigned int line)
{
	size_t i;

	if (!token || !token[0])
		return;

	for (i = 0; i < VEC_SIZE (mat_shader_unknowns); ++i)
	{
		if (mat_shader_unknowns[i].scope == scope && !q_strcasecmp (mat_shader_unknowns[i].token, token))
		{
			mat_shader_unknowns[i].count++;
			return;
		}
	}

	if (VEC_SIZE (mat_shader_unknowns) >= MAT_SHADER_UNKNOWN_LIMIT)
	{
		mat_shader_unknown_overflow = true;
		return;
	}

	{
		mat_shader_unknown_t entry;

		memset (&entry, 0, sizeof (entry));
		entry.token = Mat_Shader_DupString (token);
		entry.scope = scope;
		entry.count = 1;
		entry.first_context = Mat_Shader_DupString (context ? context : "");
		entry.first_source = Mat_Shader_DupString (source_file ? source_file : "");
		entry.first_line = line;
		VEC_PUSH (mat_shader_unknowns, entry);
	}
}

static void Mat_Shader_ReportAppend (char **buffer, size_t *len, size_t *cap, const char *fmt, ...)
{
	char temp[1024];
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
		while (newcap < *len + add + 1)
			newcap *= 2;
		{
			char *newbuf = (char *) realloc (*buffer, newcap);
			if (!newbuf)
				return;
			*buffer = newbuf;
			*cap = newcap;
		}
	}

	memcpy (*buffer + *len, temp, add);
	*len += add;
	(*buffer)[*len] = '\0';
}

typedef struct mat_shader_keyword_row_s
{
	const mat_shader_keyword_def_t *def;
	qboolean seen;
} mat_shader_keyword_row_t;

static int Mat_Shader_CompareKeywordRows (const void *a, const void *b)
{
	const mat_shader_keyword_row_t *left = (const mat_shader_keyword_row_t *) a;
	const mat_shader_keyword_row_t *right = (const mat_shader_keyword_row_t *) b;
	int scope_cmp = (int)left->def->scope - (int)right->def->scope;

	if (scope_cmp != 0)
		return scope_cmp;
	return q_strcasecmp (left->def->keyword, right->def->keyword);
}

static int Mat_Shader_CompareUnknowns (const void *a, const void *b)
{
	const mat_shader_unknown_t *const *left = (const mat_shader_unknown_t *const *) a;
	const mat_shader_unknown_t *const *right = (const mat_shader_unknown_t *const *) b;
	int scope_cmp = (int)(*left)->scope - (int)(*right)->scope;

	if (scope_cmp != 0)
		return scope_cmp;
	return q_strcasecmp ((*left)->token, (*right)->token);
}

static int Mat_Shader_CompareUnknownCountDesc (const void *a, const void *b)
{
	const mat_shader_unknown_t *const *left = (const mat_shader_unknown_t *const *) a;
	const mat_shader_unknown_t *const *right = (const mat_shader_unknown_t *const *) b;

	if ((*left)->count != (*right)->count)
		return (*left)->count > (*right)->count ? -1 : 1;
	return q_strcasecmp ((*left)->token, (*right)->token);
}

static void Mat_Shader_ReportKeywords (char **buffer, size_t *len, size_t *cap, const mat_shader_keyword_row_t *rows, size_t row_count, mat_shader_keyword_status_t status)
{
	size_t i;
	int printed = 0;

	Mat_Shader_ReportAppend (buffer, len, cap, "## %s\n", Mat_Shader_StatusName (status));

	for (i = 0; i < row_count; ++i)
	{
		const mat_shader_keyword_row_t *row = &rows[i];
		if (row->def->status != status)
			continue;
		printed++;
		Mat_Shader_ReportAppend (buffer, len, cap, "- `%s` (%s) — Seen: %s",
			row->def->keyword,
			Mat_Shader_ScopeName (row->def->scope),
			row->seen ? "yes" : "no");
		if (row->def->notes && row->def->notes[0])
			Mat_Shader_ReportAppend (buffer, len, cap, ". Notes: %s", row->def->notes);
		Mat_Shader_ReportAppend (buffer, len, cap, "\n");
	}

	if (!printed)
		Mat_Shader_ReportAppend (buffer, len, cap, "_None._\n");

	Mat_Shader_ReportAppend (buffer, len, cap, "\n");
}

static void Mat_Shader_WriteReport (void)
{
	const char *path = "docs/mat_shader_report.md";
	const size_t keyword_count = countof (mat_shader_keyword_table);
	mat_shader_keyword_row_t *rows = NULL;
	mat_shader_unknown_t **unknown_rows = NULL;
	size_t unknown_count = VEC_SIZE (mat_shader_unknowns);
	char *report = NULL;
	size_t len = 0;
	size_t cap = 0;
	size_t i;

	rows = (mat_shader_keyword_row_t *) malloc (keyword_count * sizeof (*rows));
	if (!rows)
		return;

	for (i = 0; i < keyword_count; ++i)
	{
		rows[i].def = &mat_shader_keyword_table[i];
		rows[i].seen = mat_shader_keyword_seen[i];
	}
	qsort (rows, keyword_count, sizeof (*rows), Mat_Shader_CompareKeywordRows);

	Mat_Shader_ReportAppend (&report, &len, &cap, "# Material Shader Keyword Report\n\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "Tracked keywords are grouped by implementation status and scope.\n\n");

	Mat_Shader_ReportKeywords (&report, &len, &cap, rows, keyword_count, MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED);
	Mat_Shader_ReportKeywords (&report, &len, &cap, rows, keyword_count, MAT_SHADER_KEYWORD_STATUS_PARTIAL);
	Mat_Shader_ReportKeywords (&report, &len, &cap, rows, keyword_count, MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED);

	Mat_Shader_ReportAppend (&report, &len, &cap, "## Unknown-Seen\n");
	if (unknown_count > 0)
	{
		unknown_rows = (mat_shader_unknown_t **) malloc (unknown_count * sizeof (*unknown_rows));
		if (unknown_rows)
		{
			for (i = 0; i < unknown_count; ++i)
				unknown_rows[i] = &mat_shader_unknowns[i];
			qsort (unknown_rows, unknown_count, sizeof (*unknown_rows), Mat_Shader_CompareUnknowns);

			for (i = 0; i < unknown_count; ++i)
			{
				const mat_shader_unknown_t *entry = unknown_rows[i];
				char location[MAX_QPATH * 2];
				Mat_Shader_FormatSourceLine (location, sizeof (location), entry->first_source, entry->first_line);
				Mat_Shader_ReportAppend (&report, &len, &cap,
					"- `%s` (%s) — Count: %u. First seen in %s (material: %s)\n",
					entry->token,
					Mat_Shader_ScopeName (entry->scope),
					entry->count,
					location,
					entry->first_context && entry->first_context[0] ? entry->first_context : "<unknown>");
			}
		}
		else
		{
			for (i = 0; i < unknown_count; ++i)
			{
				const mat_shader_unknown_t *entry = &mat_shader_unknowns[i];
				char location[MAX_QPATH * 2];
				Mat_Shader_FormatSourceLine (location, sizeof (location), entry->first_source, entry->first_line);
				Mat_Shader_ReportAppend (&report, &len, &cap,
					"- `%s` (%s) — Count: %u. First seen in %s (material: %s)\n",
					entry->token,
					Mat_Shader_ScopeName (entry->scope),
					entry->count,
					location,
					entry->first_context && entry->first_context[0] ? entry->first_context : "<unknown>");
			}
		}
		if (mat_shader_unknown_overflow)
			Mat_Shader_ReportAppend (&report, &len, &cap, "- _Unknown token limit reached; additional tokens omitted._\n");
	}
	else
	{
		Mat_Shader_ReportAppend (&report, &len, &cap, "_None._\n");
	}
	Mat_Shader_ReportAppend (&report, &len, &cap, "\n");

	Mat_Shader_ReportAppend (&report, &len, &cap, "## Wishlist / reference (Q3 keywords)\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- blendFunc (add, filter, blend, custom factors)\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- rgbGen modes (identity, identityLighting, entity, oneMinusEntity, vertex, exactVertex, lightingDiffuse)\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- alphaGen modes (identity, entity, oneMinusEntity, vertex, lightingSpecular, portal, wave)\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- tcGen / tcMod (base, lightmap, environment, vector; scroll/scale/rotate/stretch/transform/turb)\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- animMap / clampmap\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- depthFunc / depthWrite / alphaFunc\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- cull / sort / polygonOffset\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- deformVertexes\n");
	Mat_Shader_ReportAppend (&report, &len, &cap, "- skyParms / fogParms / q3map_*\n");

	if (report && len > 0)
		COM_WriteFile (path, report, (int)len);

	free (unknown_rows);
	free (rows);
	free (report);
}

static qboolean Mat_Shader_ShouldWriteReport (void)
{
#if defined(_DEBUG) || defined(DEBUG)
	return true;
#else
	return r_matshader_report.value > 0.f;
#endif
}

static const char *Mat_Shader_MapTypeName (mat_map_type_t map_type)
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

static void Mat_Shader_LogUnknownSummary (void)
{
	mat_shader_unknown_t **unknown_rows = NULL;
	size_t unknown_count = VEC_SIZE (mat_shader_unknowns);
	size_t i;
	size_t limit = 10;

	if (!unknown_count)
		return;

	unknown_rows = (mat_shader_unknown_t **) malloc (unknown_count * sizeof (*unknown_rows));
	if (!unknown_rows)
		return;

	for (i = 0; i < unknown_count; ++i)
		unknown_rows[i] = &mat_shader_unknowns[i];

	qsort (unknown_rows, unknown_count, sizeof (*unknown_rows), Mat_Shader_CompareUnknownCountDesc);

	Con_Printf ("MatShader: Top %zu unknown keywords\n", q_min (limit, unknown_count));
	for (i = 0; i < unknown_count && i < limit; ++i)
	{
		const mat_shader_unknown_t *entry = unknown_rows[i];
		Con_Printf ("  %s (%s) x%u\n", entry->token, Mat_Shader_ScopeName (entry->scope), entry->count);
	}

	free (unknown_rows);
}

static void Mat_Shader_LogSummary (size_t parsed_total)
{
	size_t warning_count = Mat_Shader_ParseGetWarnings ();
	size_t error_count = Mat_Shader_ParseGetErrors ();
	size_t unknown_count = VEC_SIZE (mat_shader_unknowns);

	Con_Printf ("MatShader: parsed %zu shaders (%zu errors, %zu warnings, %zu unknown unique)\n",
		parsed_total, error_count, warning_count, unknown_count);
	if (mat_shader_unknown_overflow)
		Con_Printf ("MatShader: unknown token limit reached; additional tokens omitted\n");
	Mat_Shader_LogUnknownSummary ();
}

static void Mat_Shader_DebugDumpShaders (void)
{
	size_t count = VEC_SIZE (mat_shader_list);
	size_t i;

	Con_Printf ("MatShader: Debug dump (%zu shaders)\n", count);
	for (i = 0; i < count; ++i)
	{
		const shader_material_t *material = mat_shader_list[i];
		size_t stage_count = VEC_SIZE (material->stages);

		Con_Printf ("MatShader: %s\n", material->name);
		Con_Printf ("  flags: surface=0x%08x render=0x%08x content=0x%08x\n",
			material->surfaceparms, material->render_flags, material->content_flags);

		if (stage_count == 0 && (material->stage0.map_path || material->stage0.map_type != MAT_MAP_MAP))
			stage_count = 1;
		Con_Printf ("  stages: %zu\n", stage_count);

		for (size_t stage_index = 0; stage_index < stage_count; ++stage_index)
		{
			const mat_shader_stage_t *stage = material->stages ? &material->stages[stage_index] : &material->stage0;
			const char *map_name = stage->map_path && stage->map_path[0] ? stage->map_path : Mat_Shader_MapTypeName (stage->map_type);

			Con_Printf ("    stage %zu: %s\n", stage_index, map_name);
		}
	}
}

static void Mat_Shader_DebugDumpUnknowns (void)
{
	size_t unknown_count = VEC_SIZE (mat_shader_unknowns);
	size_t i;

	Con_Printf ("MatShader: Unknown keywords (%zu)\n", unknown_count);
	for (i = 0; i < unknown_count; ++i)
	{
		const mat_shader_unknown_t *entry = &mat_shader_unknowns[i];
		char location[MAX_QPATH * 2];

		Mat_Shader_FormatSourceLine (location, sizeof (location), entry->first_source, entry->first_line);
		Con_Printf ("  %s (%s) at %s\n", entry->token, Mat_Shader_ScopeName (entry->scope), location);
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

static void Mat_Shader_Register (shader_material_t *material)
{
	mat_shader_entry_t *entry;
	unsigned int bucket;

	entry = (mat_shader_entry_t *) Z_Malloc (sizeof (*entry));
	entry->material = material;
	entry->hash = Mat_Shader_Hash (material->name);
	bucket = entry->hash;
	entry->next = mat_shader_hash[bucket];
	mat_shader_hash[bucket] = entry;

	VEC_PUSH (mat_shader_list, material);
}

static void Mat_Shader_Unregister (const shader_material_t *material)
{
	unsigned int bucket;
	mat_shader_entry_t **entry;
	size_t i;

	if (!material)
		return;

	bucket = Mat_Shader_Hash (material->name);
	entry = &mat_shader_hash[bucket];
	while (*entry)
	{
		if ((*entry)->material == material)
		{
			mat_shader_entry_t *next = (*entry)->next;
			Z_Free (*entry);
			*entry = next;
			break;
		}
		entry = &(*entry)->next;
	}

	for (i = 0; i < VEC_SIZE (mat_shader_list); ++i)
	{
		if (mat_shader_list[i] == material)
		{
			mat_shader_list[i] = VEC_LAST (mat_shader_list);
			VEC_POP (mat_shader_list);
			break;
		}
	}
}

static const shader_material_t *Mat_Shader_FindInternal (const char *name)
{
	unsigned int bucket;
	mat_shader_entry_t *entry;

	if (!name || !name[0])
		return NULL;

	bucket = Mat_Shader_Hash (name);
	for (entry = mat_shader_hash[bucket]; entry; entry = entry->next)
	{
		if (!q_strcasecmp (entry->material->name, name))
			return entry->material;
	}

	return NULL;
}

static int Mat_Shader_ReadFile (const char *path, const byte *data, size_t size, const char *label)
{
	char *buffer;
	int parsed;

	if (!data || size == 0)
		return 0;

	buffer = (char *) Z_Malloc (size + 1);
	memcpy (buffer, data, size);
	buffer[size] = '\0';
	parsed = Mat_Shader_ParseFile (path, buffer, label);
	Z_Free (buffer);

	return parsed;
}

static size_t Mat_Shader_LoadFromDirectory (const searchpath_t *search)
{
	char script_dir[MAX_OSPATH];
	findfile_t *find;
	size_t parsed_total = 0;

	if ((size_t) q_snprintf (script_dir, sizeof (script_dir), "%s/scripts", search->filename) >= sizeof (script_dir))
		return 0;

	for (find = Sys_FindFirst (script_dir, "shader"); find; find = Sys_FindNext (find))
	{
		char fullpath[MAX_OSPATH];
		char relpath[MAX_QPATH];
		byte *buffer;
		int handle;
		int size;

		if (find->attribs & FA_DIRECTORY)
			continue;

		q_snprintf (relpath, sizeof (relpath), "scripts/%s", find->name);
		q_snprintf (fullpath, sizeof (fullpath), "%s/%s", script_dir, find->name);

		size = (int) Sys_FileOpenRead (fullpath, &handle);
		if (size <= 0)
		{
			if (handle >= 0)
				Sys_FileClose (handle);
			continue;
		}

		buffer = (byte *) malloc (size + 1);
		if (!buffer)
		{
			Sys_FileClose (handle);
			continue;
		}

		if (Sys_FileRead (handle, buffer, size) != size)
		{
			free (buffer);
			Sys_FileClose (handle);
			continue;
		}

		Sys_FileClose (handle);
		buffer[size] = '\0';

		parsed_total += (size_t)Mat_Shader_ReadFile (relpath, buffer, (size_t) size, relpath);

		free (buffer);
	}

	return parsed_total;
}

static size_t Mat_Shader_LoadFromPack (const searchpath_t *search)
{
	int i;
	size_t parsed_total = 0;
	pack_t *pak = search->pack;

	for (i = 0; i < pak->numfiles; ++i)
	{
		const char *name = pak->files[i].name;
		size_t size = pak->files[i].filelen;
		byte *buffer = NULL;

		if (q_strncasecmp (name, "scripts/", 8))
			continue;
		if (q_strcasecmp (COM_FileGetExtension (name), "shader"))
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
			buffer = (byte *) malloc (size + 1);
			if (!buffer)
				continue;
			Sys_FileSeek (pak->handle, pak->files[i].filepos);
			if (Sys_FileRead (pak->handle, buffer, (int) size) != (int) size)
			{
				free (buffer);
				continue;
			}
		}

		if (buffer)
		{
			buffer[size] = '\0';
			parsed_total += (size_t)Mat_Shader_ReadFile (name, buffer, size, name);

			if (pak->is_pk3)
				MZ_FREE (buffer);
			else
				free (buffer);
		}
	}

	return parsed_total;
}

static void Mat_Shader_LoadAll (void)
{
	searchpath_t *search;
	searchpath_t **paths = NULL;
	size_t count = 0;
	size_t i;
	size_t parsed_total = 0;

	if (mat_shader_loaded && r_reloadshaders.value <= 0.f)
		return;

	Mat_Shader_Reset ();
	Mat_Shader_ParseResetStats ();
	mat_shader_loaded = true;

	if (r_shaders.value <= 0.f)
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
			parsed_total += Mat_Shader_LoadFromDirectory (search);
		else if (search->pack)
			parsed_total += Mat_Shader_LoadFromPack (search);
	}

	VEC_FREE (paths);

	if (Mat_Shader_ShouldWriteReport ())
		Mat_Shader_WriteReport ();

	Mat_Shader_LogSummary (parsed_total);
	if (r_matshader_debug_parse.value > 0.f)
	{
		Mat_Shader_DebugDumpShaders ();
		Mat_Shader_DebugDumpUnknowns ();
	}
}

static void Mat_Shader_Reload_f (cvar_t *var)
{
	if (var->value <= 0.f)
		return;
	Con_Printf ("Reloading material shaders\n");
	r_reloadshaders.value = 0.f;
	Mat_Shader_LoadAll ();
}

static void Mat_Shader_Fuzz_f (cvar_t *var)
{
	if (var->value <= 0.f)
		return;
	Mat_Shader_DebugFuzzParse ();
	var->value = 0.f;
}

static void Mat_Shader_List_f (void)
{
	size_t count = Mat_Shader_Count ();
	size_t limit = MAT_SHADER_LIST_LIMIT;
	size_t i;

	if (Cmd_Argc () > 1)
		limit = (size_t) q_max (0, Q_atoi (Cmd_Argv (1)));

	Con_Printf ("Loaded shaders: %zu\n", count);
	for (i = 0; i < count && i < limit; ++i)
	{
		const shader_material_t *material = Mat_Shader_GetByIndex (i);
		Con_Printf ("%s\n", material->name);
	}
}

static void Mat_Shader_Print_f (void)
{
	const shader_material_t *material;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("Usage: shaderprint <name>\n");
		return;
	}

	material = Mat_Shader_Find (Cmd_Argv (1));
	if (!material)
	{
		Con_Printf ("No shader named '%s'\n", Cmd_Argv (1));
		return;
	}

	Mat_Shader_Print (material);
}

static void Mat_Shader_FuzzCommand_f (void)
{
	Mat_Shader_DebugFuzzParse ();
}

void Mat_Shader_Init (void)
{
	Cvar_RegisterVariable (&r_shaders);
	Cvar_RegisterVariable (&r_shader_debug);
	Cvar_RegisterVariable (&r_matshader_debug_parse);
	Cvar_RegisterVariable (&r_reloadshaders);
	Cvar_RegisterVariable (&r_matshader_fuzz);
	Cvar_RegisterVariable (&r_matshader_report);
	Cvar_SetCallback (&r_reloadshaders, Mat_Shader_Reload_f);
	Cvar_SetCallback (&r_matshader_fuzz, Mat_Shader_Fuzz_f);

	Cmd_AddCommand ("shaderlist", Mat_Shader_List_f);
	Cmd_AddCommand ("shaderprint", Mat_Shader_Print_f);
	Cmd_AddCommand ("shaderfuzz", Mat_Shader_FuzzCommand_f);
}

void Mat_Shader_Shutdown (void)
{
	Mat_Shader_Reset ();
	mat_shader_loaded = false;
}

void Mat_Shader_Reload (void)
{
	Mat_Shader_LoadAll ();
}

size_t Mat_Shader_Count (void)
{
	Mat_Shader_LoadAll ();
	return VEC_SIZE (mat_shader_list);
}

const shader_material_t *Mat_Shader_GetByIndex (size_t index)
{
	Mat_Shader_LoadAll ();
	if (index >= VEC_SIZE (mat_shader_list))
		return NULL;
	return mat_shader_list[index];
}

void Mat_Shader_Canonicalize (const char *name, char *out, size_t out_size)
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

const shader_material_t *Mat_Shader_Find (const char *name)
{
	char canonical[MAX_QPATH];

	if (!name || !name[0])
		return NULL;

	Mat_Shader_LoadAll ();
	Mat_Shader_Canonicalize (name, canonical, sizeof (canonical));
	return Mat_Shader_FindInternal (canonical);
}

const shader_material_t *Mat_Shader_FindForTextureName (const char *texname, const char *mapname)
{
	char candidate[MAX_QPATH];
	const shader_material_t *material;

	if (!texname || !texname[0])
		return NULL;

	if (mapname && mapname[0])
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s/%s", mapname, texname);
		material = Mat_Shader_Find (candidate);
		if (material)
			return material;
	}

	q_snprintf (candidate, sizeof (candidate), "textures/%s", texname);
	material = Mat_Shader_Find (candidate);
	if (material)
		return material;

	return Mat_Shader_Find (texname);
}

const char *Mat_Shader_GetStage0Map (const shader_material_t *material, const char *texname)
{
	if (!material || (material->stage0.map_type != MAT_MAP_MAP && material->stage0.map_type != MAT_MAP_CLAMPMAP))
		return NULL;
	if (!material->stage0.map_path || !material->stage0.map_path[0])
		return NULL;

	if (texname && texname[0])
	{
		char canonical_tex[MAX_QPATH];
		Mat_Shader_Canonicalize (texname, canonical_tex, sizeof (canonical_tex));
		if (q_strcasecmp (material->name, canonical_tex))
			return NULL;
	}

	return material->stage0.map_path;
}

unsigned int Mat_Shader_GetTextureFlags (const shader_material_t *material)
{
	unsigned int flags = 0u;

	if (!material)
		return flags;

	if (material->render_flags & MAT_RENDER_NODRAW)
		flags |= MAT_SHADERFLAG_NODRAW;
	if (material->render_flags & MAT_RENDER_SKY)
		flags |= MAT_SHADERFLAG_SKY;
	if (material->render_flags & MAT_RENDER_TRANS)
		flags |= MAT_SHADERFLAG_TRANS;
	if (material->render_flags & MAT_RENDER_ALPHASHADOW)
		flags |= MAT_SHADERFLAG_ALPHASHADOW;
	if (material->render_flags & MAT_RENDER_FOG)
		flags |= MAT_SHADERFLAG_FOG;
	if (material->surfaceparms & MAT_SURFPARM_SOLID)
		flags |= MAT_SHADERFLAG_SOLID;
	if (material->surfaceparms & MAT_SURFPARM_NONSOLID)
		flags |= MAT_SHADERFLAG_NONSOLID;
	if (material->surfaceparms & MAT_SURFPARM_PLAYERCLIP)
		flags |= MAT_SHADERFLAG_PLAYERCLIP;
	if (material->surfaceparms & MAT_SURFPARM_MONSTERCLIP)
		flags |= MAT_SHADERFLAG_MONSTERCLIP;
	if (material->surfaceparms & MAT_SURFPARM_STONE)
		flags |= MAT_SHADERFLAG_STONE;
	if (material->emissive_enable)
		flags |= MAT_SHADERFLAG_EMISSIVE;
	if (material->bloom_enable)
		flags |= MAT_SHADERFLAG_BLOOM;
	if (material->godray_enable)
		flags |= MAT_SHADERFLAG_GODRAY;

	return flags;
}

void Mat_Shader_ApplyToTexture (texture_t *tex, const char *mapname)
{
	unsigned int flags;
	const shader_material_t *material = NULL;
	char candidate[MAX_QPATH];

	if (!tex)
		return;
	if (r_shaders.value <= 0.f)
		return;

	if (mapname && mapname[0])
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s/%s", mapname, tex->name);
		material = Mat_Shader_Find (candidate);
		if (material)
			tex->shader_map = Mat_Shader_GetStage0Map (material, candidate);
	}

	if (!material)
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s", tex->name);
		material = Mat_Shader_Find (candidate);
		if (material)
			tex->shader_map = Mat_Shader_GetStage0Map (material, candidate);
	}

	if (!material)
	{
		material = Mat_Shader_Find (tex->name);
		if (material)
			tex->shader_map = Mat_Shader_GetStage0Map (material, tex->name);
	}

	if (!material)
		return;

	flags = Mat_Shader_GetTextureFlags (material);

	tex->shader = material;
	tex->shader_flags = flags;

	if ((material->render_flags & MAT_RENDER_SKY) && tex->type != TEXTYPE_SKY)
		tex->type = TEXTYPE_SKY;
	if ((material->render_flags & MAT_RENDER_TRANS) && r_shader_debug.value >= 1.f)
		Con_DPrintf ("MatShader: surfaceparm trans on %s (TODO: blend path)\n", tex->name);

	if (r_shader_debug.value >= 1.f)
	{
		if (tex->shader_map)
			Con_Printf ("MatShader: %s overrides %s\n", tex->name, tex->shader_map);
	}
}

void Mat_Shader_Print (const shader_material_t *material)
{
	static const char *const cull_names[] = { "back", "front", "none" };
	static const char *const sort_names[] = { "opaque", "decal", "additive", "nearest" };
	static const char *const blend_names[] = { "replace", "alpha", "add", "mult", "premult", "custom" };
	static const char *const depth_names[] = { "lequal", "equal", "always" };
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
	Con_Printf ("  sort: %s\n", sort_names[q_min ((int)material->sort_key, (int)countof (sort_names) - 1)]);
	Con_Printf ("  polygon offset: %s\n", material->polygon_offset ? "on" : "off");
	Con_Printf ("  emissive: %s (scale %.2f)\n", material->emissive_enable ? "on" : "off", material->emissive_scale);
	Con_Printf ("  bloom: %s (scale %.2f)\n", material->bloom_enable ? "on" : "off", material->bloom_scale);
	Con_Printf ("  godray: %s (scale %.2f)\n", material->godray_enable ? "on" : "off", material->godray_scale);
	if (material->stage0.map_path || material->stage0.map_type != MAT_MAP_MAP)
	{
		const char *map_name = map_names[q_min ((int)material->stage0.map_type, (int)countof (map_names) - 1)];
		Con_Printf ("  stage0 map: %s (%s)\n", material->stage0.map_path ? material->stage0.map_path : "<builtin>", map_name);
		Con_Printf ("  stage0 blend: %s\n", blend_names[q_min ((int)material->stage0.blend_mode, (int)countof (blend_names) - 1)]);
		Con_Printf ("  stage0 depth: %s (write %s)\n",
			depth_names[q_min ((int)material->stage0.depth_func, (int)countof (depth_names) - 1)],
			material->stage0.depth_write ? "on" : "off");
	}
}

void Mat_Shader_Insert (shader_material_t *material)
{
	shader_material_t *owned;
	const shader_material_t *existing;

	if (!material || !material->name || !material->name[0])
		return;

	existing = Mat_Shader_FindInternal (material->name);
	if (existing)
	{
		Mat_Shader_Unregister (existing);
		Mat_Shader_FreeMaterial ((shader_material_t *) existing);
	}

	owned = (shader_material_t *) Z_Malloc (sizeof (*owned));
	memcpy (owned, material, sizeof (*owned));
	Mat_Shader_Register (owned);
}

void Mat_Shader_Remove (const shader_material_t *material)
{
	Mat_Shader_Unregister (material);
	Mat_Shader_FreeMaterial ((shader_material_t *) material);
}

void Mat_Shader_ReportUnknownToken (const char *token, mat_shader_keyword_scope_t scope, const char *context, const char *source_file, unsigned int line)
{
	char warn_key[MAX_QPATH * 3];
	char warn_context[MAX_QPATH * 2];
	char warn_location[MAX_QPATH * 2];
	const char *scope_name = Mat_Shader_ScopeName (scope);

	Mat_Shader_FormatSourceLine (warn_location, sizeof (warn_location), source_file, line);

	q_snprintf (warn_key, sizeof (warn_key), "%s::%s", scope_name, token);

	if (context && context[0])
		q_snprintf (warn_context, sizeof (warn_context), "%s (%s at %s)", context, scope_name, warn_location);
	else
		q_snprintf (warn_context, sizeof (warn_context), "shader (%s at %s)", scope_name, warn_location);

	Mat_Shader_WarnOnce (warn_key, token, warn_context);
	Mat_Shader_RecordUnknownToken (token, scope, context, source_file, line);
}

static int Mat_Shader_TimeBucket (float time, float fps_hint)
{
	float fps = fps_hint > 0.f ? fps_hint : 60.f;
	if (fps < 1.f)
		fps = 1.f;
	return (int)floorf (time * fps);
}

const mat_texmatrix_t *MatStage_EvalTexMatrix (mat_shader_stage_t *stage, float time)
{
	mat_texmatrix_t matrix;
	mat_texmatrix_t tmp;
	int bucket;
	int i;

	if (!stage)
		return NULL;

	bucket = Mat_Shader_TimeBucket (time, 60.f);
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

int MatStage_EvalAnimMapFrame (mat_shader_stage_t *stage, float time)
{
	int bucket;
	int frame_count;
	int frame;

	if (!stage || stage->anim_map_fps <= 0.f)
		return 0;

	frame_count = (int)VEC_SIZE (stage->anim_map_frames);
	if (frame_count <= 0)
		return 0;

	bucket = Mat_Shader_TimeBucket (time, stage->anim_map_fps);
	if (stage->anim_map_time_bucket == bucket)
		return stage->anim_map_frame;

	frame = bucket % frame_count;
	if (frame < 0)
		frame += frame_count;

	stage->anim_map_time_bucket = bucket;
	stage->anim_map_frame = frame;
	return frame;
}

const char *MatStage_GetAnimMapPath (mat_shader_stage_t *stage, float time)
{
	int frame;
	int frame_count;

	if (!stage)
		return NULL;

	frame_count = (int)VEC_SIZE (stage->anim_map_frames);
	if (frame_count <= 0)
		return NULL;

	frame = MatStage_EvalAnimMapFrame (stage, time);
	if (frame < 0 || frame >= frame_count)
		return NULL;

	return stage->anim_map_frames[frame];
}
