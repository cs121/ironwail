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
	MAT_RGBGEN_IDENTITY = 0,
	MAT_RGBGEN_VERTEX,
	MAT_RGBGEN_CONST,
	MAT_RGBGEN_WAVE
} mat_rgbgen_t;

typedef enum
{
	MAT_ALPHAGEN_IDENTITY = 0,
	MAT_ALPHAGEN_VERTEX,
	MAT_ALPHAGEN_CONST,
	MAT_ALPHAGEN_WAVE
} mat_alphagen_t;

typedef enum
{
	MAT_WAVE_SIN = 0,
	MAT_WAVE_TRIANGLE,
	MAT_WAVE_SAW,
	MAT_WAVE_INVERSESAW
} mat_wave_type_t;

typedef struct mat_wave_s
{
	mat_wave_type_t	type;
	float		base;
	float		amp;
	float		phase;
	float		freq;
} mat_wave_t;

typedef enum
{
	MAT_CULL_BACK = 0,
	MAT_CULL_FRONT,
	MAT_CULL_NONE
} mat_cull_mode_t;

typedef enum
{
	MAT_SORT_OPAQUE = 0,
	MAT_SORT_DECAL,
	MAT_SORT_ADDITIVE,
	MAT_SORT_NEAREST
} mat_sort_key_t;

typedef enum
{
	MAT_BLEND_REPLACE = 0,
	MAT_BLEND_ALPHA,
	MAT_BLEND_ADD,
	MAT_BLEND_MULT,
	MAT_BLEND_PREMULT,
	MAT_BLEND_CUSTOM
} mat_blend_mode_t;

typedef enum
{
	MAT_DEPTHFUNC_LEQUAL = 0,
	MAT_DEPTHFUNC_LESS,
	MAT_DEPTHFUNC_EQUAL,
	MAT_DEPTHFUNC_GREATER,
	MAT_DEPTHFUNC_GEQUAL,
	MAT_DEPTHFUNC_ALWAYS,
	MAT_DEPTHFUNC_NEVER
} mat_depthfunc_t;

typedef enum
{
	MAT_MAP_MAP = 0,
	MAT_MAP_CLAMPMAP,
	MAT_MAP_LIGHTMAP,
	MAT_MAP_WHITE,
	MAT_MAP_BLACK
} mat_map_type_t;

typedef enum
{
	MAT_TCGEN_BASE = 0,
	MAT_TCGEN_ENVIRONMENT,
	MAT_TCGEN_LIGHTMAP
} mat_tcgen_t;

typedef enum
{
	MAT_TCMOD_NONE = 0,
	MAT_TCMOD_SCROLL,
	MAT_TCMOD_SCALE,
	MAT_TCMOD_ROTATE,
	MAT_TCMOD_TURB,
	MAT_TCMOD_STRETCH
} mat_tcmod_type_t;

typedef struct mat_tcmod_s
{
	mat_tcmod_type_t type;
	float args[4];
} mat_tcmod_t;

typedef struct mat_texmatrix_s
{
	float m[3][3];
} mat_texmatrix_t;

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
	unsigned int		outputs;
	unsigned int		output_overrides;
	float			emissive_scale;
	float			bloom_scale;
	float			godray_scale;
	qboolean		emissive_scale_set;
	qboolean		bloom_scale_set;
	qboolean		godray_scale_set;
	char		*map_path;
	mat_rgbgen_t	rgbgen;
	mat_alphagen_t	alphagen;
	float		const_color[3];
	float		const_alpha;
	mat_wave_t	rgb_wave;
	mat_wave_t	alpha_wave;
	mat_blend_mode_t	blend_mode;
	int			blend_src;
	int			blend_dst;
	qboolean		depth_write;
	mat_depthfunc_t		depth_func;
	mat_map_type_t		map_type;
	mat_tcgen_t		tcgen;
	int			tcmod_count;
	mat_tcmod_t		tcmods[4];
	float			anim_map_fps;
	char			**anim_map_frames;
	int			texmatrix_time_bucket;
	int			anim_map_time_bucket;
	int			anim_map_frame;
	mat_texmatrix_t	texmatrix_cache;
} mat_shader_stage_t;

typedef enum
{
	MAT_STAGE_OUT_COLOR		= (1u << 0),
	MAT_STAGE_OUT_EMISSIVE		= (1u << 1),
	MAT_STAGE_OUT_BLOOM		= (1u << 2),
	MAT_STAGE_OUT_GODRAY_SOURCE	= (1u << 3)
} mat_stage_output_flags_t;

typedef struct shader_material_s
{
	char			*name;
	char			*editor_image;
	char			*source_file;
	unsigned int		surfaceparms;
	unsigned int		content_flags;
	unsigned int		render_flags;
	mat_cull_mode_t		cull_mode;
	mat_sort_key_t		sort_key;
	qboolean		polygon_offset;
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
// bloom_scale, godray_scale, emissiveScale, bloomScale, godrayScale, and a single stage block
// with map + rgbGen identity.
// To add new surfaceparms, extend mat_surfaceparm_table in mat_shader_parse.c and map to flags.

typedef struct texture_s texture_t;

extern cvar_t r_shaders;
extern cvar_t r_shader_debug;
extern cvar_t r_matshader_debug_parse;

typedef enum
{
	MAT_SHADER_KEYWORD_SCOPE_TOPLEVEL = 0,
	MAT_SHADER_KEYWORD_SCOPE_STAGE,
	MAT_SHADER_KEYWORD_SCOPE_SURFACEPARM
} mat_shader_keyword_scope_t;

typedef enum
{
	MAT_SHADER_KEYWORD_STATUS_IMPLEMENTED = 0,
	MAT_SHADER_KEYWORD_STATUS_PARTIAL,
	MAT_SHADER_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED
} mat_shader_keyword_status_t;

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
const mat_texmatrix_t *MatStage_EvalTexMatrix (mat_shader_stage_t *stage, float time);
int MatStage_EvalAnimMapFrame (mat_shader_stage_t *stage, float time);
const char *MatStage_GetAnimMapPath (mat_shader_stage_t *stage, float time);
float Mat_Shader_EvalWaveValue (const mat_wave_t *wave, float time);
void Mat_Shader_Insert (shader_material_t *material);
void Mat_Shader_Remove (const shader_material_t *material);
void Mat_Shader_MarkKeywordSeen (const char *keyword, mat_shader_keyword_scope_t scope);
void Mat_Shader_ReportUnknownToken (const char *token, mat_shader_keyword_scope_t scope, const char *context, const char *source_file, unsigned int line);

#endif // MAT_SHADER_H
