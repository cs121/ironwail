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
	{ "stone", MAT_SURFPARM_STONE, 0u, 0u }
};

typedef struct
{
	unsigned int token_count;
	qboolean token_limit_hit;
	const char *material_name;
} mat_shader_parse_state_t;

static void Mat_Shader_WarnMaterial (const mat_shader_parse_state_t *state, const char *message)
{
	if (state && state->material_name)
		Con_Warning ("Material '%s': %s\n", state->material_name, message);
	else
		Con_Warning ("%s\n", message);
}

static void Mat_Shader_WarnExpectedToken (const mat_shader_parse_state_t *state, const char *expected, const char *got)
{
	const char *got_token = (got && got[0]) ? got : "<eof>";

	if (state && state->material_name)
		Con_Warning ("Material '%s': expected %s, got '%s'\n", state->material_name, expected, got_token);
	else
		Con_Warning ("Expected %s, got '%s'\n", expected, got_token);
}

static const char *Mat_Shader_ParseToken (const char *data, mat_shader_parse_state_t *state)
{
	const char *cursor = COM_Parse (data);

	if (!cursor)
		return NULL;

	if (state && !state->token_limit_hit)
	{
		state->token_count++;
		if (state->token_count > MAT_SHADER_MAX_TOKENS)
		{
			state->token_limit_hit = true;
			Mat_Shader_WarnMaterial (state, "token limit exceeded; skipping remaining tokens");
		}
	}

	return cursor;
}

static qboolean Mat_Shader_ParseBool (const char *token, qboolean default_value)
{
	if (!token || !token[0])
		return default_value;
	if (!q_strcasecmp (token, "true") || !q_strcasecmp (token, "yes") || !q_strcasecmp (token, "on"))
		return true;
	if (!q_strcasecmp (token, "false") || !q_strcasecmp (token, "no") || !q_strcasecmp (token, "off"))
		return false;
	return Q_atof (token) != 0.f;
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

	*out = Mat_Shader_ParseBool (com_token, false);
	*data = cursor;
	return true;
}

static qboolean ParseIdent (const char **data, const char **out, mat_shader_parse_state_t *state)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = Mat_Shader_ParseToken (*data, state);
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
	*out = Mat_Shader_ParseBool (token, false);
	return true;
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
	if (!q_strcasecmp (token, "none"))
	{
		*out = MAT_CULL_NONE;
		return true;
	}
	return false;
}

