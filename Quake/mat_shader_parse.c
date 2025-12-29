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

static qboolean ParseOptionalBool (const char **data, qboolean *out)
{
	const char *cursor;

	if (!data || !*data || !out)
		return false;

	cursor = COM_Parse (*data);
	if (!cursor || !com_token[0])
		return false;

	if (!Mat_Shader_IsBoolToken (com_token))
		return false;

	*out = Mat_Shader_ParseBool (com_token, false);
	*data = cursor;
	return true;
}

static qboolean ParseIdent (const char **data, const char **out)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = COM_Parse (*data);
	if (!cursor)
		return false;

	*data = cursor;
	if (!com_token[0])
		return false;

	*out = com_token;
	return true;
}

static qboolean ParseFloat (const char **data, float *out)
{
	const char *token;

	if (!ParseIdent (data, &token))
		return false;

	*out = Q_atof (token);
	return true;
}

static qboolean ParseVec2 (const char **data, vec2_t out)
{
	if (!ParseFloat (data, &out[0]))
		return false;
	if (!ParseFloat (data, &out[1]))
		return false;
	return true;
}

static qboolean ParseVec3 (const char **data, vec3_t out)
{
	if (!ParseFloat (data, &out[0]))
		return false;
	if (!ParseFloat (data, &out[1]))
		return false;
	if (!ParseFloat (data, &out[2]))
		return false;
	return true;
}

