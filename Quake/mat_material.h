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

#ifndef MAT_MATERIAL_H
#define MAT_MATERIAL_H

#include "quakedef.h"

typedef enum
{
	MAT_SURFPARM_SOLID		= (1u << 0),
	MAT_SURFPARM_NONSOLID		= (1u << 1),
	MAT_SURFPARM_PLAYERCLIP		= (1u << 2),
	MAT_SURFPARM_MONSTERCLIP	= (1u << 3),
	MAT_SURFPARM_TRANS		= (1u << 4),
	MAT_SURFPARM_ALPHAOCCLUDE	= (1u << 5),
	MAT_SURFPARM_SKY		= (1u << 6),
	MAT_SURFPARM_FOG		= (1u << 7),
	MAT_SURFPARM_NODRAW		= (1u << 8),
	MAT_SURFPARM_STONE		= (1u << 9)
} mat_surfaceparm_t;

typedef enum
{
	MAT_RENDER_TRANS		= (1u << 0),
	MAT_RENDER_ALPHAOCCLUDE		= (1u << 1),
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
	MAT_SORT_SKY = 2,
	MAT_SORT_OPAQUE = 3,
	MAT_SORT_SEE_THROUGH = 4,
	MAT_SORT_DECAL = 5,
	MAT_SORT_BANNER = 6,
	MAT_SORT_UNDERWATER = 8,
	MAT_SORT_ADDITIVE = 9,
	MAT_SORT_NEAREST = 16
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

typedef enum
{
	MAT_PARTICLE_STAGE_SUPPORTED = 0,
	MAT_PARTICLE_STAGE_SKIPPED,
	MAT_PARTICLE_STAGE_HARD_FAIL
} mat_particle_stage_support_t;

typedef enum
{
	MAT_PARTICLE_POLICY_TOLERANT = 0,
	MAT_PARTICLE_POLICY_STRICT
} mat_particle_policy_t;

typedef struct mat_texmatrix_s
{
	float m[3][3];
} mat_texmatrix_t;

typedef enum
{
	MATERIAL_FLAG_NODRAW		= (1u << 0),
	MATERIAL_FLAG_SKY		= (1u << 1),
	MATERIAL_FLAG_TRANS		= (1u << 2),
	MATERIAL_FLAG_ALPHAOCCLUDE	= (1u << 3),
	MATERIAL_FLAG_FOG		= (1u << 4),
	MATERIAL_FLAG_SOLID		= (1u << 5),
	MATERIAL_FLAG_NONSOLID		= (1u << 6),
	MATERIAL_FLAG_PLAYERCLIP	= (1u << 7),
	MATERIAL_FLAG_MONSTERCLIP	= (1u << 8),
	MATERIAL_FLAG_STONE		= (1u << 9),
	MATERIAL_FLAG_EMISSIVE		= (1u << 10),
	MATERIAL_FLAG_BLOOM		= (1u << 11),
	MATERIAL_FLAG_GODRAY		= (1u << 12)
} material_flags_t;

typedef struct mat_material_stage_s
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
} material_stage_t;

typedef enum
{
	MAT_STAGE_OUT_COLOR		= (1u << 0),
	MAT_STAGE_OUT_EMISSIVE		= (1u << 1),
	MAT_STAGE_OUT_BLOOM		= (1u << 2),
	MAT_STAGE_OUT_GODRAY_SOURCE	= (1u << 3)
} mat_stage_output_flags_t;

typedef struct material_s
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
	float			polygon_offset_factor;
	float			polygon_offset_units;
	qboolean		emissive_enable;
	qboolean		bloom_enable;
	qboolean		godray_enable;
	float			emissive_scale;
	float			bloom_scale;
	float			godray_scale;
	material_stage_t	stage0;
	material_stage_t	*stages;
} material_t;

// Developer note:
// Material parsing supports top-level and stage blocks as implemented by ParseMaterialBlock
// and ParseStageBlock in Quake/mat_material_parse.c.
//
// Capability snapshot (see keyword table/docs for exact coverage):
// - Implemented: core top-level metadata/flags (qer_editorimage, surfaceparm, polygonOffset,
//   emissive/bloom/godray + *_scale) and core stage features (map/clampmap, depthWrite,
//   stage emissive/bloom/godray + *Scale).
// - Partial: selected classic directives such as cull, sort, animMap, rgbGen/alphaGen,
//   blendFunc, depthFunc, tcGen, and tcMod.
// - Deferred (known unimplemented): skyParms, fogParms, deformVertexes, q3map_*, alphaFunc.
//
// Extension/status references:
// - Canonical status map: mat_material_keyword_table in Quake/mat_material.c.
// - Authoring guide: docs/how2use-materials.md.
// - Particle constraints: docs/particle_material_contract.md.
//
// Warning: parser coverage evolves over time; always treat mat_material_keyword_table as the
// canonical source of directive support status.

typedef struct texture_s texture_t;

extern cvar_t r_materials;
extern cvar_t r_material_debug;
extern cvar_t r_tcgen_debug;
extern cvar_t r_sun_visibility;
extern cvar_t r_material_debug_parse;
extern cvar_t r_particles_material_strict;

#define MAT_PARTICLE_SHADER_PREFIX "particles/"

typedef enum
{
	MATERIAL_KEYWORD_SCOPE_TOPLEVEL = 0,
	MATERIAL_KEYWORD_SCOPE_STAGE,
	MATERIAL_KEYWORD_SCOPE_SURFACEPARM
} material_keyword_scope_t;

typedef enum
{
	MATERIAL_KEYWORD_STATUS_IMPLEMENTED = 0,
	MATERIAL_KEYWORD_STATUS_PARTIAL,
	MATERIAL_KEYWORD_STATUS_KNOWN_UNIMPLEMENTED
} material_keyword_status_t;

void Material_Init (void);
void Material_Shutdown (void);
void Material_Reload (void);
size_t Material_Count (void);
const material_t *Material_GetByIndex (size_t index);
void Material_Canonicalize (const char *name, char *out, size_t out_size);
const material_t *Material_Find (const char *name);
const material_t *Material_FindForTextureName (const char *texname, const char *mapname);
const char *Material_GetStage0Map (const material_t *material, const char *texname);
unsigned int Material_GetTextureFlags (const material_t *material);
void Material_ApplyToTexture (texture_t *tex, const char *mapname);
void Material_Print (const material_t *material);
char *Material_DupString (const char *value);
const mat_texmatrix_t *MaterialStage_EvalTexMatrix (material_stage_t *stage, float time);
int MaterialStage_EvalAnimMapFrame (material_stage_t *stage, float time);
const char *MaterialStage_GetAnimMapPath (material_stage_t *stage, float time);
float Material_EvalWaveValue (const mat_wave_t *wave, float time);
void Material_Insert (material_t *material);
void Material_Remove (const material_t *material);
void Material_MarkKeywordSeen (const char *keyword, material_keyword_scope_t scope);
void Material_ReportUnknownToken (const char *token, material_keyword_scope_t scope, const char *context, const char *source_file, unsigned int line);
qboolean Material_StageSupportsParticleMVP (const material_stage_t *stage, char *reason, size_t reason_size);
mat_particle_stage_support_t Material_ClassifyParticleStage (const material_stage_t *stage,
	mat_particle_policy_t policy, char *reason, size_t reason_size);

#endif // MAT_MATERIAL_H
