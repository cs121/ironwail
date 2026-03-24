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
#include <math.h>
#include <stdlib.h>

/*
Parsing policy:
- Unknown keys are ignored but tracked via warnings.
- Parse errors emit "expected vs got" warnings and resync.
- Stage-block errors skip the stage and resync to the end of the stage block.
- Material-block errors resync to the next top-level definition or EOF.
*/

typedef struct
{
	const char *name;
	unsigned int surfaceparm;
	unsigned int render_flags;
	unsigned int content_flags;
} surfaceparm_map_t;

static const surfaceparm_map_t mat_surfaceparm_table[] =
{
	{ "solid", MAT_SURFPARM_SOLID, 0u, MAT_CONTENT_SOLID },
	{ "nonsolid", MAT_SURFPARM_NONSOLID, 0u, MAT_CONTENT_NONSOLID },
	{ "playerclip", MAT_SURFPARM_PLAYERCLIP, 0u, MAT_CONTENT_PLAYERCLIP },
	{ "monsterclip", MAT_SURFPARM_MONSTERCLIP, 0u, MAT_CONTENT_MONSTERCLIP },
	{ "trans", MAT_SURFPARM_TRANS, MAT_RENDER_TRANS, 0u },
	{ "sky", MAT_SURFPARM_SKY, MAT_RENDER_SKY, 0u },
	{ "fog", MAT_SURFPARM_FOG, MAT_RENDER_FOG, 0u },
	{ "nodraw", MAT_SURFPARM_NODRAW, MAT_RENDER_NODRAW, 0u },
	{ "stone", MAT_SURFPARM_STONE, 0u, 0u }
};

typedef struct
{
	unsigned int token_count;
	qboolean token_limit_hit;
	const char *material_name;
	const char *source_file;
	unsigned int line;
	unsigned int token_line;
	qboolean track_tokens;
} mat_material_parse_state_t;

#define MATERIAL_SCALE_MAX 64.f
#define MATERIAL_TCMOD_MAX_ABS 64.f
#define MAX_TOKEN_CHARS Q_COUNTOF(com_token)

static size_t mat_material_parse_warnings;
static size_t mat_material_parse_errors;

void Material_ParseResetStats (void)
{
	mat_material_parse_warnings = 0;
	mat_material_parse_errors = 0;
}

void Material_ParseAddWarning (void)
{
	mat_material_parse_warnings++;
}

void Material_ParseAddError (void)
{
	mat_material_parse_errors++;
}

size_t Material_ParseGetWarnings (void)
{
	return mat_material_parse_warnings;
}

size_t Material_ParseGetErrors (void)
{
	return mat_material_parse_errors;
}

static void Material_WarnMaterial (const mat_material_parse_state_t *state, const char *message)
{
	Material_ParseAddWarning ();
	if (state && state->material_name)
		Con_Warning ("Material '%s': %s\n", state->material_name, message);
	else
		Con_Warning ("%s\n", message);
}

static void Material_WarnExpectedToken (const mat_material_parse_state_t *state, const char *expected, const char *got)
{
	const char *got_token = (got && got[0]) ? got : "<eof>";

	Material_ParseAddError ();
	if (state && state->material_name)
		Con_Warning ("Material '%s': expected %s, got '%s'\n", state->material_name, expected, got_token);
	else
		Con_Warning ("Expected %s, got '%s'\n", expected, got_token);
}

static void Material_TrackTokenLine (const char *data, mat_material_parse_state_t *state, const char **token_start)
{
	const char *cursor;
	unsigned int line;
	int c;

	if (token_start)
		*token_start = data;

	if (!state || !data)
		return;

	line = state->line ? state->line : 1;
	cursor = data;

skipwhite:
	while ((c = *cursor) <= ' ')
	{
		if (c == 0)
		{
			state->line = line;
			state->token_line = line;
			if (token_start)
				*token_start = cursor;
			return;
		}
		if (c == '\n')
			line++;
		cursor++;
	}

	if (c == '/' && cursor[1] == '/')
	{
		while (*cursor && *cursor != '\n')
			cursor++;
		goto skipwhite;
	}

	if (c == '/' && cursor[1] == '*')
	{
		cursor += 2;
		while (*cursor && !(*cursor == '*' && cursor[1] == '/'))
		{
			if (*cursor == '\n')
				line++;
			cursor++;
		}
		if (*cursor)
			cursor += 2;
		goto skipwhite;
	}

	state->line = line;
	state->token_line = line;
	if (token_start)
		*token_start = cursor;
}

static const char *Material_ParseToken (const char *data, mat_material_parse_state_t *state)
{
	const char *token_start = data;
	const char *cursor;

	if (state)
		Material_TrackTokenLine (data, state, &token_start);

	cursor = COM_Parse (data);
	if (!cursor)
		return NULL;

	if (state)
	{
		for (const char *scan = token_start; scan < cursor; ++scan)
		{
			if (*scan == '\n')
				state->line++;
		}

		if (state->track_tokens && !state->token_limit_hit)
		{
			state->token_count++;
			if (state->token_count > MATERIAL_MAX_TOKENS)
			{
				state->token_limit_hit = true;
				Material_WarnMaterial (state, "token limit exceeded; skipping remaining tokens");
			}
		}
	}

	return cursor;
}

static qboolean Material_ParseBool (const char *token, qboolean default_value, const mat_material_parse_state_t *state)
{
	float value;
	char message[128];

	if (!token || !token[0])
		return default_value;
	if (!q_strcasecmp (token, "true") || !q_strcasecmp (token, "yes") || !q_strcasecmp (token, "on"))
		return true;
	if (!q_strcasecmp (token, "false") || !q_strcasecmp (token, "no") || !q_strcasecmp (token, "off"))
		return false;
	value = Q_atof (token);
	if (!isfinite (value))
	{
		q_snprintf (message, sizeof (message), "boolean value '%s' is not finite; using default", token);
		Material_WarnMaterial (state, message);
		return default_value;
	}
	return value != 0.f;
}

static qboolean Material_IsBraceToken (const char *token)
{
	if (!token || !token[0])
		return false;
	return !strcmp (token, "{") || !strcmp (token, "}");
}

static qboolean Material_IsNumericToken (const char *token)
{
	size_t i;
	qboolean has_digit = false;

	if (!token || !token[0])
		return false;

	/* Optional leading sign */
	i = 0;
	if (token[i] == '+' || token[i] == '-')
		i++;

	for (; token[i]; ++i)
	{
		if (token[i] >= '0' && token[i] <= '9')
		{
			has_digit = true;
			continue;
		}
		if (token[i] == '.')
			continue;
		if ((token[i] == 'e' || token[i] == 'E') && has_digit)
		{
			/* Optional exponent sign followed by digits */
			i++;
			if (token[i] == '+' || token[i] == '-')
				i++;
			for (; token[i]; ++i)
			{
				if (token[i] >= '0' && token[i] <= '9')
					has_digit = true;
				else
					return false;
			}
			break;
		}
		return false;
	}

	return has_digit;
}

static qboolean Material_IsBoolToken (const char *token)
{
	if (!token || !token[0])
		return false;
	if (!q_strcasecmp (token, "true") || !q_strcasecmp (token, "false") ||
		!q_strcasecmp (token, "yes") || !q_strcasecmp (token, "no") ||
		!q_strcasecmp (token, "on") || !q_strcasecmp (token, "off"))
		return true;
	return Material_IsNumericToken (token);
}

