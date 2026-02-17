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
	{ "alphashadow", MAT_SURFPARM_ALPHASHADOW, MAT_RENDER_ALPHASHADOW, 0u },
	{ "sky", MAT_SURFPARM_SKY, MAT_RENDER_SKY, 0u },
	{ "fog", MAT_SURFPARM_FOG, MAT_RENDER_FOG, 0u },
	{ "nodraw", MAT_SURFPARM_NODRAW, MAT_RENDER_NODRAW, 0u },
	{ "water", MAT_SURFPARM_WATER, MAT_RENDER_TRANS, 0u },
	{ "slime", MAT_SURFPARM_SLIME, MAT_RENDER_TRANS, 0u },
	{ "lava", MAT_SURFPARM_LAVA, MAT_RENDER_TRANS, 0u },
	{ "noimpact", MAT_SURFPARM_NOIMPACT, 0u, 0u },
	{ "nomarks", MAT_SURFPARM_NOMARKS, 0u, 0u },
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
} mat_shader_parse_state_t;

#define MAT_SHADER_SCALE_MAX 64.f
#define MAT_SHADER_TCMOD_MAX_ABS 64.f
#define MAX_TOKEN_CHARS Q_COUNTOF(com_token)

static size_t mat_shader_parse_warnings;
static size_t mat_shader_parse_errors;

void Mat_Shader_ParseResetStats (void)
{
	mat_shader_parse_warnings = 0;
	mat_shader_parse_errors = 0;
}

void Mat_Shader_ParseAddWarning (void)
{
	mat_shader_parse_warnings++;
}

void Mat_Shader_ParseAddError (void)
{
	mat_shader_parse_errors++;
}

size_t Mat_Shader_ParseGetWarnings (void)
{
	return mat_shader_parse_warnings;
}

size_t Mat_Shader_ParseGetErrors (void)
{
	return mat_shader_parse_errors;
}

static void Mat_Shader_WarnMaterial (const mat_shader_parse_state_t *state, const char *message)
{
	Mat_Shader_ParseAddWarning ();
	if (state && state->material_name)
		Con_Warning ("Material '%s': %s\n", state->material_name, message);
	else
		Con_Warning ("%s\n", message);
}

static void Mat_Shader_WarnExpectedToken (const mat_shader_parse_state_t *state, const char *expected, const char *got)
{
	const char *got_token = (got && got[0]) ? got : "<eof>";

	Mat_Shader_ParseAddError ();
	if (state && state->material_name)
		Con_Warning ("Material '%s': expected %s, got '%s'\n", state->material_name, expected, got_token);
	else
		Con_Warning ("Expected %s, got '%s'\n", expected, got_token);
}

static void Mat_Shader_TrackTokenLine (const char *data, mat_shader_parse_state_t *state, const char **token_start)
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

static const char *Mat_Shader_ParseToken (const char *data, mat_shader_parse_state_t *state)
{
	const char *token_start = data;
	const char *cursor;

	if (state)
		Mat_Shader_TrackTokenLine (data, state, &token_start);

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
			if (state->token_count > MAT_SHADER_MAX_TOKENS)
			{
				state->token_limit_hit = true;
				Mat_Shader_WarnMaterial (state, "token limit exceeded; skipping remaining tokens");
			}
		}
	}

	return cursor;
}

static qboolean Mat_Shader_ParseBool (const char *token, qboolean default_value, const mat_shader_parse_state_t *state)
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
		Mat_Shader_WarnMaterial (state, message);
		return default_value;
	}
	return value != 0.f;
}

static qboolean Mat_Shader_IsBraceToken (const char *token)
{
	if (!token || !token[0])
		return false;
	return !strcmp (token, "{") || !strcmp (token, "}");
}

static qboolean Mat_Shader_IsNumericToken (const char *token)
{
	size_t i;

	if (!token || !token[0])
		return false;

	for (i = 0; token[i]; ++i)
	{
		if ((token[i] >= '0' && token[i] <= '9') || token[i] == '+' || token[i] == '-' || token[i] == '.' || token[i] == 'e' || token[i] == 'E')
			continue;
		return false;
	}

	return true;
}

static qboolean Mat_Shader_IsBoolToken (const char *token)
{
	if (!token || !token[0])
		return false;
	if (!q_strcasecmp (token, "true") || !q_strcasecmp (token, "false") ||
		!q_strcasecmp (token, "yes") || !q_strcasecmp (token, "no") ||
		!q_strcasecmp (token, "on") || !q_strcasecmp (token, "off"))
		return true;
	return Mat_Shader_IsNumericToken (token);
}

static qboolean Mat_Shader_ParseLine (const char **data, mat_shader_parse_state_t *state)
{
	const char *cursor;
	stringview_t line;

	if (!data || !*data)
		return false;

	cursor = *data;
	if (!COM_ParseLine (data, &line))
		return false;

	if (state)
	{
		const char *line_end = cursor + line.len;
		if (*line_end == '\n')
			state->line++;
	}

	return true;
}

