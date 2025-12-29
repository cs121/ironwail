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
	stage.rgbgen = MAT_RGBGEN_DEFAULT;

	while ((data = COM_Parse (data)) != NULL)
	{
		const char *value;

		if (!com_token[0])
			continue;
		if (!strcmp (com_token, "}"))
			break;
		if (!q_strcasecmp (com_token, "map"))
		{
			if (!ParseIdent (&data, &value))
				break;
			stage.map_path = Mat_Shader_DupString (value);
			continue;
		}
		if (!q_strcasecmp (com_token, "rgbGen"))
		{
			if (!ParseIdent (&data, &value))
				break;
			if (!q_strcasecmp (value, "identity"))
				stage.rgbgen = MAT_RGBGEN_IDENTITY;
			continue;
		}

		Mat_Shader_ReportUnknownToken (com_token, material->name);
		data = SkipUnknownBlockOrLine (data, false);
		if (!data)
			break;
	}

	if (!material->stages)
		material->stages = NULL;
	VEC_PUSH (material->stages, stage);

	if (stage_index == 0)
		material->stage0 = stage;

	return data;
}

static const char *ParseMaterialBlock (const char *data, const char *name, const char *source_file)
{
	shader_material_t material;
	const shader_material_t *existing;
	int stage_index = 0;
	char canonical[MAX_QPATH];

	memset (&material, 0, sizeof (material));
	Mat_Shader_Canonicalize (name, canonical, sizeof (canonical));
	material.name = Mat_Shader_DupString (canonical);
	material.source_file = Mat_Shader_DupString (source_file ? source_file : "");
	material.emissive_scale = 1.f;
	material.bloom_scale = 1.f;
	material.godray_scale = 1.f;

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