static qboolean Material_ParseLine (const char **data, mat_material_parse_state_t *state)
{
	const char *cursor;
	const char *line_end;
	stringview_t line;

	if (!data || !*data)
		return false;

	cursor = *data;
	if (!COM_ParseLine (data, &line))
		return false;

	if (state)
	{
		/* Count all newlines consumed, not just the terminating one */
		line_end = cursor + line.len;
		for (; cursor <= line_end; ++cursor)
		{
			if (*cursor == '\n')
				state->line++;
			if (cursor == line_end)
				break;
		}
	}

	return true;
}

static qboolean ParseOptionalBool (const char **data, qboolean *out, mat_material_parse_state_t *state)
{
	const char *cursor;

	if (!data || !*data || !out)
		return false;

	cursor = Material_ParseToken (*data, state);
	if (!cursor || !com_token[0])
		return false;

	if (state && state->token_limit_hit)
		return false;

	if (!Material_IsBoolToken (com_token))
		return false;

	*out = Material_ParseBool (com_token, false, state);
	*data = cursor;
	return true;
}

static qboolean ParseIdent (const char **data, const char **out, mat_material_parse_state_t *state)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = Material_ParseToken (*data, state);
	if (!cursor)
		return false;

	*data = cursor;
	if (!com_token[0])
		return false;

	if (state && state->token_limit_hit)
		return false;

	*out = com_token;
	return true;
}

static qboolean ParseIdentExpected (const char **data, const char **out, mat_material_parse_state_t *state, const char *expected)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = Material_ParseToken (*data, state);
	if (!cursor)
	{
		Material_WarnExpectedToken (state, expected, "<eof>");
		return false;
	}

	*data = cursor;
	if (state && state->token_limit_hit)
		return false;
	if (!com_token[0] || Material_IsBraceToken (com_token))
	{
		Material_WarnExpectedToken (state, expected, com_token);
		return false;
	}

	*out = com_token;
	return true;
}

static const char *StripCommaToken (const char *token, char *buffer, size_t buffer_size)
{
	const char *start;
	const char *end;
	size_t len;

	if (!token || !buffer || !buffer_size)
		return token;

	start = token;
	while (*start == ',')
		start++;

	end = token + strlen (token);
	while (end > start && end[-1] == ',')
		end--;

	len = (size_t)(end - start);
	if (len >= buffer_size)
		len = buffer_size - 1;

	/* Use memmove: buffer may alias token (e.g. StripCommaToken(buf, buf, size)) */
	memmove (buffer, start, len);
	buffer[len] = '\0';
	return buffer;
}

static qboolean ParseFloat (const char **data, float *out, mat_material_parse_state_t *state)
{
	const char *token;

	if (!ParseIdentExpected (data, &token, state, "float"))
		return false;

	if (!Material_IsNumericToken (token))
	{
		Material_WarnExpectedToken (state, "float", token);
		return false;
	}

	*out = Q_atof (token);
	return true;
}

static qboolean ParseVec2 (const char **data, vec2_t out, mat_material_parse_state_t *state)
{
	if (!ParseFloat (data, &out[0], state))
		return false;
	if (!ParseFloat (data, &out[1], state))
		return false;
	return true;
}

static qboolean ParseVec4 (const char **data, vec4_t out, mat_material_parse_state_t *state)
{
	if (!ParseFloat (data, &out[0], state))
		return false;
	if (!ParseFloat (data, &out[1], state))
		return false;
	if (!ParseFloat (data, &out[2], state))
		return false;
	if (!ParseFloat (data, &out[3], state))
		return false;
	return true;
}

static qboolean ParseWaveType (const char *token, mat_wave_type_t *out)
{
	if (!token || !token[0])
		return false;
	if (!q_strcasecmp (token, "sin"))
	{
		*out = MAT_WAVE_SIN;
		return true;
	}
	if (!q_strcasecmp (token, "triangle"))
	{
		*out = MAT_WAVE_TRIANGLE;
		return true;
	}
	if (!q_strcasecmp (token, "saw"))
	{
		*out = MAT_WAVE_SAW;
		return true;
	}
	if (!q_strcasecmp (token, "inversesaw"))
	{
		*out = MAT_WAVE_INVERSESAW;
		return true;
	}
	return false;
}

static qboolean ParseRequiredBool (const char **data, qboolean *out, mat_material_parse_state_t *state)
{
	const char *token;

	if (!ParseIdentExpected (data, &token, state, "boolean"))
		return false;
	if (!Material_IsBoolToken (token))
	{
		Material_WarnExpectedToken (state, "boolean", token);
		return false;
	}
	*out = Material_ParseBool (token, false, state);
	return true;
}

static qboolean Material_ValidateFiniteFloat (const mat_material_parse_state_t *state, const char *label, float value, float default_value, float *out)
{
	char message[128];

	if (isfinite (value))
	{
		*out = value;
		return true;
	}

	q_snprintf (message, sizeof (message), "%s is not finite; using default", label);
	Material_WarnMaterial (state, message);
	*out = default_value;
	return false;
}

static qboolean Material_ValidateFloatMin (const mat_material_parse_state_t *state, const char *label, float value, float min_value, float default_value, float *out)
{
	char message[128];

	if (!isfinite (value))
	{
		q_snprintf (message, sizeof (message), "%s is not finite; using default", label);
		Material_WarnMaterial (state, message);
		*out = default_value;
		return false;
	}
	if (value < min_value)
	{
		q_snprintf (message, sizeof (message), "%s below %.2f; using default", label, min_value);
		Material_WarnMaterial (state, message);
		*out = default_value;
		return false;
	}

	*out = value;
	return true;
}

static void Material_ValidateTcModArgs (const mat_material_parse_state_t *state, const char *label, float *values, const float *defaults, int count)
{
	char message[128];

	for (int i = 0; i < count; ++i)
	{
		if (!isfinite (values[i]))
		{
			q_snprintf (message, sizeof (message), "%s has non-finite values; using defaults", label);
			Material_WarnMaterial (state, message);
			for (int j = 0; j < count; ++j)
				values[j] = defaults[j];
			return;
		}
	}

	for (int i = 0; i < count; ++i)
		values[i] = CLAMP (-MATERIAL_TCMOD_MAX_ABS, values[i], MATERIAL_TCMOD_MAX_ABS);
}

static qboolean Material_ParseTcGen (const char *token, mat_tcgen_t *out)
{
	if (!token || !out)
		return false;
	if (!q_strcasecmp (token, "base"))
	{
		*out = MAT_TCGEN_BASE;
		return true;
	}
	if (!q_strcasecmp (token, "environment"))
	{
		*out = MAT_TCGEN_ENVIRONMENT;
		return true;
	}
	if (!q_strcasecmp (token, "lightmap"))
	{
		*out = MAT_TCGEN_LIGHTMAP;
		return true;
	}
	return false;
}

static qboolean Material_PushTcMod (material_stage_t *stage, mat_tcmod_type_t type, const float *args, int arg_count)
{
	mat_tcmod_t mod;
	int i;

	if (!stage || stage->tcmod_count >= (int)countof (stage->tcmods))
		return false;

	memset (&mod, 0, sizeof (mod));
	mod.type = type;
	for (i = 0; i < arg_count && i < (int)countof (mod.args); ++i)
		mod.args[i] = args[i];

	stage->tcmods[stage->tcmod_count++] = mod;
	return true;
}