static qboolean ParseVec4 (const char **data, vec4_t out)
{
	if (!ParseFloat (data, &out[0]))
		return false;
	if (!ParseFloat (data, &out[1]))
		return false;
	if (!ParseFloat (data, &out[2]))
		return false;
	if (!ParseFloat (data, &out[3]))
		return false;
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

static qboolean Mat_Shader_ParseWaveType (const char *token, mat_wave_type_t *out)
{
	if (!token || !out)
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

static qboolean ExpectToken (const char **data, const char *token)
{
	const char *cursor;

	if (!data || !*data)
		return false;

	cursor = COM_Parse (*data);
	if (!cursor)
		return false;

	*data = cursor;
	return !strcmp (com_token, token);
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

static qboolean Mat_Shader_IsLightmapFilterStage (const mat_shader_stage_t *stage)
{
	if (!stage)
		return false;
	return stage->map_type == MAT_MAP_LIGHTMAP && stage->blend_mode == MAT_BLEND_MULT;
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

static const char *SkipUnknownBlockOrLine (const char *data, qboolean already_open)
{
	int depth = 1;
	const char *cursor = data;

	if (!cursor)
		return NULL;

	if (!already_open)
	{
		cursor = COM_Parse (cursor);
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

	while ((cursor = COM_Parse (cursor)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
			depth++;
		else if (!strcmp (com_token, "}"))
		{
			depth--;
			if (depth <= 0)
				break;
		}
	}
	return cursor;
}

static const char *ParseStageBlock (const char *data, shader_material_t *material, size_t stage_index)
{
	mat_shader_stage_t stage;

	memset (&stage, 0, sizeof (stage));
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
	stage.output_flags = MAT_STAGE_OUTPUT_COLOR;
	stage.emissive_scale = material->emissive_scale;
	stage.bloom_scale = material->bloom_scale;
	stage.godray_scale = material->godray_scale;

	while ((data = COM_Parse (data)) != NULL)
	{
		const char *value;
		float scale;

		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "}"))
			break;
		if (!q_strcasecmp (com_token, "map"))
		{
			if (!ParseIdent (&data, &value))
				break;
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
			if (!ParseIdent (&data, &value))
				break;
			stage.map_type = MAT_MAP_CLAMPMAP;
			stage.map_path = Mat_Shader_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "rgbGen"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (!q_strcasecmp (value, "identity"))
			{
				stage.rgbgen = MAT_RGBGEN_IDENTITY;
				continue;
			}
			if (!q_strcasecmp (value, "vertex"))
			{
				stage.rgbgen = MAT_RGBGEN_VERTEX;
				continue;
			}
			if (!q_strcasecmp (value, "const"))
			{
				const char *cursor = data;
				const char *token = NULL;
				vec3_t color = { 1.f, 1.f, 1.f };
				if (!ParseIdent (&cursor, &token))
					break;
				if (!strcmp (token, "("))
				{
					if (!ParseVec3 (&cursor, color))
						break;
					if (!ExpectToken (&cursor, ")"))
						break;
				}
				else
				{
					color[0] = Q_atof (token);
					if (!ParseFloat (&cursor, &color[1]))
						break;
					if (!ParseFloat (&cursor, &color[2]))
						break;
				}

				stage.rgbgen = MAT_RGBGEN_CONST;
				stage.const_color[0] = color[0];
				stage.const_color[1] = color[1];
				stage.const_color[2] = color[2];
				data = cursor;
				continue;
			}
			if (!q_strcasecmp (value, "wave"))
			{
				mat_wave_t wave;
				const char *wave_type = NULL;

				memset (&wave, 0, sizeof (wave));
				if (!ParseIdent (&data, &wave_type))
					break;
				if (!Mat_Shader_ParseWaveType (wave_type, &wave.type))
				{
					Mat_Shader_ReportUnknownToken (wave_type, material->name);
					continue;
				}
				if (!ParseFloat (&data, &wave.base))
					break;
				if (!ParseFloat (&data, &wave.amp))
					break;
				if (!ParseFloat (&data, &wave.phase))
					break;
				if (!ParseFloat (&data, &wave.freq))
					break;
				stage.rgbgen = MAT_RGBGEN_WAVE;
				stage.rgb_wave = wave;
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "alphaGen"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (!q_strcasecmp (value, "identity"))
			{
				stage.alphagen = MAT_ALPHAGEN_IDENTITY;
				continue;
			}
			if (!q_strcasecmp (value, "vertex"))
			{
				stage.alphagen = MAT_ALPHAGEN_VERTEX;
				continue;
			}
			if (!q_strcasecmp (value, "const"))
			{
				const char *cursor = data;
				const char *token = NULL;
				float alpha = 1.f;

				if (!ParseIdent (&cursor, &token))
					break;
				if (!strcmp (token, "("))
				{
					if (!ParseFloat (&cursor, &alpha))
						break;
					if (!ExpectToken (&cursor, ")"))
						break;
				}
				else
				{
					alpha = Q_atof (token);
				}

				stage.alphagen = MAT_ALPHAGEN_CONST;
				stage.const_alpha = alpha;
				data = cursor;
				continue;
			}
			if (!q_strcasecmp (value, "wave"))
			{
				mat_wave_t wave;
				const char *wave_type = NULL;

				memset (&wave, 0, sizeof (wave));
				if (!ParseIdent (&data, &wave_type))
					break;
				if (!Mat_Shader_ParseWaveType (wave_type, &wave.type))
				{
					Mat_Shader_ReportUnknownToken (wave_type, material->name);
					continue;
				}
				if (!ParseFloat (&data, &wave.base))
					break;
				if (!ParseFloat (&data, &wave.amp))
					break;
				if (!ParseFloat (&data, &wave.phase))
					break;
				if (!ParseFloat (&data, &wave.freq))
					break;
				stage.alphagen = MAT_ALPHAGEN_WAVE;
				stage.alpha_wave = wave;
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "blendFunc"))
		{
			int src;
			int dst;

			if (!ParseIdent (&data, &value))
				break;

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
				if (!ParseIdent (&data, &value))
					break;
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

			parsed = ParseOptionalBool (&data, &value_bool);
			stage.depth_write = parsed ? value_bool : true;
			continue;
		}
		if (!q_strcasecmp (com_token, "depthFunc"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (Mat_Shader_ParseDepthFunc (value, &stage.depth_func))
				continue;
			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcGen"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (!Mat_Shader_ParseTcGen (value, &stage.tcgen))
				Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "tcMod"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (!q_strcasecmp (value, "scroll"))
			{
				vec2_t scroll = { 0.f, 0.f };
				if (!ParseVec2 (&data, scroll))
					break;
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_SCROLL, scroll, 2);
				continue;
			}
			if (!q_strcasecmp (value, "scale"))
			{
				vec2_t scale = { 1.f, 1.f };
				if (!ParseVec2 (&data, scale))
					break;
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_SCALE, scale, 2);
				continue;
			}
			if (!q_strcasecmp (value, "rotate"))
			{
				float deg_per_sec = 0.f;
				if (!ParseFloat (&data, &deg_per_sec))
					break;
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_ROTATE, &deg_per_sec, 1);
				continue;
			}
			if (!q_strcasecmp (value, "turb"))
			{
				vec4_t turb = { 0.f, 0.f, 0.f, 0.f };
				if (!ParseVec4 (&data, turb))
					break;
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_TURB, turb, 4);
				continue;
			}
			if (!q_strcasecmp (value, "stretch"))
			{
				vec4_t stretch = { 0.f, 0.f, 0.f, 0.f };
				if (!ParseVec4 (&data, stretch))
					break;
				Mat_Shader_PushTcMod (&stage, MAT_TCMOD_STRETCH, stretch, 4);
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, material->name);
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive"))
		{
			if (!ParseIdent (&data, &value))
				break;
			stage.output_override_flags |= MAT_STAGE_OUTPUT_EMISSIVE;
			if (Mat_Shader_ParseBool (value, false))
				stage.output_flags |= MAT_STAGE_OUTPUT_EMISSIVE;
			else
				stage.output_flags &= ~MAT_STAGE_OUTPUT_EMISSIVE;
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom"))
		{
			if (!ParseIdent (&data, &value))
				break;
			stage.output_override_flags |= MAT_STAGE_OUTPUT_BLOOM;
			if (Mat_Shader_ParseBool (value, false))
				stage.output_flags |= MAT_STAGE_OUTPUT_BLOOM;
			else
				stage.output_flags &= ~MAT_STAGE_OUTPUT_BLOOM;
			continue;
		}
		if (!q_strcasecmp (com_token, "godray"))
		{
			if (!ParseIdent (&data, &value))
				break;
			stage.output_override_flags |= MAT_STAGE_OUTPUT_GODRAY_SOURCE;
			if (Mat_Shader_ParseBool (value, false))
				stage.output_flags |= MAT_STAGE_OUTPUT_GODRAY_SOURCE;
			else
				stage.output_flags &= ~MAT_STAGE_OUTPUT_GODRAY_SOURCE;
			continue;
		}
		if (!q_strcasecmp (com_token, "emissiveScale") || !q_strcasecmp (com_token, "emissive_scale"))
		{
			if (!ParseFloat (&data, &scale))
				break;
			stage.emissive_scale = scale;
			stage.scale_override_flags |= MAT_STAGE_OUTPUT_EMISSIVE;
			continue;
		}
		if (!q_strcasecmp (com_token, "bloomScale") || !q_strcasecmp (com_token, "bloom_scale"))
		{
			if (!ParseFloat (&data, &scale))
				break;
			stage.bloom_scale = scale;
			stage.scale_override_flags |= MAT_STAGE_OUTPUT_BLOOM;
			continue;
		}
		if (!q_strcasecmp (com_token, "godrayScale") || !q_strcasecmp (com_token, "godray_scale"))
		{
			if (!ParseFloat (&data, &scale))
				break;
			stage.godray_scale = scale;
			stage.scale_override_flags |= MAT_STAGE_OUTPUT_GODRAY_SOURCE;
			continue;
		}
		if (!q_strcasecmp (com_token, "animMap"))
		{
			float fps = 0.f;
			int frames = 0;
			const char *cursor = data;

			if (!ParseFloat (&cursor, &fps))
				break;

			while (1)
			{
				const char *next = COM_Parse (cursor);
				if (!next || !com_token[0])
					break;
				if (!strcmp (com_token, "{") || !strcmp (com_token, "}"))
					break;
				VEC_PUSH (stage.anim_map_frames, Mat_Shader_DupString (com_token));
				frames++;
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
		data = SkipUnknownBlockOrLine (data, false);
		if (!data)
			break;
	}

	if (!Mat_Shader_IsLightmapFilterStage (&stage) &&
		(stage.blend_mode != MAT_BLEND_REPLACE || !stage.depth_write || stage.depth_func != MAT_DEPTHFUNC_LEQUAL))
		material->render_flags |= MAT_RENDER_TRANS;

	if (!material->stages)
		material->stages = NULL;
	VEC_PUSH (material->stages, stage);

	if (stage_index == 0)
		material->stage0 = stage;

	return data;
}

static void Mat_Shader_ApplyStageDefaults (shader_material_t *material)
{
	size_t i;

	if (!material)
		return;

	for (i = 0; i < VEC_SIZE (material->stages); ++i)
	{
		mat_shader_stage_t *stage = &material->stages[i];

		stage->output_flags |= MAT_STAGE_OUTPUT_COLOR;

		if ((stage->output_override_flags & MAT_STAGE_OUTPUT_EMISSIVE) == 0u)
		{
			if (material->emissive_enable)
				stage->output_flags |= MAT_STAGE_OUTPUT_EMISSIVE;
			else
				stage->output_flags &= ~MAT_STAGE_OUTPUT_EMISSIVE;
		}
		if ((stage->output_override_flags & MAT_STAGE_OUTPUT_BLOOM) == 0u)
		{
			if (material->bloom_enable)
				stage->output_flags |= MAT_STAGE_OUTPUT_BLOOM;
			else
				stage->output_flags &= ~MAT_STAGE_OUTPUT_BLOOM;
		}
		if ((stage->output_override_flags & MAT_STAGE_OUTPUT_GODRAY_SOURCE) == 0u)
		{
			if (material->godray_enable)
				stage->output_flags |= MAT_STAGE_OUTPUT_GODRAY_SOURCE;
			else
				stage->output_flags &= ~MAT_STAGE_OUTPUT_GODRAY_SOURCE;
		}

		if ((stage->scale_override_flags & MAT_STAGE_OUTPUT_EMISSIVE) == 0u)
			stage->emissive_scale = material->emissive_scale;
		if ((stage->scale_override_flags & MAT_STAGE_OUTPUT_BLOOM) == 0u)
			stage->bloom_scale = material->bloom_scale;
		if ((stage->scale_override_flags & MAT_STAGE_OUTPUT_GODRAY_SOURCE) == 0u)
			stage->godray_scale = material->godray_scale;
	}

	if (VEC_SIZE (material->stages) > 0)
		material->stage0 = material->stages[0];
	else
	{
		mat_shader_stage_t *stage = &material->stage0;

		stage->output_flags |= MAT_STAGE_OUTPUT_COLOR;
		if ((stage->output_override_flags & MAT_STAGE_OUTPUT_EMISSIVE) == 0u)
		{
			if (material->emissive_enable)
				stage->output_flags |= MAT_STAGE_OUTPUT_EMISSIVE;
			else
				stage->output_flags &= ~MAT_STAGE_OUTPUT_EMISSIVE;
		}
		if ((stage->output_override_flags & MAT_STAGE_OUTPUT_BLOOM) == 0u)
		{
			if (material->bloom_enable)
				stage->output_flags |= MAT_STAGE_OUTPUT_BLOOM;
			else
				stage->output_flags &= ~MAT_STAGE_OUTPUT_BLOOM;
		}
		if ((stage->output_override_flags & MAT_STAGE_OUTPUT_GODRAY_SOURCE) == 0u)
		{
			if (material->godray_enable)
				stage->output_flags |= MAT_STAGE_OUTPUT_GODRAY_SOURCE;
			else
				stage->output_flags &= ~MAT_STAGE_OUTPUT_GODRAY_SOURCE;
		}

		if ((stage->scale_override_flags & MAT_STAGE_OUTPUT_EMISSIVE) == 0u)
			stage->emissive_scale = material->emissive_scale;
		if ((stage->scale_override_flags & MAT_STAGE_OUTPUT_BLOOM) == 0u)
			stage->bloom_scale = material->bloom_scale;
		if ((stage->scale_override_flags & MAT_STAGE_OUTPUT_GODRAY_SOURCE) == 0u)
			stage->godray_scale = material->godray_scale;
	}
}

static const char *ParseMaterialBlock (const char *data, const char *name, const char *source_file)
{
	shader_material_t material;
	const shader_material_t *existing;
	int stage_index = 0;
	qboolean sort_explicit = false;
	qboolean stage_limit_warned = false;
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

	while ((data = COM_Parse (data)) != NULL)
	{
		const char *value;
		float scale;

		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "}"))
			break;
		if (!strcmp (com_token, "{"))
		{
			if (stage_index >= MAT_SHADER_MAX_STAGES)
			{
				char warn_key[MAX_QPATH * 2];
				char warn_msg[256];

				if (!stage_limit_warned)
				{
					q_snprintf (warn_key, sizeof (warn_key), "%s::maxstages", material.name ? material.name : "shader");
					q_snprintf (warn_msg, sizeof (warn_msg),
						"%s exceeds max stages (%d); extra stages ignored",
						material.name ? material.name : "shader",
						MAT_SHADER_MAX_STAGES);
					Mat_Shader_ReportWarningOnce (warn_key, warn_msg);
					stage_limit_warned = true;
				}
				data = SkipUnknownBlockOrLine (data, true);
				continue;
			}

			data = ParseStageBlock (data, &material, (size_t)stage_index++);
			continue;
		}
		if (!q_strcasecmp (com_token, "qer_editorimage"))
		{
			if (!ParseIdent (&data, &value))
				break;
			material.editor_image = Mat_Shader_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "surfaceparm"))
		{
			if (!ParseIdent (&data, &value))
				break;
			Mat_Shader_ApplySurfaceParm (&material, value);
			continue;
		}
		if (!q_strcasecmp (com_token, "cull"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (Mat_Shader_ParseCullMode (value, &material.cull_mode))
				continue;
			Mat_Shader_ReportUnknownToken (value, material.name);
			continue;
		}
		if (!q_strcasecmp (com_token, "sort"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (Mat_Shader_ParseSortKey (value, &material.sort_key))
			{
				if (material.sort_key != MAT_SORT_OPAQUE)
					material.render_flags |= MAT_RENDER_TRANS;
				sort_explicit = true;
				continue;
			}
			Mat_Shader_ReportUnknownToken (value, material.name);
			continue;
		}
		if (!q_strcasecmp (com_token, "polygonOffset"))
		{
			qboolean parsed = false;
			qboolean value_bool = true;

			parsed = ParseOptionalBool (&data, &value_bool);
			material.polygon_offset = parsed ? value_bool : true;
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive"))
		{
			if (!ParseIdent (&data, &value))
				break;
			material.emissive_enable = Mat_Shader_ParseBool (value, false);
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom"))
		{
			if (!ParseIdent (&data, &value))
				break;
			material.bloom_enable = Mat_Shader_ParseBool (value, false);
			continue;
		}
		if (!q_strcasecmp (com_token, "godray"))
		{
			if (!ParseIdent (&data, &value))
				break;
			material.godray_enable = Mat_Shader_ParseBool (value, false);
			continue;
		}
		if (!q_strcasecmp (com_token, "emissive_scale"))
		{
			if (!ParseFloat (&data, &scale))
				break;
			material.emissive_scale = scale;
			continue;
		}
		if (!q_strcasecmp (com_token, "bloom_scale"))
		{
			if (!ParseFloat (&data, &scale))
				break;
			material.bloom_scale = scale;
			continue;
		}
		if (!q_strcasecmp (com_token, "godray_scale"))
		{
			if (!ParseFloat (&data, &scale))
				break;
			material.godray_scale = scale;
			continue;
		}
		Mat_Shader_ReportUnknownToken (com_token, material.name);
		data = SkipUnknownBlockOrLine (data, false);
		if (!data)
			break;
	}

	Mat_Shader_ApplyStageDefaults (&material);

	if (!sort_explicit)
	{
		if (material.polygon_offset)
		{
			material.sort_key = MAT_SORT_DECAL;
			material.render_flags |= MAT_RENDER_TRANS;
		}
		else if (material.render_flags & MAT_RENDER_TRANS)
		{
			material.sort_key = MAT_SORT_ADDITIVE;
		}
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

	(void) path;

	if (!cursor)
		return 0;

	while ((cursor = COM_Parse (cursor)) != NULL)
	{
		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "{"))
		{
			cursor = SkipUnknownBlockOrLine (cursor, true);
			continue;
		}
		{
			char name[MAX_QPATH];
			q_strlcpy (name, com_token, sizeof (name));
			if (!ExpectToken (&cursor, "{"))
				continue;
			cursor = ParseMaterialBlock (cursor, name, source_file);
			parsed++;
		}
	}

	return parsed;
}
