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

#ifndef MAT_SHADER_H
#define MAT_SHADER_H

#include "quakedef.h"

typedef enum
{
	MAT_SURFPARM_SOLID		= (1u << 0),
	MAT_SURFPARM_NONSOLID		= (1u << 1),
	MAT_SURFPARM_PLAYERCLIP		= (1u << 2),
	MAT_SURFPARM_MONSTERCLIP	= (1u << 3),
	MAT_SURFPARM_TRANS		= (1u << 4),
	MAT_SURFPARM_ALPHASHADOW	= (1u << 5),
	MAT_SURFPARM_SKY		= (1u << 6),
	MAT_SURFPARM_FOG		= (1u << 7),
	MAT_SURFPARM_NODRAW		= (1u << 8),
	MAT_SURFPARM_STONE		= (1u << 9)
} mat_surfaceparm_t;

typedef enum
{
	MAT_RENDER_TRANS		= (1u << 0),
	MAT_RENDER_ALPHASHADOW		= (1u << 1),
	MAT_RENDER_SKY			= (1u << 2),
	MAT_RENDER_FOG			= (1u << 3),
	MAT_RENDER_NODRAW		= (1u << 4)
} mat_render_flags_t;

typedef enum
{
	MAT_CONTENT_SOLID		= (1u << 0),
	MAT_CONTENT_NONSOLID		= (1u << 1),
	MAT_CONTENT_PLAYERCLIP		= (1u << 2),
	MAT_CONTENT_MONSTERCLIP		= (1u << 3)
} mat_content_flags_t;

typedef enum
{
	MAT_RGBGEN_DEFAULT = 0,
	MAT_RGBGEN_IDENTITY
} mat_rgbgen_t;

typedef enum
{
	MAT_SHADERFLAG_NODRAW		= (1u << 0),
	MAT_SHADERFLAG_SKY		= (1u << 1),
	MAT_SHADERFLAG_TRANS		= (1u << 2),
	MAT_SHADERFLAG_ALPHASHADOW	= (1u << 3),
	MAT_SHADERFLAG_FOG		= (1u << 4),
	MAT_SHADERFLAG_SOLID		= (1u << 5),
	MAT_SHADERFLAG_NONSOLID		= (1u << 6),
	MAT_SHADERFLAG_PLAYERCLIP	= (1u << 7),
	MAT_SHADERFLAG_MONSTERCLIP	= (1u << 8),
	MAT_SHADERFLAG_STONE		= (1u << 9),
	MAT_SHADERFLAG_EMISSIVE		= (1u << 10),
	MAT_SHADERFLAG_BLOOM		= (1u << 11),
	MAT_SHADERFLAG_GODRAY		= (1u << 12)
} mat_shader_flags_t;

typedef struct mat_shader_stage_s
{
	char		*map_path;
	mat_rgbgen_t	rgbgen;
} mat_shader_stage_t;

typedef struct shader_material_s
{
	char			*name;
	char			*editor_image;
	char			*source_file;
	unsigned int		surfaceparms;
	unsigned int		content_flags;
	unsigned int		render_flags;
	qboolean		emissive_enable;
	qboolean		bloom_enable;
	qboolean		godray_enable;
	float			emissive_scale;
	float			bloom_scale;
	float			godray_scale;
	mat_shader_stage_t	stage0;
	mat_shader_stage_t	*stages;
} shader_material_t;

// Developer note:
// Supported directives: qer_editorimage, surfaceparm, emissive, bloom, godray, emissive_scale,
// bloom_scale, godray_scale, and a single stage block with map + rgbGen identity.
// To add new surfaceparms, extend mat_surfaceparm_table in mat_shader_parse.c and map to flags.

typedef struct texture_s texture_t;

extern cvar_t r_shaders;
extern cvar_t r_shader_debug;

void Mat_Shader_Init (void);
void Mat_Shader_Shutdown (void);
void Mat_Shader_Reload (void);
size_t Mat_Shader_Count (void);
const shader_material_t *Mat_Shader_GetByIndex (size_t index);
void Mat_Shader_Canonicalize (const char *name, char *out, size_t out_size);
const shader_material_t *Mat_Shader_Find (const char *name);
const shader_material_t *Mat_Shader_FindForTextureName (const char *texname, const char *mapname);
const char *Mat_Shader_GetStage0Map (const shader_material_t *material, const char *texname);
unsigned int Mat_Shader_GetTextureFlags (const shader_material_t *material);
void Mat_Shader_ApplyToTexture (texture_t *tex, const char *mapname);
void Mat_Shader_Print (const shader_material_t *material);
char *Mat_Shader_DupString (const char *value);
void Mat_Shader_Insert (shader_material_t *material);
void Mat_Shader_Remove (const shader_material_t *material);
void Mat_Shader_ReportUnknownToken (const char *token, const char *context);

#endif // MAT_SHADER_H