static qboolean ExpectToken (const char **data, const char *token, mat_material_parse_state_t *state)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = Material_ParseToken (*data, state);
	if (!cursor)
	{
		Material_WarnExpectedToken (state, token, "<eof>");
		return false;
	}

	*data = cursor;
	if (state && state->token_limit_hit)
		return false;
	if (strcmp (com_token, token))
	{
		Material_WarnExpectedToken (state, token, com_token);
		return false;
	}
	return true;
}

static qboolean Material_ParseCullMode (const char *token, mat_cull_mode_t *out)
{
	if (!token || !out)
		return false;
	if (!q_strcasecmp (token, "back"))
	{
		*out = MAT_CULL_BACK;
		return true;
	}
	if (!q_strcasecmp (token, "front"))
	{
		*out = MAT_CULL_FRONT;
		return true;
	}
	if (!q_strcasecmp (token, "none")
		|| !q_strcasecmp (token, "disable")
		|| !q_strcasecmp (token, "disabled")
		|| !q_strcasecmp (token, "off"))
	{
		*out = MAT_CULL_NONE;
		return true;
	}
	return false;
}

static qboolean Material_ParseSortKey (const char *token, mat_sort_key_t *out)
{
	char *endptr;
	double numeric_value;

	if (!token || !out)
		return false;
	if (token[0] == '-' || token[0] == '+' || (token[0] >= '0' && token[0] <= '9'))
	{
		numeric_value = strtod (token, &endptr);
		if (endptr && *endptr == '\0')
		{
			if (numeric_value <= MAT_SORT_SKY)
				*out = MAT_SORT_SKY;
			else if (numeric_value <= MAT_SORT_OPAQUE)
				*out = MAT_SORT_OPAQUE;
			else if (numeric_value <= MAT_SORT_SEE_THROUGH)
				*out = MAT_SORT_SEE_THROUGH;
			else if (numeric_value <= MAT_SORT_DECAL)
				*out = MAT_SORT_DECAL;
			else if (numeric_value <= MAT_SORT_BANNER)
				*out = MAT_SORT_BANNER;
			else if (numeric_value <= MAT_SORT_UNDERWATER)
				*out = MAT_SORT_UNDERWATER;
			else if (numeric_value <= MAT_SORT_ADDITIVE)
				*out = MAT_SORT_ADDITIVE;
			else
				*out = MAT_SORT_NEAREST;
			return true;
		}
	}
	if (!q_strcasecmp (token, "opaque"))
	{
		*out = MAT_SORT_OPAQUE;
		return true;
	}
	if (!q_strcasecmp (token, "sky"))
	{
		*out = MAT_SORT_SKY;
		return true;
	}
	if (!q_strcasecmp (token, "seethrough") || !q_strcasecmp (token, "seeThrough"))
	{
		*out = MAT_SORT_SEE_THROUGH;
		return true;
	}
	if (!q_strcasecmp (token, "decal"))
	{
		*out = MAT_SORT_DECAL;
		return true;
	}
	if (!q_strcasecmp (token, "banner"))
	{
		*out = MAT_SORT_BANNER;
		return true;
	}
	if (!q_strcasecmp (token, "underwater"))
	{
		*out = MAT_SORT_UNDERWATER;
		return true;
	}
	if (!q_strcasecmp (token, "additive"))
	{
		*out = MAT_SORT_ADDITIVE;
		return true;
	}
	if (!q_strcasecmp (token, "nearest"))
	{
		*out = MAT_SORT_NEAREST;
		return true;
	}
	return false;
}

static qboolean Material_ParseBlendFactor (const char *token, int *out)
{
	const char *name;

	if (!token || !out)
		return false;

	if (!q_strncasecmp (token, "GL_", 3))
		name = token + 3;
	else
		name = token;

	if (!q_strcasecmp (name, "ONE"))
		*out = GL_ONE;
	else if (!q_strcasecmp (name, "ZERO"))
		*out = GL_ZERO;
	else if (!q_strcasecmp (name, "SRC_ALPHA"))
		*out = GL_SRC_ALPHA;
	else if (!q_strcasecmp (name, "ONE_MINUS_SRC_ALPHA"))
		*out = GL_ONE_MINUS_SRC_ALPHA;
	else if (!q_strcasecmp (name, "DST_ALPHA"))
		*out = GL_DST_ALPHA;
	else if (!q_strcasecmp (name, "ONE_MINUS_DST_ALPHA"))
		*out = GL_ONE_MINUS_DST_ALPHA;
	else if (!q_strcasecmp (name, "SRC_COLOR"))
		*out = GL_SRC_COLOR;
	else if (!q_strcasecmp (name, "ONE_MINUS_SRC_COLOR"))
		*out = GL_ONE_MINUS_SRC_COLOR;
	else if (!q_strcasecmp (name, "DST_COLOR"))
		*out = GL_DST_COLOR;
	else if (!q_strcasecmp (name, "ONE_MINUS_DST_COLOR"))
		*out = GL_ONE_MINUS_DST_COLOR;
	else
		return false;

	return true;
}

static qboolean Material_ParseBlendMode (const char *token, mat_blend_mode_t *out)
{
	if (!token || !out)
		return false;
	if (!q_strcasecmp (token, "add"))
	{
		*out = MAT_BLEND_ADD;
		return true;
	}
	if (!q_strcasecmp (token, "filter") || !q_strcasecmp (token, "multiply") || !q_strcasecmp (token, "mult"))
	{
		*out = MAT_BLEND_MULT;
		return true;
	}
	if (!q_strcasecmp (token, "blend") || !q_strcasecmp (token, "alpha"))
	{
		*out = MAT_BLEND_ALPHA;
		return true;
	}
	if (!q_strcasecmp (token, "premult") || !q_strcasecmp (token, "premultiplied"))
	{
		*out = MAT_BLEND_PREMULT;
		return true;
	}
	return false;
}

static qboolean Material_ParseDepthFunc (const char *token, mat_depthfunc_t *out)
{
	if (!token || !out)
		return false;
	if (!q_strcasecmp (token, "lequal"))
	{
		*out = MAT_DEPTHFUNC_LEQUAL;
		return true;
	}
	if (!q_strcasecmp (token, "less"))
	{
		*out = MAT_DEPTHFUNC_LESS;
		return true;
	}
	if (!q_strcasecmp (token, "equal"))
	{
		*out = MAT_DEPTHFUNC_EQUAL;
		return true;
	}
	if (!q_strcasecmp (token, "greater"))
	{
		*out = MAT_DEPTHFUNC_GREATER;
		return true;
	}
	if (!q_strcasecmp (token, "gequal"))
	{
		*out = MAT_DEPTHFUNC_GEQUAL;
		return true;
	}
	if (!q_strcasecmp (token, "always"))
	{
		*out = MAT_DEPTHFUNC_ALWAYS;
		return true;
	}
	if (!q_strcasecmp (token, "never"))
	{
		*out = MAT_DEPTHFUNC_NEVER;
		return true;
	}
	return false;
}