static qboolean ParseOptionalBool (const char **data, qboolean *out, mat_shader_parse_state_t *state)
{
	const char *cursor;

	if (!data || !*data || !out)
		return false;

	cursor = Mat_Shader_ParseToken (*data, state);
	if (!cursor || !com_token[0])
		return false;

	if (state && state->token_limit_hit)
		return false;

	if (!Mat_Shader_IsBoolToken (com_token))
		return false;

	*out = Mat_Shader_ParseBool (com_token, false, state);
	*data = cursor;
	return true;
}

static qboolean ParseIdentExpected (const char **data, const char **out, mat_shader_parse_state_t *state, const char *expected)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = Mat_Shader_ParseToken (*data, state);
	if (!cursor)
	{
		Mat_Shader_WarnExpectedToken (state, expected, "<eof>");
		return false;
	}

	*data = cursor;
	if (state && state->token_limit_hit)
		return false;
	if (!com_token[0] || Mat_Shader_IsBraceToken (com_token))
	{
		Mat_Shader_WarnExpectedToken (state, expected, com_token);
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

	memcpy (buffer, start, len);
	buffer[len] = '\0';
	return buffer;
}

static qboolean ParseFloat (const char **data, float *out, mat_shader_parse_state_t *state)
{
	const char *token;

	if (!ParseIdentExpected (data, &token, state, "float"))
		return false;

	if (!Mat_Shader_IsNumericToken (token))
	{
		Mat_Shader_WarnExpectedToken (state, "float", token);
		return false;
	}

	*out = Q_atof (token);
	return true;
}

static qboolean ParseVec2 (const char **data, vec2_t out, mat_shader_parse_state_t *state)
{
	if (!ParseFloat (data, &out[0], state))
		return false;
	if (!ParseFloat (data, &out[1], state))
		return false;
	return true;
}

static qboolean ParseVec3 (const char **data, vec3_t out, mat_shader_parse_state_t *state)
{
	if (!ParseFloat (data, &out[0], state))
		return false;
	if (!ParseFloat (data, &out[1], state))
		return false;
	if (!ParseFloat (data, &out[2], state))
		return false;
	return true;
}

static qboolean ParseVec4 (const char **data, vec4_t out, mat_shader_parse_state_t *state)
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

static qboolean ParseRequiredBool (const char **data, qboolean *out, mat_shader_parse_state_t *state)
{
	const char *token;

	if (!ParseIdentExpected (data, &token, state, "boolean"))
		return false;
	if (!Mat_Shader_IsBoolToken (token))
	{
		Mat_Shader_WarnExpectedToken (state, "boolean", token);
		return false;
	}
	*out = Mat_Shader_ParseBool (token, false, state);
	return true;
}

static qboolean Mat_Shader_ValidateFiniteFloat (const mat_shader_parse_state_t *state, const char *label, float value, float default_value, float *out)
{
	char message[128];

	if (isfinite (value))
	{
		*out = value;
		return true;
	}

	q_snprintf (message, sizeof (message), "%s is not finite; using default", label);
	Mat_Shader_WarnMaterial (state, message);
	*out = default_value;
	return false;
}

static qboolean Mat_Shader_ValidateFloatMin (const mat_shader_parse_state_t *state, const char *label, float value, float min_value, float default_value, float *out)
{
	char message[128];

	if (!isfinite (value))
	{
		q_snprintf (message, sizeof (message), "%s is not finite; using default", label);
		Mat_Shader_WarnMaterial (state, message);
		*out = default_value;
		return false;
	}
	if (value < min_value)
	{
		q_snprintf (message, sizeof (message), "%s below %.2f; using default", label, min_value);
		Mat_Shader_WarnMaterial (state, message);
		*out = default_value;
		return false;
	}

	*out = value;
	return true;
}

static void Mat_Shader_ValidateTcModArgs (const mat_shader_parse_state_t *state, const char *label, float *values, const float *defaults, int count)
{
	char message[128];

	for (int i = 0; i < count; ++i)
	{
		if (!isfinite (values[i]))
		{
			q_snprintf (message, sizeof (message), "%s has non-finite values; using defaults", label);
			Mat_Shader_WarnMaterial (state, message);
			for (int j = 0; j < count; ++j)
				values[j] = defaults[j];
			return;
		}
	}

	for (int i = 0; i < count; ++i)
		values[i] = CLAMP (-MAT_SHADER_TCMOD_MAX_ABS, values[i], MAT_SHADER_TCMOD_MAX_ABS);
}

static qboolean Mat_Shader_ParseTcGen (const char *token, mat_tcgen_t *out)
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
	if (!q_strcasecmp (token, "vector"))
	{
		*out = MAT_TCGEN_VECTOR;
		return true;
	}
	return false;
}