static qboolean Mat_Shader_ParseSortKey (const char *token, mat_sort_key_t *out)
{
	if (!token || !out)
		return false;
	if (!q_strcasecmp (token, "opaque"))
	{
		*out = MAT_SORT_OPAQUE;
		return true;
	}
	if (!q_strcasecmp (token, "decal"))
	{
		*out = MAT_SORT_DECAL;
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
	if (!q_strcasecmp (token, "equal"))
	{
		*out = MAT_DEPTHFUNC_EQUAL;
		return true;
	}
	if (!q_strcasecmp (token, "always"))
	{
		*out = MAT_DEPTHFUNC_ALWAYS;
		return true;
	}
	return false;
}

static void Mat_Shader_ApplySurfaceParm (shader_material_t *material, const char *token)
{
	for (size_t i = 0; i < countof (mat_surfaceparm_table); ++i)
	{
		if (!q_strcasecmp (token, mat_surfaceparm_table[i].name))
		{
			material->surfaceparms |= mat_surfaceparm_table[i].surfaceparm;
			material->render_flags |= mat_surfaceparm_table[i].render_flags;
			material->content_flags |= mat_surfaceparm_table[i].content_flags;
			return;
		}
	}

	Mat_Shader_ReportUnknownToken (token, material->name);
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
			if (!COM_ParseLine (&cursor, NULL))
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
	stage.rgbgen = MAT_RGBGEN_DEFAULT;
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
			if (!ParseIdentExpected (&data, &value, state, "rgbGen mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!q_strcasecmp (value, "identity"))
				stage.rgbgen = MAT_RGBGEN_IDENTITY;
			continue;
		}
		if (!q_strcasecmp (com_token, "blendFunc"))
		{
			int src;
			int dst;

			if (!ParseIdentExpected (&data, &value, state, "blendFunc mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}

			if (Mat_Shader_ParseBlendMode (value, &stage.blend_mode))
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

			if (Mat_Shader_ParseBlendFactor (value, &src))
			{
				if (!ParseIdentExpected (&data, &value, state, "blendFunc dst"))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				if (!Mat_Shader_ParseBlendFactor (value, &dst))
				{
					Mat_Shader_ReportUnknownToken (value, material->name);
					break;
				}
				stage.blend_mode = MAT_BLEND_CUSTOM;
				stage.blend_src = src;
				stage.blend_dst = dst;
				continue;
			}

			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "depthWrite"))
		{
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			stage.depth_write = parsed ? value_bool : true;
			continue;
		}
		if (!q_strcasecmp (com_token, "depthFunc"))
		{
			if (!ParseIdentExpected (&data, &value, state, "depthFunc mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (Mat_Shader_ParseDepthFunc (value, &stage.depth_func))
				continue;
			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcGen"))
		{
			if (!ParseIdentExpected (&data, &value, state, "tcGen mode"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!Mat_Shader_ParseTcGen (value, &stage.tcgen))
				Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcMod"))
		{
			if (!ParseIdentExpected (&data, &value, state, "tcMod type"))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}
			if (!q_strcasecmp (value, "scroll"))
			{
				vec2_t scroll = { 0.f, 0.f };
				if (!ParseVec2 (&data, scroll, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_SCROLL, scroll, 2);
				continue;
			}
			if (!q_strcasecmp (value, "scale"))
			{
				vec2_t scale = { 1.f, 1.f };
				if (!ParseVec2 (&data, scale, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_SCALE, scale, 2);
				continue;
			}
			if (!q_strcasecmp (value, "rotate"))
			{
				float deg_per_sec = 0.f;
				if (!ParseFloat (&data, &deg_per_sec, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_ROTATE, &deg_per_sec, 1);
				continue;
			}
			if (!q_strcasecmp (value, "turb"))
			{
				vec4_t turb = { 0.f, 0.f, 0.f, 0.f };
				if (!ParseVec4 (&data, turb, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_TURB, turb, 4);
				continue;
			}
			if (!q_strcasecmp (value, "stretch"))
			{
				vec4_t stretch = { 0.f, 0.f, 0.f, 0.f };
				if (!ParseVec4 (&data, stretch, state))
				{
					valid = false;
					data = SkipUnknownBlockOrLine (data, true, state);
					break;
				}
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_STRETCH, stretch, 4);
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "animMap"))
		{
			float fps = 0.f;
			int frames = 0;
			qboolean limit_hit = false;
			const char *cursor = data;

			if (!ParseFloat (&cursor, &fps, state))
			{
				valid = false;
				data = SkipUnknownBlockOrLine (data, true, state);
				break;
			}

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
				stage.anim_map_fps = fps;
				data = cursor;
			}
			continue;
		}

		Mat_Shader_ReportUnknownToken (com_token, material->name);
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
	Mat_Shader_Canonicalize (name, canonical, sizeof (canonical));
	material.name = Mat_Shader_DupString (canonical);
	material.source_file = Mat_Shader_DupString (source_file ? source_file : "");
	material.emissive_scale = 1.f;
	material.bloom_scale = 1.f;
	material.godray_scale = 1.f;
	material.cull_mode = MAT_CULL_BACK;
	material.sort_key = MAT_SORT_OPAQUE;
	material.polygon_offset = false;

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
			if (!ParseIdentExpected (&data, &value, state, "surfaceparm value"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			Mat_Shader_ApplySurfaceParm (&material, value);
			continue;
		}
		if (!q_strcasecmp (com_token, "cull"))
		{
			if (!ParseIdentExpected (&data, &value, state, "cull mode"))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			if (Mat_Shader_ParseCullMode (value, &material.cull_mode))
				continue;
			Mat_Shader_ReportUnknownToken (value, material.name);
			continue;
		}
		if (!q_strcasecmp (com_token, "sort"))
		{
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
			Mat_Shader_ReportUnknownToken (value, material.name);
			continue;
		}
		if (!q_strcasecmp (com_token, "polygonOffset"))
		{
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool, state);
			material.polygon_offset = parsed ? value_bool : true;
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive"))
		{
			if (!ParseRequiredBool (&data, &material.emissive_enable, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom"))
		{
			if (!ParseRequiredBool (&data, &material.bloom_enable, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "godray"))
		{
			if (!ParseRequiredBool (&data, &material.godray_enable, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive_scale"))
		{
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			material.emissive_scale = scale;
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom_scale"))
		{
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			material.bloom_scale = scale;
			continue;
		}
		if (!q_strcasecmp (com_token, "godray_scale"))
		{
			if (!ParseFloat (&data, &scale, state))
			{
				data = ResyncMaterialBlock (data, state);
				break;
			}
			material.godray_scale = scale;
			continue;
		}
		Mat_Shader_ReportUnknownToken (com_token, material.name);
		data = SkipUnknownBlockOrLine (data, false, state);
		if (!data)
			break;
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
	mat_shader_parse_state_t state;

	(void) path;

	if (!cursor)
		return 0;

	while ((cursor = COM_Parse (cursor)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
		{
			cursor = SkipUnknownBlockOrLine (cursor, true, NULL);
			continue;
		}
		{
			char name[MAX_QPATH];

			q_strlcpy (name, com_token, sizeof (name));
			memset (&state, 0, sizeof (state));
			state.material_name = name;
			if (!ExpectToken (&cursor, "{", &state))
				continue;
			cursor = ParseMaterialBlock (cursor, name, source_file, &state);
			parsed++;
		}
	}

	return parsed;
}