static void Material_ApplySurfaceParm (material_t *material, const char *token, const mat_material_parse_state_t *state)
{
	for (size_t i = 0; i < countof (mat_surfaceparm_table); ++i)
	{
		if (!q_strcasecmp (token, mat_surfaceparm_table[i].name))
		{
			Material_MarkKeywordSeen (mat_surfaceparm_table[i].name, MATERIAL_KEYWORD_SCOPE_SURFACEPARM);
			material->surfaceparms |= mat_surfaceparm_table[i].surfaceparm;
			material->render_flags |= mat_surfaceparm_table[i].render_flags;
			material->content_flags |= mat_surfaceparm_table[i].content_flags;
			return;
		}
	}

	Material_ReportUnknownToken (token, MATERIAL_KEYWORD_SCOPE_SURFACEPARM, material->name,
		state ? state->source_file : material->source_file,
		state ? state->token_line : 0u);
}

static const char *SkipUnknownBlockOrLine (const char *data, qboolean already_open, mat_material_parse_state_t *state)
{
	int depth = 1;
	const char *cursor = data;

	if (!cursor)
		return NULL;

	if (!already_open)
	{
		cursor = Material_ParseToken (cursor, state);
		if (!cursor)
			return NULL;
		if (!com_token[0])
			return cursor;
		if (!strcmp (com_token, "{"))
			depth = 1;
		else
		{
			if (!Material_ParseLine (&cursor, state))
				return NULL;
			return cursor;
		}
	}

	while ((cursor = Material_ParseToken (cursor, state)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
		{
			depth++;
			if (depth > MATERIAL_MAX_BRACE_DEPTH)
			{
				Material_WarnMaterial (state, "brace depth limit exceeded while skipping unknown block");
				return NULL;
			}
		}
		else if (!strcmp (com_token, "}"))
		{
			depth--;
			if (depth <= 0)
				break;
		}
	}
	return cursor;
}

static const char *ResyncMaterialBlock (const char *data, mat_material_parse_state_t *state)
{
	int depth = 1;
	const char *cursor = data;

	while ((cursor = Material_ParseToken (cursor, state)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
		{
			depth++;
			if (depth > MATERIAL_MAX_BRACE_DEPTH)
			{
				Material_WarnMaterial (state, "brace depth limit exceeded while resyncing material");
				return NULL;
			}
		}
		else if (!strcmp (com_token, "}"))
		{
			depth--;
			if (depth <= 0)
				return cursor;
		}
	}

	return NULL;
}

static const char *ParseStageBlock (const char *data, material_t *material, size_t stage_index, mat_material_parse_state_t *state, qboolean *stage_valid)
{
	material_stage_t stage;
	qboolean valid = true;

	memset (&stage, 0, sizeof (stage));
	stage.outputs = MAT_STAGE_OUT_COLOR;
	stage.rgbgen = MAT_RGBGEN_IDENTITY;
	stage.alphagen = MAT_ALPHAGEN_IDENTITY;
	stage.const_color[0] = 1.f;
	stage.const_color[1] = 1.f;
	stage.const_color[2] = 1.f;
	stage.const_alpha = 1.f;
	stage.rgb_wave.type = MAT_WAVE_SIN;
	stage.alpha_wave.type = MAT_WAVE_SIN;
	stage.blend_mode = MAT_BLEND_REPLACE;
	stage.depth_write = true;
	stage.depth_func = MAT_DEPTHFUNC_LEQUAL;
	stage.map_type = MAT_MAP_MAP;
	stage.blend_src = GL_ONE;
	stage.blend_dst = GL_ZERO;
	stage.tcgen = MAT_TCGEN_BASE;
	stage.anim_map_fps = 0.f;
	stage.anim_map_frame = 0;
	stage.texmatrix_time_bucket = -1;
	stage.anim_map_time_bucket = -1;

	while ((data = Material_ParseToken (data, state)) != NULL)
	{
		const char *value;

		if (!com_token[0])
			continue;
		if (state && state->token_limit_hit)
		{
			valid = false;
			data = SkipUnknownBlockOrLine (data, true, NULL);
			break;
		}
		if (!strcmp (com_token, "}"))
			break;
		if (!q_strcasecmp (com_token, "map"))
		{
			Material_MarkKeywordSeen ("map", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "map path"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (stage.map_path)
			{
				Z_Free (stage.map_path);
				stage.map_path = NULL;
			}
			if (!q_strcasecmp (value, "$lightmap"))
			{
				stage.map_type = MAT_MAP_LIGHTMAP;
			}
			else if (!q_strcasecmp (value, "$whiteimage") || !q_strcasecmp (value, "$white"))
			{
				stage.map_type = MAT_MAP_WHITE;
			}
			else if (!q_strcasecmp (value, "$blackimage") || !q_strcasecmp (value, "$black"))
			{
				stage.map_type = MAT_MAP_BLACK;
			}
			else
			{
				stage.map_type = MAT_MAP_MAP;
				stage.map_path = Material_DupString (value);
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "clampmap"))
		{
			Material_MarkKeywordSeen ("clampmap", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "map path"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (stage.map_path)
			{
				Z_Free (stage.map_path);
				stage.map_path = NULL;
			}
			stage.map_type = MAT_MAP_CLAMPMAP;
			stage.map_path = Material_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "rgbGen"))
		{
			Material_MarkKeywordSeen ("rgbGen", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "rgbGen mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!q_strcasecmp (value, "identity"))
				stage.rgbgen = MAT_RGBGEN_IDENTITY;
			else if (!q_strcasecmp (value, "vertex"))
				stage.rgbgen = MAT_RGBGEN_VERTEX;
			else if (!q_strcasecmp (value, "const"))
			{
				const char *token;
				float color[3];

				if (!ParseIdentExpected (&data, &token, state, "rgbGen const"))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}

				if (!q_strcasecmp (token, "("))
				{
					if (!ParseFloat (&data, &color[0], state)
						|| !ParseFloat (&data, &color[1], state)
						|| !ParseFloat (&data, &color[2], state))
					{
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
					if (!ExpectToken (&data, ")", state))
					{
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
				}
				else
				{
					if (!Material_IsNumericToken (token))
					{
						Material_WarnExpectedToken (state, "float", token);
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
					color[0] = Q_atof (token);
					if (!ParseFloat (&data, &color[1], state)
						|| !ParseFloat (&data, &color[2], state))
					{
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
				}

				stage.rgbgen = MAT_RGBGEN_CONST;
				stage.const_color[0] = color[0];
				stage.const_color[1] = color[1];
				stage.const_color[2] = color[2];
			}
			else if (!q_strcasecmp (value, "wave"))
			{
				mat_wave_type_t wave_type;
				float base;
				float amp;
				float phase;
				float freq;

				if (!ParseIdentExpected (&data, &value, state, "rgbGen wave type"))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				if (!ParseWaveType (value, &wave_type))
				{
					Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
						state ? state->source_file : material->source_file,
						state ? state->token_line : 0u);
					continue;
				}
				if (!ParseFloat (&data, &base, state)
					|| !ParseFloat (&data, &amp, state)
					|| !ParseFloat (&data, &phase, state)
					|| !ParseFloat (&data, &freq, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}

				Material_ValidateFiniteFloat (state, "rgbGen wave base", base, 0.f, &stage.rgb_wave.base);
				Material_ValidateFiniteFloat (state, "rgbGen wave amp", amp, 0.f, &stage.rgb_wave.amp);
				Material_ValidateFiniteFloat (state, "rgbGen wave phase", phase, 0.f, &stage.rgb_wave.phase);
				Material_ValidateFiniteFloat (state, "rgbGen wave freq", freq, 0.f, &stage.rgb_wave.freq);
				stage.rgb_wave.type = wave_type;
				stage.rgbgen = MAT_RGBGEN_WAVE;
			}
			else
			{
				Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "alphaGen"))
		{
			Material_MarkKeywordSeen ("alphaGen", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "alphaGen mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!q_strcasecmp (value, "identity"))
				stage.alphagen = MAT_ALPHAGEN_IDENTITY;
			else if (!q_strcasecmp (value, "vertex"))
				stage.alphagen = MAT_ALPHAGEN_VERTEX;
			else if (!q_strcasecmp (value, "const"))
			{
				const char *token;
				float alpha;

				if (!ParseIdentExpected (&data, &token, state, "alphaGen const"))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}

				if (!q_strcasecmp (token, "("))
				{
					if (!ParseFloat (&data, &alpha, state))
					{
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
					if (!ExpectToken (&data, ")", state))
					{
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
				}
				else
				{
					if (!Material_IsNumericToken (token))
					{
						Material_WarnExpectedToken (state, "float", token);
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
					alpha = Q_atof (token);
				}

				stage.alphagen = MAT_ALPHAGEN_CONST;
				stage.const_alpha = alpha;
			}
			else if (!q_strcasecmp (value, "wave"))
			{
				mat_wave_type_t wave_type;
				float base;
				float amp;
				float phase;
				float freq;

				if (!ParseIdentExpected (&data, &value, state, "alphaGen wave type"))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				if (!ParseWaveType (value, &wave_type))
				{
					Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
						state ? state->source_file : material->source_file,
						state ? state->token_line : 0u);
					continue;
				}
				if (!ParseFloat (&data, &base, state)
					|| !ParseFloat (&data, &amp, state)
					|| !ParseFloat (&data, &phase, state)
					|| !ParseFloat (&data, &freq, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}

				Material_ValidateFiniteFloat (state, "alphaGen wave base", base, 0.f, &stage.alpha_wave.base);
				Material_ValidateFiniteFloat (state, "alphaGen wave amp", amp, 0.f, &stage.alpha_wave.amp);
				Material_ValidateFiniteFloat (state, "alphaGen wave phase", phase, 0.f, &stage.alpha_wave.phase);
				Material_ValidateFiniteFloat (state, "alphaGen wave freq", freq, 0.f, &stage.alpha_wave.freq);
				stage.alpha_wave.type = wave_type;
				stage.alphagen = MAT_ALPHAGEN_WAVE;
			}
			else
			{
				Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "blendFunc"))
		{
			Material_MarkKeywordSeen ("blendFunc", MATERIAL_KEYWORD_SCOPE_STAGE);
			int src;
			int dst;
			char mode_buffer[MAX_TOKEN_CHARS];
			const char *mode_token;
			const char *comma;

			if (!ParseIdentExpected (&data, &value, state, "blendFunc mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}

			mode_token = StripCommaToken (value, mode_buffer, sizeof (mode_buffer));
			comma = strchr (mode_token, ',');
			if (comma)
			{
				char src_buffer[MAX_TOKEN_CHARS];
				char dst_buffer[MAX_TOKEN_CHARS];
				const char *dst_start = comma + 1;
				size_t src_len = (size_t)(comma - mode_token);
				const char *src_token;
				const char *dst_token;

				if (src_len >= sizeof (src_buffer))
					src_len = sizeof (src_buffer) - 1;
				memcpy (src_buffer, mode_token, src_len);
				src_buffer[src_len] = '\0';

				src_token = StripCommaToken (src_buffer, src_buffer, sizeof (src_buffer));
				dst_token = StripCommaToken (dst_start, dst_buffer, sizeof (dst_buffer));
				if (src_token[0] && dst_token[0]
					&& Material_ParseBlendFactor (src_token, &src)
					&& Material_ParseBlendFactor (dst_token, &dst))
				{
					stage.blend_mode = MAT_BLEND_CUSTOM;
					stage.blend_src = src;
					stage.blend_dst = dst;
					continue;
				}
				Material_ReportUnknownToken (mode_token, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
				continue;
			}

			if (Material_ParseBlendMode (mode_token, &stage.blend_mode))
			{
				switch (stage.blend_mode)
				{
				case MAT_BLEND_ALPHA:
					stage.blend_src = GL_SRC_ALPHA;
					stage.blend_dst = GL_ONE_MINUS_SRC_ALPHA;
					break;
				case MAT_BLEND_ADD:
					stage.blend_src = GL_ONE;
					stage.blend_dst = GL_ONE;
					break;
				case MAT_BLEND_MULT:
					stage.blend_src = GL_ZERO;
					stage.blend_dst = GL_SRC_COLOR;
					break;
				case MAT_BLEND_PREMULT:
					stage.blend_src = GL_ONE;
					stage.blend_dst = GL_ONE_MINUS_SRC_ALPHA;
					break;
				default:
					stage.blend_src = GL_ONE;
					stage.blend_dst = GL_ZERO;
					break;
				}
				continue;
			}

			if (Material_ParseBlendFactor (mode_token, &src))
			{
				char dst_buffer[MAX_TOKEN_CHARS];
				const char *dst_token;

				if (!ParseIdentExpected (&data, &value, state, "blendFunc dst"))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}

				dst_token = StripCommaToken (value, dst_buffer, sizeof (dst_buffer));
				if (!Material_ParseBlendFactor (dst_token, &dst))
				{
					Material_ReportUnknownToken (dst_token, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
						state ? state->source_file : material->source_file,
						state ? state->token_line : 0u);
					continue;
				}
				stage.blend_mode = MAT_BLEND_CUSTOM;
				stage.blend_src = src;
				stage.blend_dst = dst;
				continue;
			}

			Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
				state ? state->source_file : material->source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "depthWrite"))
		{
			Material_MarkKeywordSeen ("depthWrite", MATERIAL_KEYWORD_SCOPE_STAGE);
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			stage.depth_write = parsed ? value_bool : true;
			continue;
		}
		if (!q_strcasecmp (com_token, "depthFunc"))
		{
			Material_MarkKeywordSeen ("depthFunc", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "depthFunc mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (Material_ParseDepthFunc (value, &stage.depth_func))
				continue;
			stage.depth_func = MAT_DEPTHFUNC_LEQUAL;
			Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
				state ? state->source_file : material->source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcGen"))
		{
			Material_MarkKeywordSeen ("tcGen", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "tcGen mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!Material_ParseTcGen (value, &stage.tcgen))
				Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcMod"))
		{
			Material_MarkKeywordSeen ("tcMod", MATERIAL_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "tcMod type"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!q_strcasecmp (value, "scroll"))
			{
				vec2_t scroll = { 0.f, 0.f };
				const vec2_t scroll_defaults = { 0.f, 0.f };
				if (!ParseVec2 (&data, scroll, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Material_ValidateTcModArgs (state, "tcMod scroll", scroll, scroll_defaults, 2);
				if (!Material_PushTcMod (&stage, MAT_TCMOD_SCROLL, scroll, 2))
				{
					Material_WarnMaterial (state, "tcMod limit exceeded; ignoring extra modifiers");
					stage.tcmod_overflow = true;
					if (r_particles_material_strict.value > 0.f)
						valid = false;
				}
				continue;
			}
			if (!q_strcasecmp (value, "scale"))
			{
				vec2_t scale = { 1.f, 1.f };
				const vec2_t scale_defaults = { 1.f, 1.f };
				if (!ParseVec2 (&data, scale, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Material_ValidateTcModArgs (state, "tcMod scale", scale, scale_defaults, 2);
				if (!Material_PushTcMod (&stage, MAT_TCMOD_SCALE, scale, 2))
				{
					Material_WarnMaterial (state, "tcMod limit exceeded; ignoring extra modifiers");
					stage.tcmod_overflow = true;
					if (r_particles_material_strict.value > 0.f)
						valid = false;
				}
				continue;
			}
			if (!q_strcasecmp (value, "rotate"))
			{
				float deg_per_sec = 0.f;
				const float rotate_default = 0.f;
				if (!ParseFloat (&data, &deg_per_sec, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Material_ValidateTcModArgs (state, "tcMod rotate", &deg_per_sec, &rotate_default, 1);
				if (!Material_PushTcMod (&stage, MAT_TCMOD_ROTATE, &deg_per_sec, 1))
				{
					Material_WarnMaterial (state, "tcMod limit exceeded; ignoring extra modifiers");
					stage.tcmod_overflow = true;
					if (r_particles_material_strict.value > 0.f)
						valid = false;
				}
				continue;
			}
			if (!q_strcasecmp (value, "turb"))
			{
				vec4_t turb = { 0.f, 0.f, 0.f, 0.f };
				const vec4_t turb_defaults = { 0.f, 0.f, 0.f, 0.f };
				if (!ParseVec4 (&data, turb, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Material_ValidateTcModArgs (state, "tcMod turb", turb, turb_defaults, 4);
				if (!Material_PushTcMod (&stage, MAT_TCMOD_TURB, turb, 4))
				{
					Material_WarnMaterial (state, "tcMod limit exceeded; ignoring extra modifiers");
					stage.tcmod_overflow = true;
					if (r_particles_material_strict.value > 0.f)
						valid = false;
				}
				continue;
			}
			if (!q_strcasecmp (value, "stretch"))
			{
				vec4_t stretch = { 0.f, 0.f, 0.f, 0.f };
				const vec4_t stretch_defaults = { 0.f, 0.f, 0.f, 0.f };
				if (!ParseVec4 (&data, stretch, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Material_ValidateTcModArgs (state, "tcMod stretch", stretch, stretch_defaults, 4);
				if (!Material_PushTcMod (&stage, MAT_TCMOD_STRETCH, stretch, 4))
				{
					Material_WarnMaterial (state, "tcMod limit exceeded; ignoring extra modifiers");
					stage.tcmod_overflow = true;
					if (r_particles_material_strict.value > 0.f)
						valid = false;
				}
				continue;
			}
			Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
				state ? state->source_file : material->source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "animMap"))
		{
			Material_MarkKeywordSeen ("animMap", MATERIAL_KEYWORD_SCOPE_STAGE);
			float fps = 0.f;
			float validated_fps = 0.f;
			int frames = 0;
			qboolean limit_hit = false;
			const char *cursor = data;

			if (!ParseFloat (&cursor, &fps, state))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}

			Material_ValidateFloatMin (state, "animMap fps", fps, 0.f, 0.f, &validated_fps);

			while (1)
			{
				const char *next = Material_ParseToken (cursor, state);
				if (!next || !com_token[0])
					break;
				if (state && state->token_limit_hit)
				{
					/* Token limit hit mid-animMap frame list. Simply mark the stage
					   invalid and break; the outer ParseStageBlock loop will handle
					   cleanup. Do NOT call SkipUnknownBlockOrLine with already_open=true
					   here â€” there is no extra '{' open, so that would consume the
					   stage block's own closing '}' and corrupt the parse position. */
					valid = false;
					goto stage_done;
				}
				if (!strcmp (com_token, "{") || !strcmp (com_token, "}"))
					break;
				if (frames < MATERIAL_MAX_ANIM_FRAMES)
				{
					VEC_PUSH (stage.anim_map_frames, Material_DupString (com_token));
					frames++;
				}
				else if (!limit_hit)
				{
					Material_WarnMaterial (state, "animMap frame limit exceeded; ignoring remaining frames");
					limit_hit = true;
				}
				cursor = next;
			}

			if (frames > 0)
			{
				stage.anim_map_fps = validated_fps;
				data = cursor;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive"))
		{
			Material_MarkKeywordSeen ("emissive", MATERIAL_KEYWORD_SCOPE_STAGE);
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			if (!parsed)
				value_bool = true;

			stage.output_overrides |= MAT_STAGE_OUT_EMISSIVE;
			if (value_bool)
				stage.outputs |= MAT_STAGE_OUT_EMISSIVE;
			else
				stage.outputs &= ~MAT_STAGE_OUT_EMISSIVE;
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom"))
		{
			Material_MarkKeywordSeen ("bloom", MATERIAL_KEYWORD_SCOPE_STAGE);
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			if (!parsed)
				value_bool = true;

			stage.output_overrides |= MAT_STAGE_OUT_BLOOM;
			if (value_bool)
				stage.outputs |= MAT_STAGE_OUT_BLOOM;
			else
				stage.outputs &= ~MAT_STAGE_OUT_BLOOM;
			continue;
		}
		{
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			if (!parsed)
				value_bool = true;

			if (value_bool)
			else
			continue;
		}
		if (!q_strcasecmp (com_token, "emissiveScale"))
		{
			Material_MarkKeywordSeen ("emissiveScale", MATERIAL_KEYWORD_SCOPE_STAGE);
			float validated_scale = 1.f;
			float scale;

			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}

			Material_ValidateFiniteFloat (state, "emissiveScale", scale, 1.f, &validated_scale);
			stage.emissive_scale = CLAMP (0.f, validated_scale, MATERIAL_SCALE_MAX);
			stage.emissive_scale_set = true;
			continue;
		}
		if (!q_strcasecmp (com_token, "bloomScale"))
		{
			Material_MarkKeywordSeen ("bloomScale", MATERIAL_KEYWORD_SCOPE_STAGE);
			float validated_scale = 1.f;
			float scale;

			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}

			Material_ValidateFiniteFloat (state, "bloomScale", scale, 1.f, &validated_scale);
			stage.bloom_scale = CLAMP (0.f, validated_scale, MATERIAL_SCALE_MAX);
			stage.bloom_scale_set = true;
			continue;
		}
		{
			float validated_scale = 1.f;
			float scale;

			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}

			continue;
		}

		Material_ReportUnknownToken (com_token, MATERIAL_KEYWORD_SCOPE_STAGE, material->name,
			state ? state->source_file : material->source_file,
			state ? state->token_line : 0u);
		data = SkipUnknownBlockOrLine (data, false, state);
		if (!data)
		{
			valid = false;
			break;
		}
	}

stage_done:
	if (state && state->token_limit_hit)
	{
		if (stage_valid)
			*stage_valid = false;
		return data;
	}

	if (stage_valid)
		*stage_valid = valid;

	if (!valid)
		return data;

	if (stage.blend_mode != MAT_BLEND_REPLACE || !stage.depth_write || stage.depth_func != MAT_DEPTHFUNC_LEQUAL)
		material->render_flags |= MAT_RENDER_TRANS;

	VEC_PUSH (material->stages, stage);

	if (stage_index == 0)
		material->stage0 = stage;

	return data;
}

static const char *ParseMaterialBlock (const char *data, const char *name, const char *source_file, mat_material_parse_state_t *state)
{
	material_t material;
	const material_t *existing;
	size_t stage_index = 0;
	char canonical[MAX_QPATH];

	memset (&material, 0, sizeof (material));
	if (state)
		state->source_file = source_file;
	Material_Canonicalize (name, canonical, sizeof (canonical));
	material.name = Material_DupString (canonical);
	material.source_file = Material_DupString (source_file ? source_file : "");
	material.emissive_scale = 1.f;
	material.bloom_scale = 1.f;
	material.cull_mode = MAT_CULL_BACK;
	material.sort_key = MAT_SORT_OPAQUE;
	material.polygon_offset = false;
	material.polygon_offset_factor = 0.f;
	material.polygon_offset_units = 1.f;

	while ((data = Material_ParseToken (data, state)) != NULL)
	{
		const char *value;
		float scale;

		if (!com_token[0])
			continue;
		if (state && state->token_limit_hit)
		{
			data = SkipUnknownBlockOrLine (data, true, NULL);
			break;
		}
		if (!strcmp (com_token, "}"))
			break;
		if (!strcmp (com_token, "{"))
		{
			if (stage_index >= MATERIAL_MAX_STAGES)
			{
				Material_WarnMaterial (state, "stage limit exceeded; skipping remaining stages");
				data = SkipUnknownBlockOrLine (data, true, NULL);
				break;
			}
			{
				qboolean stage_valid = true;

				data = ParseStageBlock (data, &material, stage_index, state, &stage_valid);
				if (stage_valid)
					stage_index++;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "qer_editorimage"))
		{
			Material_MarkKeywordSeen ("qer_editorimage", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "qer_editorimage path"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			material.editor_image = Material_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "surfaceparm"))
		{
			Material_MarkKeywordSeen ("surfaceparm", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "surfaceparm value"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			Material_ApplySurfaceParm (&material, value, state);
			continue;
		}
		if (!q_strcasecmp (com_token, "cull"))
		{
			Material_MarkKeywordSeen ("cull", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "cull mode"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (Material_ParseCullMode (value, &material.cull_mode))
				continue;
			Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_TOPLEVEL, material.name,
				state ? state->source_file : material.source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "sort"))
		{
			Material_MarkKeywordSeen ("sort", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "sort key"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (Material_ParseSortKey (value, &material.sort_key))
			{
				if (material.sort_key != MAT_SORT_OPAQUE)
					material.render_flags |= MAT_RENDER_TRANS;
				continue;
			}
			Material_ReportUnknownToken (value, MATERIAL_KEYWORD_SCOPE_TOPLEVEL, material.name,
				state ? state->source_file : material.source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "polygonOffset"))
		{
			Material_MarkKeywordSeen ("polygonOffset", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			qboolean value_bool = true;
			float factor = material.polygon_offset_factor;
			float units = material.polygon_offset_units;
			const char *cursor = Material_ParseToken (data, NULL);

			if (!cursor || !com_token[0] || Material_IsBraceToken (com_token))
			{
				material.polygon_offset = true;
				continue;
			}

			if (!Material_IsNumericToken (com_token) && Material_IsBoolToken (com_token))
			{
				if (!ParseRequiredBool (&data, &value_bool, state))
				{
					data = ResyncMaterialBlock (data, state);
					break;
				}
				material.polygon_offset = value_bool;
				if (!value_bool)
					continue;

				cursor = Material_ParseToken (data, NULL);
				if (cursor && com_token[0] && Material_IsNumericToken (com_token))
				{
					const char *next = Material_ParseToken (cursor, NULL);
					if (next && com_token[0] && Material_IsNumericToken (com_token))
					{
						if (!ParseFloat (&data, &factor, state) || !ParseFloat (&data, &units, state))
						{
							data = ResyncMaterialBlock (data, state);
							break;
						}
						material.polygon_offset_factor = factor;
						material.polygon_offset_units = units;
					}
				}
				continue;
			}

			if (Material_IsNumericToken (com_token))
			{
				const char *next = Material_ParseToken (cursor, NULL);
				if (next && com_token[0] && Material_IsNumericToken (com_token))
				{
					if (!ParseFloat (&data, &factor, state) || !ParseFloat (&data, &units, state))
					{
						data = ResyncMaterialBlock (data, state);
						break;
					}
					material.polygon_offset = true;
					material.polygon_offset_factor = factor;
					material.polygon_offset_units = units;
					continue;
				}

				if (!ParseRequiredBool (&data, &value_bool, state))
				{
					data = ResyncMaterialBlock (data, state);
					break;
				}
				material.polygon_offset = value_bool;
				continue;
			}

			material.polygon_offset = true;
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive"))
		{
			Material_MarkKeywordSeen ("emissive", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseRequiredBool (&data, &material.emissive_enable, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom"))
		{
			Material_MarkKeywordSeen ("bloom", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			const char *cursor = Material_ParseToken (data, NULL);
			if (cursor && com_token[0] && Material_IsNumericToken (com_token))
			{
				float validated_scale = 1.f;
				if (!ParseFloat (&data, &scale, state))
				{
					data = ResyncMaterialBlock (data, state);
					break;
				}
				Material_ValidateFiniteFloat (state, "bloom", scale, 1.f, &validated_scale);
				material.bloom_scale = CLAMP (0.f, validated_scale, MATERIAL_SCALE_MAX);
				material.bloom_enable = (material.bloom_scale > 0.f);
				continue;
			}
			if (!ParseRequiredBool (&data, &material.bloom_enable, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (!material.bloom_enable)
				material.bloom_scale = 0.f;
			continue;
		}
		{
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive_scale"))
		{
			Material_MarkKeywordSeen ("emissive_scale", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			float validated_scale = 1.f;
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			Material_ValidateFiniteFloat (state, "emissive_scale", scale, 1.f, &validated_scale);
			material.emissive_scale = CLAMP (0.f, validated_scale, MATERIAL_SCALE_MAX);
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom_scale"))
		{
			Material_MarkKeywordSeen ("bloom_scale", MATERIAL_KEYWORD_SCOPE_TOPLEVEL);
			float validated_scale = 1.f;
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			Material_ValidateFiniteFloat (state, "bloom_scale", scale, 1.f, &validated_scale);
			material.bloom_scale = CLAMP (0.f, validated_scale, MATERIAL_SCALE_MAX);
			continue;
		}
		{
			float validated_scale = 1.f;
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}
		Material_ReportUnknownToken (com_token, MATERIAL_KEYWORD_SCOPE_TOPLEVEL, material.name,
			state ? state->source_file : material.source_file,
			state ? state->token_line : 0u);
		data = SkipUnknownBlockOrLine (data, false, state);
		if (!data)
			break;
	}

	if (material.stages)
	{
		size_t stage_count = VEC_SIZE (material.stages);
		qboolean has_emissive = false;
		qboolean has_bloom = false;

		for (size_t i = 0; i < stage_count; ++i)
		{
			material_stage_t *stage = &material.stages[i];

			stage->outputs |= MAT_STAGE_OUT_COLOR;
			if ((stage->output_overrides & MAT_STAGE_OUT_EMISSIVE) == 0u && material.emissive_enable)
				stage->outputs |= MAT_STAGE_OUT_EMISSIVE;
			if ((stage->output_overrides & MAT_STAGE_OUT_BLOOM) == 0u && material.bloom_enable)
				stage->outputs |= MAT_STAGE_OUT_BLOOM;

			if (!stage->emissive_scale_set)
				stage->emissive_scale = material.emissive_scale;
			if (!stage->bloom_scale_set)
				stage->bloom_scale = material.bloom_scale;

			if (stage->outputs & MAT_STAGE_OUT_EMISSIVE)
				has_emissive = true;
			if (stage->outputs & MAT_STAGE_OUT_BLOOM)
				has_bloom = true;
		}

		material.emissive_enable = has_emissive;
		material.bloom_enable = has_bloom;
		material.stage0 = material.stages[0];
	}

	existing = Material_Find (material.name);
	if (existing)
		Material_Remove (existing);
	Material_Insert (&material);

	return data;
}

int Material_ParseFile (const char *path, const char *data, const char *source_file)
{
	int parsed = 0;
	const char *cursor = data;
	mat_material_parse_state_t file_state;
	mat_material_parse_state_t state;

	(void) path;

	if (!cursor)
		return 0;

	memset (&file_state, 0, sizeof (file_state));
	file_state.line = 1;
	file_state.source_file = source_file;
	file_state.track_tokens = false;

	while ((cursor = Material_ParseToken (cursor, &file_state)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!q_strcasecmp (com_token, "decal") || !q_strcasecmp (com_token, "decaldef"))
		{
			const char *ignored_name = NULL;

			if (!ParseIdent (&cursor, &ignored_name, &file_state))
				continue;
			if (!ExpectToken (&cursor, "{", &file_state))
				continue;
			cursor = SkipUnknownBlockOrLine (cursor, true, &file_state);
			continue;
		}
		if (!strcmp (com_token, "{"))
		{
			cursor = SkipUnknownBlockOrLine (cursor, true, &file_state);
			continue;
		}
		{
			char name[MAX_QPATH];

			q_strlcpy (name, com_token, sizeof (name));
			memset (&state, 0, sizeof (state));
			state.line = file_state.line;
			state.source_file = file_state.source_file;
			state.track_tokens = true;
			state.material_name = name;
			if (!ExpectToken (&cursor, "{", &state))
				continue;
			cursor = ParseMaterialBlock (cursor, name, source_file, &state);
			file_state.line = state.line;
			parsed++;
		}
	}

	return parsed;
}

static char *Material_BuildLongPathShader (void)
{
	const char *prefix = "fuzz_longpath\n{\n { map textures/";
	const char *suffix = " }\n}\n";
	const size_t path_len = MAX_QPATH * 4;
	const size_t total = strlen (prefix) + path_len + strlen (suffix) + 1;
	char *buffer = (char *) q_malloc(total);
	size_t offset = 0;

	if (!buffer)
		return NULL;

	offset += (size_t) q_snprintf (buffer + offset, total - offset, "%s", prefix);
	memset (buffer + offset, 'a', path_len);
	offset += path_len;
	q_snprintf (buffer + offset, total - offset, "%s", suffix);
	return buffer;
}

static char *Material_BuildManyStagesShader (void)
{
	const char *prefix = "fuzz_many_stages\n{\n";
	const char *stage = " { map $whiteimage }\n";
	const char *suffix = "}\n";
	const size_t stage_count = MATERIAL_MAX_STAGES + 2;
	const size_t total = strlen (prefix) + (stage_count * strlen (stage)) + strlen (suffix) + 1;
	char *buffer = (char *) q_malloc(total);
	size_t offset = 0;

	if (!buffer)
		return NULL;

	offset += (size_t) q_snprintf (buffer + offset, total - offset, "%s", prefix);
	for (size_t i = 0; i < stage_count; ++i)
		offset += (size_t) q_snprintf (buffer + offset, total - offset, "%s", stage);
	q_snprintf (buffer + offset, total - offset, "%s", suffix);
	return buffer;
}

static void Material_RemoveFuzzMaterial (const char *name)
{
	const material_t *material = Material_Find (name);

	if (!material || !material->source_file)
		return;
	if (q_strncasecmp (material->source_file, "matshader_fuzz", 14))
		return;

	Material_Remove (material);
}

void Material_DebugFuzzParse (void)
{
	const char *fuzz_missing_brace =
		"fuzz_missing_brace\n"
		"{\n"
		" surfaceparm solid\n"
		" { map textures/test }\n";
	const char *fuzz_unknown =
		"fuzz_unknown\n"
		"{\n"
		" unknown_key 1\n"
		" unknown_block { nested 1 }\n"
		" { map textures/test }\n"
		"}\n";
	const char *fuzz_nonfinite =
		"fuzz_nonfinite\n"
		"{\n"
		" emissive_scale nan\n"
		" bloom_scale inf\n"
		" { map $whiteimage }\n"
		"}\n";
	const char *fuzz_duplicate =
		"fuzz_duplicate\n"
		"{\n"
		" surfaceparm solid\n"
		" surfaceparm solid\n"
		" cull none\n"
		" cull back\n"
		" emissive_scale 2\n"
		" emissive_scale 3\n"
		" { map $whiteimage }\n"
		"}\n";
	char *fuzz_longpath = Material_BuildLongPathShader ();
	char *fuzz_many_stages = Material_BuildManyStagesShader ();
	const size_t warnings_before = Material_ParseGetWarnings ();
	const size_t errors_before = Material_ParseGetErrors ();
	const char *names[] =
	{
		"fuzz_missing_brace",
		"fuzz_unknown",
		"fuzz_longpath",
		"fuzz_many_stages",
		"fuzz_nonfinite",
		"fuzz_duplicate"
	};
	struct
	{
		const char *label;
		const char *data;
	} cases[] =
	{
		{ "missing_brace", fuzz_missing_brace },
		{ "unknown_keywords", fuzz_unknown },
		{ "long_path", fuzz_longpath },
		{ "many_stages", fuzz_many_stages },
		{ "nonfinite_scales", fuzz_nonfinite },
		{ "duplicate_keys", fuzz_duplicate }
	};

	Con_Printf ("Material fuzz parse begin\n");

	for (size_t i = 0; i < countof (cases); ++i)
	{
		char source_label[64];
		int parsed;

		if (!cases[i].data)
		{
			Con_Warning ("Material fuzz '%s' skipped (allocation failed)\n", cases[i].label);
			continue;
		}

		q_snprintf (source_label, sizeof (source_label), "matshader_fuzz:%s", cases[i].label);
		parsed = Material_ParseFile ("matshader_fuzz", cases[i].data, source_label);
		Con_Printf ("Material fuzz '%s' parsed %d material(s)\n", cases[i].label, parsed);
	}

	for (size_t i = 0; i < countof (names); ++i)
		Material_RemoveFuzzMaterial (names[i]);

	Con_Printf ("Material fuzz end (+%zu warnings, +%zu errors)\n",
		Material_ParseGetWarnings () - warnings_before,
		Material_ParseGetErrors () - errors_before);

	q_free(fuzz_longpath);
	q_free(fuzz_many_stages);
}