static void Mat_Shader_PushTcMod (mat_shader_stage_t *stage, mat_tcmod_type_t type, const float *args, int arg_count)
{
	mat_tcmod_t mod;
	int i;

	if (!stage || stage->tcmod_count >= (int)countof (stage->tcmods))
		return;

	memset (&mod, 0, sizeof (mod));
	mod.type = type;
	for (i = 0; i < arg_count && i < (int)countof (mod.args); ++i)
		mod.args[i] = args[i];

	stage->tcmods[stage->tcmod_count++] = mod;
}

static qboolean ExpectToken (const char **data, const char *token, mat_shader_parse_state_t *state)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = Mat_Shader_ParseToken (*data, state);
	if (!cursor)
	{
		Mat_Shader_WarnExpectedToken (state, token, "<eof>");
		return false;
	}

	*data = cursor;
	if (state && state->token_limit_hit)
		return false;
	if (strcmp (com_token, token))
	{
		Mat_Shader_WarnExpectedToken (state, token, com_token);
		return false;
	}
	return true;
}

static qboolean Mat_Shader_ParseCullMode (const char *token, mat_cull_mode_t *out)
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

static qboolean Mat_Shader_ParseSortKey (const char *token, mat_sort_key_t *out)
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

static qboolean Mat_Shader_ParseBlendFactor (const char *token, int *out)
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

static qboolean Mat_Shader_ParseBlendMode (const char *token, mat_blend_mode_t *out)
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

static qboolean Mat_Shader_ParseDepthFunc (const char *token, mat_depthfunc_t *out)
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

static void Mat_Shader_ApplySurfaceParm (shader_material_t *material, const char *token, const mat_shader_parse_state_t *state)
{
	for (size_t i = 0; i < countof (mat_surfaceparm_table); ++i)
	{
		if (!q_strcasecmp (token, mat_surfaceparm_table[i].name))
		{
			Mat_Shader_MarkKeywordSeen (mat_surfaceparm_table[i].name, MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM);
			material->surfaceparms |= mat_surfaceparm_table[i].surfaceparm;
			material->render_flags |= mat_surfaceparm_table[i].render_flags;
			material->content_flags |= mat_surfaceparm_table[i].content_flags;
			return;
		}
	}

	Mat_Shader_ReportUnknownToken (token, MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM, material->name,
		state ? state->source_file : material->source_file,
		state ? state->token_line : 0u);
}

static const char *SkipUnknownBlockOrLine (const char *data, qboolean already_open, mat_shader_parse_state_t *state)
{
	int depth = 1;
	const char *cursor = data;

	if (!cursor)
		return NULL;

	if (!already_open)
	{
		cursor = Mat_Shader_ParseToken (cursor, state);
		if (!cursor)
			return NULL;
		if (!com_token[0])
			return cursor;
		if (!strcmp (com_token, "{"))
			depth = 1;
		else
		{
			if (!Mat_Shader_ParseLine (&cursor, state))
				return NULL;
			return cursor;
		}
	}

	while ((cursor = Mat_Shader_ParseToken (cursor, state)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
		{
			depth++;
			if (depth > MAT_SHADER_MAX_BRACE_DEPTH)
			{
				Mat_Shader_WarnMaterial (state, "brace depth limit exceeded while skipping unknown block");
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

static const char *ResyncMaterialBlock (const char *data, mat_shader_parse_state_t *state)
{
	int depth = 1;
	const char *cursor = data;

	while ((cursor = Mat_Shader_ParseToken (cursor, state)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
		{
			depth++;
			if (depth > MAT_SHADER_MAX_BRACE_DEPTH)
			{
				Mat_Shader_WarnMaterial (state, "brace depth limit exceeded while resyncing material");
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

static const char *ParseStageBlock (const char *data, shader_material_t *material, size_t stage_index, mat_shader_parse_state_t *state, qboolean *stage_valid)
{
	mat_shader_stage_t stage;
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
	stage.alpha_func = MAT_ALPHAFUNC_NONE;
	VectorClear (stage.tcgen_vec0);
	VectorClear (stage.tcgen_vec1);
	stage.tcgen_is_vector = false;
	stage.anim_map_fps = 0.f;
	stage.anim_map_frame = 0;
	stage.texmatrix_time_bucket = -1;
	stage.anim_map_time_bucket = -1;

	while ((data = Mat_Shader_ParseToken (data, state)) != NULL)
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
			Mat_Shader_MarkKeywordSeen ("map", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "map path"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
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
				stage.map_path = Mat_Shader_DupString (value);
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "clampmap"))
		{
			Mat_Shader_MarkKeywordSeen ("clampmap", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "map path"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			stage.map_type = MAT_MAP_CLAMPMAP;
			stage.map_path = Mat_Shader_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "rgbGen"))
		{
			Mat_Shader_MarkKeywordSeen ("rgbGen", MAT_SHADER_KEYWORD_SCOPE_STAGE);
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
					if (!Mat_Shader_IsNumericToken (token))
					{
						Mat_Shader_WarnExpectedToken (state, "float", token);
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
					Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
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

				Mat_Shader_ValidateFiniteFloat (state, "rgbGen wave base", base, 0.f, &stage.rgb_wave.base);
				Mat_Shader_ValidateFiniteFloat (state, "rgbGen wave amp", amp, 0.f, &stage.rgb_wave.amp);
				Mat_Shader_ValidateFiniteFloat (state, "rgbGen wave phase", phase, 0.f, &stage.rgb_wave.phase);
				Mat_Shader_ValidateFiniteFloat (state, "rgbGen wave freq", freq, 0.f, &stage.rgb_wave.freq);
				stage.rgb_wave.type = wave_type;
				stage.rgbgen = MAT_RGBGEN_WAVE;
			}
			else
			{
				Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "alphaGen"))
		{
			Mat_Shader_MarkKeywordSeen ("alphaGen", MAT_SHADER_KEYWORD_SCOPE_STAGE);
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
					if (!Mat_Shader_IsNumericToken (token))
					{
						Mat_Shader_WarnExpectedToken (state, "float", token);
						valid = false;
						data = SkipUnknownBlockOrLine (data, true, state);
						break;
					}
					alpha = Q_atof (token);
				}

				stage.alphagen = MAT_ALPHAGEN_CONST;
				stage.const_alpha = alpha;
			}
			else if (!q_strcasecmp (value, "lightingSpecular"))
			{
				stage.alphagen = MAT_ALPHAGEN_LIGHTINGSPECULAR;
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
					Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
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

				Mat_Shader_ValidateFiniteFloat (state, "alphaGen wave base", base, 0.f, &stage.alpha_wave.base);
				Mat_Shader_ValidateFiniteFloat (state, "alphaGen wave amp", amp, 0.f, &stage.alpha_wave.amp);
				Mat_Shader_ValidateFiniteFloat (state, "alphaGen wave phase", phase, 0.f, &stage.alpha_wave.phase);
				Mat_Shader_ValidateFiniteFloat (state, "alphaGen wave freq", freq, 0.f, &stage.alpha_wave.freq);
				stage.alpha_wave.type = wave_type;
				stage.alphagen = MAT_ALPHAGEN_WAVE;
			}
			else
			{
				Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "blendFunc"))
		{
			Mat_Shader_MarkKeywordSeen ("blendFunc", MAT_SHADER_KEYWORD_SCOPE_STAGE);
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
					&& Mat_Shader_ParseBlendFactor (src_token, &src)
					&& Mat_Shader_ParseBlendFactor (dst_token, &dst))
				{
					stage.blend_mode = MAT_BLEND_CUSTOM;
					stage.blend_src = src;
					stage.blend_dst = dst;
					continue;
				}
				Mat_Shader_ReportUnknownToken (mode_token, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
				continue;
			}

			if (Mat_Shader_ParseBlendMode (mode_token, &stage.blend_mode))
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

			if (Mat_Shader_ParseBlendFactor (mode_token, &src))
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
				if (!Mat_Shader_ParseBlendFactor (dst_token, &dst))
				{
					Mat_Shader_ReportUnknownToken (dst_token, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
						state ? state->source_file : material->source_file,
						state ? state->token_line : 0u);
					break;
				}
				stage.blend_mode = MAT_BLEND_CUSTOM;
				stage.blend_src = src;
				stage.blend_dst = dst;
				continue;
			}

			Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
				state ? state->source_file : material->source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "depthWrite"))
		{
			Mat_Shader_MarkKeywordSeen ("depthWrite", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			stage.depth_write = parsed ? value_bool : true;
			continue;
		}
		if (!q_strcasecmp (com_token, "depthFunc"))
		{
			Mat_Shader_MarkKeywordSeen ("depthFunc", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "depthFunc mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (Mat_Shader_ParseDepthFunc (value, &stage.depth_func))
				continue;
			stage.depth_func = MAT_DEPTHFUNC_LEQUAL;
			Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
				state ? state->source_file : material->source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "alphaFunc"))
		{
			Mat_Shader_MarkKeywordSeen ("alphaFunc", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "alphaFunc mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!q_strcasecmp (value, "GT0"))
				stage.alpha_func = MAT_ALPHAFUNC_GT0;
			else if (!q_strcasecmp (value, "LT128"))
				stage.alpha_func = MAT_ALPHAFUNC_LT128;
			else if (!q_strcasecmp (value, "GE128"))
				stage.alpha_func = MAT_ALPHAFUNC_GE128;
			else if (!q_strcasecmp (value, "none"))
				stage.alpha_func = MAT_ALPHAFUNC_NONE;
			else
				Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcGen"))
		{
			Mat_Shader_MarkKeywordSeen ("tcGen", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			if (!ParseIdentExpected (&data, &value, state, "tcGen mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!Mat_Shader_ParseTcGen (value, &stage.tcgen))
			{
				Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
					state ? state->source_file : material->source_file,
					state ? state->token_line : 0u);
				continue;
			}
			if (stage.tcgen == MAT_TCGEN_VECTOR)
			{
				if (!ExpectToken (&data, "(", state)
					|| !ParseVec3 (&data, stage.tcgen_vec0, state)
					|| !ExpectToken (&data, ")", state)
					|| !ExpectToken (&data, "(", state)
					|| !ParseVec3 (&data, stage.tcgen_vec1, state)
					|| !ExpectToken (&data, ")", state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				stage.tcgen_is_vector = true;
			}
			else
			{
				stage.tcgen_is_vector = false;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "tcMod"))
		{
			Mat_Shader_MarkKeywordSeen ("tcMod", MAT_SHADER_KEYWORD_SCOPE_STAGE);
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
				Mat_Shader_ValidateTcModArgs (state, "tcMod scroll", scroll, scroll_defaults, 2);
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_SCROLL, scroll, 2);
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
				Mat_Shader_ValidateTcModArgs (state, "tcMod scale", scale, scale_defaults, 2);
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_SCALE, scale, 2);
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
				Mat_Shader_ValidateTcModArgs (state, "tcMod rotate", &deg_per_sec, &rotate_default, 1);
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_ROTATE, &deg_per_sec, 1);
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
				Mat_Shader_ValidateTcModArgs (state, "tcMod turb", turb, turb_defaults, 4);
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_TURB, turb, 4);
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
				Mat_Shader_ValidateTcModArgs (state, "tcMod stretch", stretch, stretch_defaults, 4);
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_STRETCH, stretch, 4);
				continue;
			}
			if (!q_strcasecmp (value, "transform"))
			{
				float transform[6] = { 1.f, 0.f, 0.f, 1.f, 0.f, 0.f };
				const float transform_defaults[6] = { 1.f, 0.f, 0.f, 1.f, 0.f, 0.f };
				if (!ParseFloat (&data, &transform[0], state)
					|| !ParseFloat (&data, &transform[1], state)
					|| !ParseFloat (&data, &transform[2], state)
					|| !ParseFloat (&data, &transform[3], state)
					|| !ParseFloat (&data, &transform[4], state)
					|| !ParseFloat (&data, &transform[5], state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Mat_Shader_ValidateTcModArgs (state, "tcMod transform", transform, transform_defaults, 6);
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_TRANSFORM, transform, 6);
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
				state ? state->source_file : material->source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "animMap"))
		{
			Mat_Shader_MarkKeywordSeen ("animMap", MAT_SHADER_KEYWORD_SCOPE_STAGE);
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

			Mat_Shader_ValidateFloatMin (state, "animMap fps", fps, 0.f, 0.f, &validated_fps);

			while (1)
			{
				const char *next = Mat_Shader_ParseToken (cursor, state);
				if (!next || !com_token[0])
					break;
				if (state && state->token_limit_hit)
				{
					valid = false;
					data = SkipUnknownBlockOrLine (next, true, NULL);
					goto stage_done;
				}
				if (!strcmp (com_token, "{") || !strcmp (com_token, "}"))
					break;
				if (frames < MAT_SHADER_MAX_ANIM_FRAMES)
				{
					VEC_PUSH (stage.anim_map_frames, Mat_Shader_DupString (com_token));
					frames++;
				}
				else if (!limit_hit)
				{
					Mat_Shader_WarnMaterial (state, "animMap frame limit exceeded; ignoring remaining frames");
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
			Mat_Shader_MarkKeywordSeen ("emissive", MAT_SHADER_KEYWORD_SCOPE_STAGE);
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

		if (!q_strcasecmp (com_token, "emissiveScale"))
		{
			Mat_Shader_MarkKeywordSeen ("emissiveScale", MAT_SHADER_KEYWORD_SCOPE_STAGE);
			float validated_scale = 1.f;
			float scale;

			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}

			Mat_Shader_ValidateFiniteFloat (state, "emissiveScale", scale, 1.f, &validated_scale);
			stage.emissive_scale = CLAMP (0.f, validated_scale, MAT_SHADER_SCALE_MAX);
			stage.emissive_scale_set = true;
			continue;
		}


		Mat_Shader_ReportUnknownToken (com_token, MAT_SHADER_KEYWORD_SCOPE_STAGE, material->name,
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

	if (!material->stages)
		material->stages = NULL;
	VEC_PUSH (material->stages, stage);

	if (stage_index == 0)
		material->stage0 = stage;

	return data;
}

static const char *ParseMaterialBlock (const char *data, const char *name, const char *source_file, mat_shader_parse_state_t *state)
{
	shader_material_t material;
	const shader_material_t *existing;
	size_t stage_index = 0;
	char canonical[MAX_QPATH];

	memset (&material, 0, sizeof (material));
	if (state)
		state->source_file = source_file;
	Mat_Shader_Canonicalize (name, canonical, sizeof (canonical));
	material.name = Mat_Shader_DupString (canonical);
	material.source_file = Mat_Shader_DupString (source_file ? source_file : "");
	material.emissive_scale = 1.f;
	material.cull_mode = MAT_CULL_BACK;
	material.sort_key = MAT_SORT_OPAQUE;
	material.polygon_offset = false;
	material.polygon_offset_factor = 0.f;
	material.polygon_offset_units = 1.f;

	while ((data = Mat_Shader_ParseToken (data, state)) != NULL)
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
			if (stage_index >= MAT_SHADER_MAX_STAGES)
			{
				Mat_Shader_WarnMaterial (state, "stage limit exceeded; skipping remaining stages");
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
			Mat_Shader_MarkKeywordSeen ("qer_editorimage", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "qer_editorimage path"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			material.editor_image = Mat_Shader_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "surfaceparm"))
		{
			Mat_Shader_MarkKeywordSeen ("surfaceparm", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "surfaceparm value"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			Mat_Shader_ApplySurfaceParm (&material, value, state);
			continue;
		}
		if (!q_strcasecmp (com_token, "skyparms") || !q_strcasecmp (com_token, "skyParms"))
		{
			const char *farbox;
			const char *cloudheight_token;
			const char *nearbox;
			float cloudheight = 0.f;

			Mat_Shader_MarkKeywordSeen ("skyParms", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &farbox, state, "skyParms farbox")
				|| !ParseIdentExpected (&data, &cloudheight_token, state, "skyParms cloudheight")
				|| !ParseIdentExpected (&data, &nearbox, state, "skyParms nearbox"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}

			if (Mat_Shader_IsNumericToken (cloudheight_token))
				cloudheight = Q_atof (cloudheight_token);
			else if (q_strcasecmp (cloudheight_token, "-"))
			{
				Mat_Shader_WarnExpectedToken (state, "skyParms cloudheight float or '-'", cloudheight_token);
				data = ResyncMaterialBlock (data, state);
				break;
			}

			if (material.skybox_far)
				Z_Free (material.skybox_far);
			if (material.skybox_near)
				Z_Free (material.skybox_near);

			material.skybox_far = (q_strcasecmp (farbox, "-") == 0) ? NULL : Mat_Shader_DupString (farbox);
			material.skybox_near = (q_strcasecmp (nearbox, "-") == 0) ? NULL : Mat_Shader_DupString (nearbox);
			material.sky_cloudheight = q_max (0.f, cloudheight);
			material.has_skyparms = true;
			material.render_flags |= MAT_RENDER_SKY;
			continue;
		}
		if (!q_strcasecmp (com_token, "fogparms") || !q_strcasecmp (com_token, "fogParms"))
		{
			float r,g,b,dist;
			Mat_Shader_MarkKeywordSeen ("fogParms", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ExpectToken (&data, "(", state) || !ParseFloat (&data, &r, state) || !ParseFloat (&data, &g, state) || !ParseFloat (&data, &b, state) || !ExpectToken (&data, ")", state) || !ParseFloat (&data, &dist, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			material.has_fogparms = true;
			material.fog_color[0] = CLAMP(0.f, r, 1.f);
			material.fog_color[1] = CLAMP(0.f, g, 1.f);
			material.fog_color[2] = CLAMP(0.f, b, 1.f);
			material.fog_distance = q_max(0.f, dist);
			material.render_flags |= MAT_RENDER_FOG;
			continue;
		}
		if (!q_strcasecmp (com_token, "deformVertexes"))
		{
			const char *deform_type;
			mat_deform_t deform;
			memset(&deform, 0, sizeof(deform));
			Mat_Shader_MarkKeywordSeen ("deformVertexes", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &deform_type, state, "deformVertexes mode"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (!q_strcasecmp(deform_type, "wave"))
			{
				const char *wt; float spread, base, amp, phase, freq;
				deform.type = MAT_DEFORM_WAVE;
				if (!ParseFloat(&data, &spread, state) || !ParseIdentExpected(&data, &wt, state, "deform wave type") || !ParseFloat(&data,&base,state) || !ParseFloat(&data,&amp,state) || !ParseFloat(&data,&phase,state) || !ParseFloat(&data,&freq,state)) { data = ResyncMaterialBlock(data,state); break; }
				deform.args[0] = spread;
				if (!ParseWaveType (wt, &deform.wave.type)) deform.wave.type = MAT_WAVE_SIN;
				deform.wave.base=base; deform.wave.amp=amp; deform.wave.phase=phase; deform.wave.freq=freq;
			}
			else if (!q_strcasecmp(deform_type, "move"))
			{
				const char *wt; float base, amp, phase, freq;
				deform.type = MAT_DEFORM_MOVE;
				if (!ParseFloat(&data, &deform.move[0], state) || !ParseFloat(&data, &deform.move[1], state) || !ParseFloat(&data, &deform.move[2], state) || !ParseIdentExpected(&data, &wt, state, "deform move wave") || !ParseFloat(&data,&base,state) || !ParseFloat(&data,&amp,state) || !ParseFloat(&data,&phase,state) || !ParseFloat(&data,&freq,state)) { data = ResyncMaterialBlock(data,state); break; }
				if (!ParseWaveType (wt, &deform.wave.type)) deform.wave.type = MAT_WAVE_SIN;
				deform.wave.base=base; deform.wave.amp=amp; deform.wave.phase=phase; deform.wave.freq=freq;
			}
			else if (!q_strcasecmp(deform_type, "bulge"))
			{
				deform.type = MAT_DEFORM_BULGE;
				if (!ParseFloat(&data, &deform.args[0], state) || !ParseFloat(&data, &deform.args[1], state) || !ParseFloat(&data, &deform.args[2], state)) { data = ResyncMaterialBlock(data,state); break; }
			}
			else if (!q_strcasecmp(deform_type, "autosprite"))
			{
				deform.type = MAT_DEFORM_AUTOSPRITE;
			}
			else
			{
				Mat_Shader_ReportUnknownToken (deform_type, MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, material.name, state ? state->source_file : material.source_file, state ? state->token_line : 0u);
				continue;
			}
			VEC_PUSH(material.deforms, deform);
			continue;
		}
		if (!q_strcasecmp (com_token, "cull"))
		{
			Mat_Shader_MarkKeywordSeen ("cull", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "cull mode"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (Mat_Shader_ParseCullMode (value, &material.cull_mode))
				continue;
			Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, material.name,
				state ? state->source_file : material.source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "sort"))
		{
			Mat_Shader_MarkKeywordSeen ("sort", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseIdentExpected (&data, &value, state, "sort key"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (Mat_Shader_ParseSortKey (value, &material.sort_key))
			{
				if (material.sort_key != MAT_SORT_OPAQUE)
					material.render_flags |= MAT_RENDER_TRANS;
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, material.name,
				state ? state->source_file : material.source_file,
				state ? state->token_line : 0u);
			continue;
		}
		if (!q_strcasecmp (com_token, "polygonOffset"))
		{
			Mat_Shader_MarkKeywordSeen ("polygonOffset", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			qboolean value_bool = true;
			float factor = material.polygon_offset_factor;
			float units = material.polygon_offset_units;
			const char *cursor = Mat_Shader_ParseToken (data, NULL);

			if (!cursor || !com_token[0] || Mat_Shader_IsBraceToken (com_token))
			{
				material.polygon_offset = true;
				continue;
			}

			if (!Mat_Shader_IsNumericToken (com_token) && Mat_Shader_IsBoolToken (com_token))
			{
				if (!ParseRequiredBool (&data, &value_bool, state))
				{
					data = ResyncMaterialBlock (data, state);
					break;
				}
				material.polygon_offset = value_bool;
				if (!value_bool)
					continue;

				cursor = Mat_Shader_ParseToken (data, NULL);
				if (cursor && com_token[0] && Mat_Shader_IsNumericToken (com_token))
				{
					const char *next = Mat_Shader_ParseToken (cursor, NULL);
					if (next && com_token[0] && Mat_Shader_IsNumericToken (com_token))
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

			if (Mat_Shader_IsNumericToken (com_token))
			{
				const char *next = Mat_Shader_ParseToken (cursor, NULL);
				if (next && com_token[0] && Mat_Shader_IsNumericToken (com_token))
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
			Mat_Shader_MarkKeywordSeen ("emissive", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			if (!ParseRequiredBool (&data, &material.emissive_enable, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}

		if (!q_strcasecmp (com_token, "emissive_scale"))
		{
			Mat_Shader_MarkKeywordSeen ("emissive_scale", MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL);
			float validated_scale = 1.f;
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			Mat_Shader_ValidateFiniteFloat (state, "emissive_scale", scale, 1.f, &validated_scale);
			material.emissive_scale = CLAMP (0.f, validated_scale, MAT_SHADER_SCALE_MAX);
			continue;
		}

		Mat_Shader_ReportUnknownToken (com_token, MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL, material.name,
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

		for (size_t i = 0; i < stage_count; ++i)
		{
			mat_shader_stage_t *stage = &material.stages[i];

			stage->outputs |= MAT_STAGE_OUT_COLOR;
			if ((stage->output_overrides & MAT_STAGE_OUT_EMISSIVE) == 0u && material.emissive_enable)
				stage->outputs |= MAT_STAGE_OUT_EMISSIVE;

			if (!stage->emissive_scale_set)
				stage->emissive_scale = material.emissive_scale;

			if (stage->outputs & MAT_STAGE_OUT_EMISSIVE)
				has_emissive = true;
		}

		material.emissive_enable = has_emissive;
		material.stage0 = material.stages[0];
	}

	existing = Mat_Shader_Find (material.name);
	if (existing)
		Mat_Shader_Remove (existing);
	Mat_Shader_Insert (&material);

	return data;
}

int Mat_Shader_ParseFile (const char *path, const char *data, const char *source_file)
{
	int parsed = 0;
	const char *cursor = data;
	mat_shader_parse_state_t file_state;
	mat_shader_parse_state_t state;

	(void) path;

	if (!cursor)
		return 0;

	memset (&file_state, 0, sizeof (file_state));
	file_state.line = 1;
	file_state.source_file = source_file;
	file_state.track_tokens = false;

	while ((cursor = Mat_Shader_ParseToken (cursor, &file_state)) != NULL)
	{
		if (!q_strcasecmp (com_token, "decal"))
		{
			const char *decl_name;

			if (!ParseIdentExpected (&cursor, &decl_name, NULL, "decal name"))
				continue;
			if (!ExpectToken (&cursor, "{", NULL))
				continue;
			cursor = SkipUnknownBlockOrLine (cursor, true, &file_state);
			continue;
		}

		if (!com_token[0])
			continue;
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

static char *Mat_Shader_BuildLongPathShader (void)
{
	const char *prefix = "fuzz_longpath\n{\n { map textures/";
	const char *suffix = " }\n}\n";
	const size_t path_len = MAX_QPATH * 4;
	const size_t total = strlen (prefix) + path_len + strlen (suffix) + 1;
	char *buffer = (char *) malloc (total);
	size_t offset = 0;

	if (!buffer)
		return NULL;

	offset += (size_t) q_snprintf (buffer + offset, total - offset, "%s", prefix);
	memset (buffer + offset, 'a', path_len);
	offset += path_len;
	q_snprintf (buffer + offset, total - offset, "%s", suffix);
	return buffer;
}

static char *Mat_Shader_BuildManyStagesShader (void)
{
	const char *prefix = "fuzz_many_stages\n{\n";
	const char *stage = " { map $whiteimage }\n";
	const char *suffix = "}\n";
	const size_t stage_count = MAT_SHADER_MAX_STAGES + 2;
	const size_t total = strlen (prefix) + (stage_count * strlen (stage)) + strlen (suffix) + 1;
	char *buffer = (char *) malloc (total);
	size_t offset = 0;

	if (!buffer)
		return NULL;

	offset += (size_t) q_snprintf (buffer + offset, total - offset, "%s", prefix);
	for (size_t i = 0; i < stage_count; ++i)
		offset += (size_t) q_snprintf (buffer + offset, total - offset, "%s", stage);
	q_snprintf (buffer + offset, total - offset, "%s", suffix);
	return buffer;
}

static void Mat_Shader_RemoveFuzzMaterial (const char *name)
{
	const shader_material_t *material = Mat_Shader_Find (name);

	if (!material || !material->source_file)
		return;
	if (q_strncasecmp (material->source_file, "matshader_fuzz", 14))
		return;

	Mat_Shader_Remove (material);
}

void Mat_Shader_DebugFuzzParse (void)
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
	char *fuzz_longpath = Mat_Shader_BuildLongPathShader ();
	char *fuzz_many_stages = Mat_Shader_BuildManyStagesShader ();
	const size_t warnings_before = Mat_Shader_ParseGetWarnings ();
	const size_t errors_before = Mat_Shader_ParseGetErrors ();
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

	Con_Printf ("Material shader fuzz parse begin\n");

	for (size_t i = 0; i < countof (cases); ++i)
	{
		char source_label[64];
		int parsed;

		if (!cases[i].data)
		{
			Con_Warning ("Material shader fuzz '%s' skipped (allocation failed)\n", cases[i].label);
			continue;
		}

		q_snprintf (source_label, sizeof (source_label), "matshader_fuzz:%s", cases[i].label);
		parsed = Mat_Shader_ParseFile ("matshader_fuzz", cases[i].data, source_label);
		Con_Printf ("Material shader fuzz '%s' parsed %d material(s)\n", cases[i].label, parsed);
	}

	for (size_t i = 0; i < countof (names); ++i)
		Mat_Shader_RemoveFuzzMaterial (names[i]);

	Con_Printf ("Material shader fuzz end (+%zu warnings, +%zu errors)\n",
		Mat_Shader_ParseGetWarnings () - warnings_before,
		Mat_Shader_ParseGetErrors () - errors_before);

	free (fuzz_longpath);
	free (fuzz_many_stages);
}
