/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

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
// r_world.c: world model rendering

#include "quakedef.h"
#include "mat_material.h"
#include "r_framegraph.h"
#include "r_realtimelight.h"

extern cvar_t gl_fullbrights, r_oldskyleaf, r_showtris; //johnfitz
extern cvar_t r_godrays;
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_oit;

extern gltexture_t *lightmap_texture;
extern gltexture_t *lightmap_dir_texture;
extern cvar_t r_lightingdir;


extern GLuint gl_bmodel_vbo;
extern size_t gl_bmodel_vbo_size;
extern GLuint gl_bmodel_ibo;
extern size_t gl_bmodel_ibo_size;
extern GLuint gl_bmodel_indirect_buffer;
extern size_t gl_bmodel_indirect_buffer_size;
extern GLuint gl_bmodel_surf_buffer;
extern GLuint gl_bmodel_marksurf_buffer;
extern GLuint gl_bmodel_marksurf_buffer_size;

typedef struct gpumark_frame_s {
	vec4_t		frustum[4];
	vec3_t		vieworg;
	GLuint		oldskyleaf;
	GLuint		framecount;
	GLuint		cull_flags;
	GLuint		padding[2];
	vec4_t		shadow_sphere; // xyz=center, w=radius (0 disables sphere cull)
} gpumark_frame_t;

enum
{
	GPUMARK_CULL_SKY = 1u << 0,
	GPUMARK_CULL_VIS = 1u << 1,
	GPUMARK_CULL_BACKFACE = 1u << 2,
	GPUMARK_CULL_FRUSTUM = 1u << 3,
	GPUMARK_CULL_SHADOW_SPHERE = 1u << 4
};

static uint32_t r_gpumark_dispatch_serial = 0u;
static byte *r_gpumark_last_vis = NULL;
static GLuint r_gpumark_last_flags = 0u;
static qboolean r_gpumark_last_oldskyleaf = false;
static const qmodel_t *r_tex_table_validated_model = NULL;
static qboolean r_tex_table_validated_ok = false;

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

static inline qboolean R_ValidPtr (const void *ptr);
static qboolean R_ValidateTextureTables (const qmodel_t *model);

static qboolean R_BrushModelHasTextureTables (const qmodel_t *model)
{
	return model
		&& model->type == mod_brush
		&& model->numtextures > 0
		&& model->textures
		&& model->usedtextures;
}

static texture_t *R_GetUsedTexture (const qmodel_t *model, int used_index, int *out_texnum)
{
	int used_count;
	int texnum;
	static int last_warn_frame = -1;
	static int last_bad_texnum_frame = -1;
	texture_t *tex;

	if (!model || !model->usedtextures || !model->textures)
		return NULL;
	if (!R_ValidateTextureTables (model))
		return NULL;

	used_count = model->texofs[TEXTYPE_COUNT];
	if (model->numtextures <= 0 || used_count < 0 || used_count > model->numtextures)
	{
		if (r_framecount != last_warn_frame)
		{
			last_warn_frame = r_framecount;
			Con_DWarning ("R_GetUsedTexture: invalid texture table on %s (used=%d numtextures=%d)\n",
				model->name, used_count, model->numtextures);
		}
		return NULL;
	}
	if (used_index < 0 || used_index >= used_count)
		return NULL;

	texnum = model->usedtextures[used_index];
	if (texnum < 0 || texnum >= model->numtextures)
	{
		if (r_framecount != last_bad_texnum_frame)
		{
			last_bad_texnum_frame = r_framecount;
			Con_DWarning ("R_GetUsedTexture: invalid texnum on %s (used_index=%d texnum=%d numtextures=%d)\n",
				model->name, used_index, texnum, model->numtextures);
		}
		return NULL;
	}

	if (out_texnum)
		*out_texnum = texnum;

	tex = model->textures[texnum];
	return tex ? tex : NULL;
}

static inline qboolean R_ValidPtr (const void *ptr)
{
	uintptr_t address = (uintptr_t)ptr;

	if (address == 0 || address == (uintptr_t)-1)
		return false;
	if (address < 0x10000)
		return false;

	return true;
}

static qboolean R_ValidateTextureTables (const qmodel_t *model)
{
	int i;
	int used_count;
	static int last_warn_frame = -1;

	if (!model)
		return false;
	if (model == r_tex_table_validated_model)
		return r_tex_table_validated_ok;

	r_tex_table_validated_model = model;
	r_tex_table_validated_ok = false;

	if (!model->usedtextures || !model->textures)
		return false;
	if (!R_ValidPtr (model->usedtextures) || !R_ValidPtr (model->textures))
	{
		if (r_framecount != last_warn_frame)
		{
			last_warn_frame = r_framecount;
			Con_DWarning ("R_ValidateTextureTables: invalid table pointers on %s (used=%p textures=%p)\n",
				model->name, (void *)model->usedtextures, (void *)model->textures);
		}
		return false;
	}

	used_count = model->texofs[TEXTYPE_COUNT];
	if (model->numtextures <= 0 || used_count < 0 || used_count > model->numtextures)
		return false;

	for (i = 0; i < used_count; ++i)
	{
		const int texnum = model->usedtextures[i];
		texture_t *tex;

		if (texnum < 0 || texnum >= model->numtextures)
			return false;
		tex = model->textures[texnum];
		if (tex && !R_ValidPtr (tex))
		{
			if (r_framecount != last_warn_frame)
			{
				last_warn_frame = r_framecount;
				Con_DWarning ("R_ValidateTextureTables: invalid texture pointer on %s (used_index=%d texnum=%d ptr=%p)\n",
					model->name, i, texnum, (void *)tex);
			}
			return false;
		}
	}

	r_tex_table_validated_ok = true;
	return true;
}

static gltexture_t *R_SanitizeGLTexture (gltexture_t *tex, const char *texname, const char *label)
{
	static int last_bad_glt_frame = -1;

	if (!tex)
		return NULL;

	if (!R_ValidPtr (tex))
	{
		if (r_framecount != last_bad_glt_frame)
		{
			last_bad_glt_frame = r_framecount;
			Con_DWarning ("R_SanitizeGLTexture: invalid %s texture on %s (ptr=%p)\n",
				label, texname ? texname : "<unknown>", (void *)tex);
		}
		return NULL;
	}

	return tex;
}

/*
===============
R_MarkVisSurfaces
===============
*/
static void R_MarkVisSurfaces (byte *vis, GLuint cull_flags, qboolean oldskyleaf, const vec3_t vieworg_override, const mplane_t shadow_frustum[4], const vec4_t *shadow_sphere)
{
	int			i;
	GLuint		buf;
	GLbyte*		ofs;
	size_t		vissize = (cl.worldmodel->numleafs + 7) >> 3;
	size_t		nummark = gl_bmodel_marksurf_buffer_size / sizeof (bmodel_gpu_marksurf_t);
	gpumark_frame_t frame;

	GL_BeginGroup ("Mark surfaces");

	for (i = 0; i < 4; i++)
	{
		const mplane_t *src = shadow_frustum ? &shadow_frustum[i] : &frustum[i];
		frame.frustum[i][0] = src->normal[0];
		frame.frustum[i][1] = src->normal[1];
		frame.frustum[i][2] = src->normal[2];
		frame.frustum[i][3] = src->dist;
	}
	if (vieworg_override)
	{
		frame.vieworg[0] = vieworg_override[0];
		frame.vieworg[1] = vieworg_override[1];
		frame.vieworg[2] = vieworg_override[2];
	}
	else
	{
		frame.vieworg[0] = r_refdef.vieworg[0];
		frame.vieworg[1] = r_refdef.vieworg[1];
		frame.vieworg[2] = r_refdef.vieworg[2];
	}
	frame.oldskyleaf = oldskyleaf ? 1u : 0u;
	if (++r_gpumark_dispatch_serial == 0u)
		r_gpumark_dispatch_serial = 1u;
	frame.framecount = r_gpumark_dispatch_serial;
	frame.cull_flags = cull_flags;
	if (shadow_sphere)
	{
		frame.shadow_sphere[0] = (*shadow_sphere)[0];
		frame.shadow_sphere[1] = (*shadow_sphere)[1];
		frame.shadow_sphere[2] = (*shadow_sphere)[2];
		frame.shadow_sphere[3] = (*shadow_sphere)[3];
	}
	else
	{
		frame.shadow_sphere[0] = 0.f;
		frame.shadow_sphere[1] = 0.f;
		frame.shadow_sphere[2] = 0.f;
		frame.shadow_sphere[3] = 0.f;
	}

	COMPILE_TIME_ASSERT (vis_alignment_must_be_power_of_2, (VIS_ALIGN & (VIS_ALIGN - 1)) == 0);
	COMPILE_TIME_ASSERT (vis_alignment_must_be_multiple_of_uint, (VIS_ALIGN & 3) == 0);
	vissize = (vissize + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK; // round up

	GL_UseProgram (glprogs.clear_indirect);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_indirect_buffer, 0, cl.worldmodel->texofs[TEXTYPE_COUNT] * sizeof(bmodel_draw_indirect_t));
	R_Backend_Dispatch ((unsigned)((cl.worldmodel->texofs[TEXTYPE_COUNT] + 63) / 64), 1u, 1u);
	R_Backend_MemoryBarrier (R_BACKEND_BARRIER_SHADER_STORAGE);

	GL_UseProgram (glprogs.cull_mark);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_ibo, 0, gl_bmodel_ibo_size);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, vis, vissize, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, buf, (GLintptr)ofs, vissize);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, gl_bmodel_marksurf_buffer, 0, gl_bmodel_marksurf_buffer_size);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_surf_buffer, 0, cl.worldmodel->numsurfaces * sizeof(bmodel_gpu_surf_t));
	GL_Upload (GL_UNIFORM_BUFFER, &frame, sizeof(frame), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 1, buf, (GLintptr)ofs, sizeof(frame));

	R_Backend_Dispatch ((unsigned)((nummark + 63) / 64), 1u, 1u);
	R_Backend_MemoryBarrier (R_BACKEND_BARRIER_COMMAND | R_BACKEND_BARRIER_SHADER_STORAGE | R_BACKEND_BARRIER_ELEMENT_ARRAY);

	GL_EndGroup ();
}

/*
===============
R_MarkSurfaces
===============
*/
void R_MarkSurfaces (void)
{
	byte		*vis;
	int			i;
	qboolean	nearwaterportal;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i=0; i < r_viewleaf->nummarksurfaces; i++)
		if (cl.worldmodel->surfaces[r_viewleaf->firstmarksurface[i]].flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (r_novis.value || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		vis = Mod_NoVisPVS (cl.worldmodel);
	else if (nearwaterportal)
		vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	r_visframecount++;

	r_gpumark_last_vis = vis;
	r_gpumark_last_flags = GPUMARK_CULL_SKY | GPUMARK_CULL_VIS | GPUMARK_CULL_BACKFACE | GPUMARK_CULL_FRUSTUM;
	r_gpumark_last_oldskyleaf = (r_oldskyleaf.value != 0.f);

	R_MarkVisSurfaces (vis, r_gpumark_last_flags, r_gpumark_last_oldskyleaf, NULL, NULL, NULL);
	R_AddStaticModels (vis);
}

void R_MarkSurfaces_Shadow (const vec3_t vieworg, const mplane_t shadow_frustum[4], const vec4_t *shadow_sphere, qboolean use_vis, qboolean use_backface, qboolean use_frustum)
{
	byte *vis;
	GLuint cull_flags = GPUMARK_CULL_SKY;

	if (!cl.worldmodel)
		return;

	/* Shadow marking can optionally reuse camera VIS for speed. Frustum/sphere
	 * culling is controlled by the caller per shadow pass. */
	if (use_vis && r_gpumark_last_vis)
	{
		vis = r_gpumark_last_vis;
		cull_flags |= GPUMARK_CULL_VIS;
	}
	else
	{
		vis = Mod_NoVisPVS (cl.worldmodel);
	}
	if (use_backface)
		cull_flags |= GPUMARK_CULL_BACKFACE;
	if (use_frustum)
		cull_flags |= GPUMARK_CULL_FRUSTUM;
	if (shadow_sphere && (*shadow_sphere)[3] > 0.f)
		cull_flags |= GPUMARK_CULL_SHADOW_SPHERE;

	R_MarkVisSurfaces (vis, cull_flags, false, vieworg, shadow_frustum, shadow_sphere);
}

void R_RestoreMarkedSurfaces (void)
{
	if (!cl.worldmodel || !r_gpumark_last_vis)
		return;

	R_MarkVisSurfaces (r_gpumark_last_vis, r_gpumark_last_flags, r_gpumark_last_oldskyleaf, NULL, NULL, NULL);
}

/*
================
GL_WaterAlphaForEntityTextureType
 
Returns the water alpha to use for the entity and texture type combination.
================
*/
float GL_WaterAlphaForEntityTextureType (entity_t *ent, textype_t type)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForTextureType(type);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}

typedef struct bmodel_gpu_instance_s {
	float		world[12];	// world matrix (transposed mat4x3)
	float		prev_world[12];	// previous world matrix (transposed mat4x3)
	float		alpha;
	vec3_t		relight;
} bmodel_gpu_instance_t;

typedef struct bmodel_bindless_gpu_call_s {
	GLuint		flags;
	GLuint		tcgen;
	GLfloat		alpha;
	GLint		spec_mode;
	GLfloat		polygon_offset[2];
	GLfloat		specular[2]; /* x = intensity, y = exponent */
	GLfloat		stage_color[4];
	GLuint64	texture;
	GLuint64	fullbright;
	GLuint64	emissive;
	GLuint64	normal_map;
	GLuint64	specular_map;
	GLuint		padding[2];
} bmodel_bindless_gpu_call_t;

typedef struct bmodel_bound_gpu_call_s {
	GLuint		flags;
	GLuint		tcgen;
	GLfloat		alpha;
	GLint		spec_mode;
	GLfloat		polygon_offset[2];
	GLfloat		specular[2]; /* x = intensity, y = exponent */
	GLfloat		stage_color[4];
	GLint		baseinstance;
	GLint		padding[3];
} bmodel_bound_gpu_call_t;

typedef struct bmodel_gpu_call_remap_s {
	GLuint		src;
	GLuint		inst;
} bmodel_gpu_call_remap_t;

COMPILE_TIME_ASSERT (bmodel_instance_std430_size, sizeof (bmodel_gpu_instance_t) == 112);
COMPILE_TIME_ASSERT (bmodel_bindless_call_std430_size, sizeof (bmodel_bindless_gpu_call_t) == 96);
COMPILE_TIME_ASSERT (bmodel_bound_call_std430_size, sizeof (bmodel_bound_gpu_call_t) == 64);
COMPILE_TIME_ASSERT (bmodel_call_remap_std430_size, sizeof (bmodel_gpu_call_remap_t) == 8);
COMPILE_TIME_ASSERT (bmodel_bindless_call_spec_mode_offset, offsetof (bmodel_bindless_gpu_call_t, spec_mode) == 12);
COMPILE_TIME_ASSERT (bmodel_bindless_call_normal_map_offset, offsetof (bmodel_bindless_gpu_call_t, normal_map) == 72);
COMPILE_TIME_ASSERT (bmodel_bindless_call_spec_map_offset, offsetof (bmodel_bindless_gpu_call_t, specular_map) == 80);
COMPILE_TIME_ASSERT (bmodel_bound_call_spec_mode_offset, offsetof (bmodel_bound_gpu_call_t, spec_mode) == 12);

static bmodel_gpu_instance_t		bmodel_instances[MAX_VISEDICTS + 1]; // +1 for worldspawn
static union {
	struct {
		bmodel_bindless_gpu_call_t	params[MAX_BMODEL_DRAWS];
	} bindless;
	struct {
		bmodel_bound_gpu_call_t		params[MAX_BMODEL_DRAWS];
		gltexture_t					*textures[MAX_BMODEL_DRAWS][5];
	} bound;
} bmodel_calls;
static bmodel_gpu_call_remap_t		bmodel_call_remap[MAX_BMODEL_DRAWS];
static int							num_bmodel_calls;
static GLuint						bmodel_batch_program;

enum
{
	BMODEL_TEX_SLOT_BASE = 0,
	BMODEL_TEX_SLOT_FULLBRIGHT,
	BMODEL_TEX_SLOT_EMISSIVE,
	BMODEL_TEX_SLOT_NORMAL,
	BMODEL_TEX_SLOT_SPECULAR_OR_ORM,
	BMODEL_TEX_SLOT_COUNT
};

COMPILE_TIME_ASSERT (bmodel_bound_texture_slot_count, BMODEL_TEX_SLOT_COUNT == 5);

static void R_GetPolygonOffsetValues (const material_t *material, qboolean use_offset,
	float *factor, float *units)
{
	if (!factor || !units)
		return;
	if (!use_offset)
	{
		*factor = 0.f;
		*units = 0.f;
		return;
	}
	if (material && material->polygon_offset)
	{
		*factor = material->polygon_offset_factor;
		*units = material->polygon_offset_units;
		return;
	}
	*factor = 0.f;
	*units = 1.f;
}

static void R_SampleBModelStaticLighting (const vec3_t pos, vec3_t out_color)
{
	VectorClear (out_color);

	if (!cl.worldmodel)
		return;

	if (r_model_lightgrid.value > 0.f && R_LightgridEnabled ())
	{
		// R_LightgridLighting already applies AO internally.
		R_LightgridLighting (pos, out_color, NULL);
	}
	else
	{
		R_SampleLightmapAtPoint (pos, out_color);
	}

	for (int i = 0; i < 3; i++)
	{
#if defined(_MSC_VER)
		if (_finite (out_color[i]) == 0)
#else
		if (!isfinite (out_color[i]))
#endif
			out_color[i] = 0.f;
		out_color[i] = CLAMP (-1.f, out_color[i], 1.f);
	}
}

static void R_RotateOffsetByAngles (vec3_t angles, const vec3_t local_offset, vec3_t out_world_offset)
{
	vec3_t forward, right, up;

	AngleVectors (angles, forward, right, up);
	out_world_offset[0] = forward[0] * local_offset[0] + right[0] * local_offset[1] + up[0] * local_offset[2];
	out_world_offset[1] = forward[1] * local_offset[0] + right[1] * local_offset[1] + up[1] * local_offset[2];
	out_world_offset[2] = forward[2] * local_offset[0] + right[2] * local_offset[1] + up[2] * local_offset[2];
}

static int R_BModelBuildProbePoints (entity_t *ent, const vec3_t origin, vec3_t angles,
	vec3_t out_points[5], float out_weights[5])
{
	float scale;
	float half_extent_x;
	float half_extent_y;
	float xofs;
	float yofs;
	int side_count = 0;
	int count = 0;
	float side_weight = 0.f;

	VectorCopy (origin, out_points[count]);
	out_weights[count++] = 1.f;

	if (!ent || !ent->model)
		return count;

	scale = (ent == &cl_entities[0]) ? ENTSCALE_DEFAULT : ent->scale;
	half_extent_x = q_max (fabsf (ent->model->mins[0]), fabsf (ent->model->maxs[0])) * scale;
	half_extent_y = q_max (fabsf (ent->model->mins[1]), fabsf (ent->model->maxs[1])) * scale;

	if (half_extent_x > 4.f)
		side_count += 2;
	if (half_extent_y > 4.f)
		side_count += 2;

	if (side_count <= 0)
		return count;

	out_weights[0] = 0.4f;
	side_weight = 0.6f / (float)side_count;
	xofs = CLAMP (8.f, half_extent_x * 0.4f, 64.f);
	yofs = CLAMP (8.f, half_extent_y * 0.4f, 64.f);

	if (half_extent_x > 4.f && count + 1 < 5)
	{
		vec3_t local = {xofs, 0.f, 0.f};
		vec3_t world_offset;
		R_RotateOffsetByAngles (angles, local, world_offset);
		VectorAdd (origin, world_offset, out_points[count]);
		out_weights[count++] = side_weight;

		VectorScale (world_offset, -1.f, world_offset);
		VectorAdd (origin, world_offset, out_points[count]);
		out_weights[count++] = side_weight;
	}

	if (half_extent_y > 4.f && count + 1 < 5)
	{
		vec3_t local = {0.f, yofs, 0.f};
		vec3_t world_offset;
		R_RotateOffsetByAngles (angles, local, world_offset);
		VectorAdd (origin, world_offset, out_points[count]);
		out_weights[count++] = side_weight;

		VectorScale (world_offset, -1.f, world_offset);
		VectorAdd (origin, world_offset, out_points[count]);
		out_weights[count++] = side_weight;
	}

	return count;
}

static void R_SampleBModelStaticLightingAveraged (entity_t *ent, const vec3_t origin, vec3_t angles, vec3_t out_color)
{
	vec3_t points[5];
	float weights[5];
	float weight_sum = 0.f;
	int count;

	VectorClear (out_color);
	count = R_BModelBuildProbePoints (ent, origin, angles, points, weights);

	for (int i = 0; i < count; i++)
	{
		vec3_t sample;
		R_SampleBModelStaticLighting (points[i], sample);
		VectorMA (out_color, weights[i], sample, out_color);
		weight_sum += weights[i];
	}

	if (weight_sum > 0.f)
		VectorScale (out_color, 1.f / weight_sum, out_color);

	for (int i = 0; i < 3; i++)
		out_color[i] = CLAMP (-1.f, out_color[i], 1.f);
}

/*
=============
R_InitBModelInstance
=============
*/
static void R_InitBModelInstance (bmodel_gpu_instance_t *inst, entity_t *ent)
{
        vec3_t angles;
        vec3_t prev_angles;
        vec3_t curr_origin;
        vec3_t prev_origin;
        float mat[16];
        float prev_mat[16];
        qboolean has_prev = false;

        VectorCopy (ent->origin, curr_origin);
        VectorCopy (ent->angles, angles);

        if (ent->motion_blur_prev_valid && ent->motion_blur_prev_frame == r_framecount - 1)
        {
                VectorCopy (ent->motion_blur_prev_origin, prev_origin);
                VectorCopy (ent->motion_blur_prev_angles, prev_angles);
                has_prev = true;
        }

        if (!has_prev)
        {
                VectorCopy (curr_origin, prev_origin);
                VectorCopy (angles, prev_angles);
        }

        vec3_t matrix_angles;
        vec3_t prev_matrix_angles;
        float scale = (ent == &cl_entities[0]) ? ENTSCALE_DEFAULT : ent->scale;

        VectorCopy (angles, matrix_angles);
        VectorCopy (prev_angles, prev_matrix_angles);
        matrix_angles[0] = -matrix_angles[0];
        prev_matrix_angles[0] = -prev_matrix_angles[0];

        R_EntityMatrix (mat, curr_origin, matrix_angles, scale);
        R_EntityMatrix (prev_mat, prev_origin, prev_matrix_angles, scale);

        MatrixTranspose4x3 (mat, inst->world);
        MatrixTranspose4x3 (prev_mat, inst->prev_world);

        VectorCopy (curr_origin, ent->motion_blur_prev_origin);
        VectorCopy (ent->angles, ent->motion_blur_prev_angles);
        ent->motion_blur_prev_frame = r_framecount;
        ent->motion_blur_prev_valid = true;

	inst->alpha = ent->alpha == ENTALPHA_DEFAULT ? -1.f : ENTALPHA_DECODE (ent->alpha);
	VectorClear (inst->relight);

	if (ent != &cl_entities[0] && ent->model && !ent->model->lightdata)
	{
		/* External bmodel pickups (e.g. maps/b_bh*.bsp) may not carry baked
		 * lightmap samples. Provide a static world-sample fallback so they
		 * do not render as black cubes. */
		R_SampleBModelStaticLightingAveraged (ent, curr_origin, angles, inst->relight);
		for (int i = 0; i < 3; i++)
			inst->relight[i] = CLAMP (0.f, inst->relight[i], 1.f);
	}
	else if (r_bmodel_relight.value > 0.f && ent != &cl_entities[0] && cl.worldmodel)
	{
		vec3_t current_static;
		vec3_t baseline_static;

		R_SampleBModelStaticLightingAveraged (ent, curr_origin, angles, current_static);
		R_SampleBModelStaticLightingAveraged (ent, ent->baseline.origin, ent->baseline.angles, baseline_static);
		VectorSubtract (current_static, baseline_static, inst->relight);
		for (int i = 0; i < 3; i++)
			inst->relight[i] = CLAMP (-1.f, inst->relight[i], 1.f);
	}
}

/*
=============
R_ResetBModelCalls
=============
*/
static void R_ResetBModelCalls (GLuint program)
{
	bmodel_batch_program = program;
	num_bmodel_calls = 0;
}

static qboolean R_IsWorldLightingProgram (GLuint program)
{
	int oit, dither, mode, alphatest;

	for (oit = 0; oit < 2; ++oit)
		for (dither = 0; dither < 3; ++dither)
			for (mode = 0; mode < 3; ++mode)
				if (glprogs.world[oit][dither][mode] == program)
					return true;

	for (alphatest = 0; alphatest < 2; ++alphatest)
		if (glprogs.world_dlight[alphatest] == program)
			return true;

	return false;
}

static qboolean R_IsWorldShadowCasterProgram (GLuint program)
{
	return program == glprogs.world_shadow[0] || program == glprogs.world_shadow[1];
}

/*
=============
R_FlushBModelCalls
=============
*/
static void R_FlushBModelCalls (void)
{
	GLuint	cmdbuf, buf;
	GLbyte	*ofs;
	size_t	dstcmdofs;

	if (!num_bmodel_calls)
		return;

	GL_ReserveDeviceMemory (GL_DRAW_INDIRECT_BUFFER, sizeof (bmodel_draw_indirect_t) * num_bmodel_calls, &cmdbuf, &dstcmdofs);

	GL_UseProgram (glprogs.gather_indirect);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_indirect_buffer, 0, gl_bmodel_indirect_buffer_size);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 6, cmdbuf, dstcmdofs, sizeof (bmodel_draw_indirect_t) * num_bmodel_calls);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_call_remap, sizeof (bmodel_call_remap[0]) * num_bmodel_calls, &buf, &ofs);
	GL_BindBufferRange  (GL_SHADER_STORAGE_BUFFER, 7, buf, (GLintptr)ofs, sizeof (bmodel_call_remap[0]) * num_bmodel_calls);
	R_Backend_Dispatch ((unsigned)((num_bmodel_calls + 63) / 64), 1u, 1u);
	R_Backend_MemoryBarrier (R_BACKEND_BARRIER_COMMAND);

	GL_UseProgram (bmodel_batch_program);
	if (R_IsWorldLightingProgram (bmodel_batch_program))
		R_Shadow_ApplyWorldReceiverUniforms (bmodel_batch_program);
	else if (R_IsWorldShadowCasterProgram (bmodel_batch_program))
		R_Shadow_ApplyWorldCasterUniforms (bmodel_batch_program);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, cmdbuf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, pos));
	GL_VertexAttribPointerFunc (1, 4, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, st));
	GL_VertexAttribPointerFunc (2, 1, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, lmofs));
	GL_VertexAttribIPointerFunc (3, 4, GL_UNSIGNED_BYTE, sizeof (glvert_t), (void *) offsetof (glvert_t, styles));
	GL_VertexAttribPointerFunc (4, 3, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, normal));
	GL_VertexAttribPointerFunc (5, 4, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, tangent));
	GL_VertexAttribPointerFunc (6, 3, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, lightgrid));
	GL_VertexAttribPointerFunc (7, 1, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, skyvisibility));

	if (gl_bindless_able)
	{
		GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_calls.bindless.params, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls, &buf, &ofs);
		GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls);
		R_Backend_MultiDrawIndexedIndirect (R_BACKEND_PRIMITIVE_TRIANGLES, R_BACKEND_INDEX_TYPE_UINT32, (intptr_t)dstcmdofs, num_bmodel_calls, sizeof (bmodel_draw_indirect_t));
	}
	else
	{
		int i;

		GL_Upload (GL_SHADER_STORAGE_BUFFER, &bmodel_calls.bound.params, sizeof (bmodel_calls.bound.params[0]) * num_bmodel_calls, &buf, &ofs);
		GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bound.params[0]) * num_bmodel_calls);

		for (i = 0; i < num_bmodel_calls; i++)
		{
			GL_Uniform1iFunc (0, i);
			GL_BindTextures (0, 2, bmodel_calls.bound.textures[i]);
			GL_Bind (GL_TEXTURE4, bmodel_calls.bound.textures[i][BMODEL_TEX_SLOT_EMISSIVE]);
			GL_Bind (GL_TEXTURE5, bmodel_calls.bound.textures[i][BMODEL_TEX_SLOT_NORMAL]);
			GL_Bind (GL_TEXTURE6, bmodel_calls.bound.textures[i][BMODEL_TEX_SLOT_SPECULAR_OR_ORM]);
			R_Backend_DrawIndexedIndirect (R_BACKEND_PRIMITIVE_TRIANGLES, R_BACKEND_INDEX_TYPE_UINT32, (intptr_t)(dstcmdofs + i * sizeof (bmodel_draw_indirect_t)));
		}
	}

	num_bmodel_calls = 0;
}

#define CALLFLAG_EMISSIVE        (1u << 3)
#define CALLFLAG_ALPHA_TEST      (1u << 4)
#define CALLFLAG_NOLIGHTMAP      (1u << 2)
#define CALLFLAG_GODRAYS_LIGHT   (1u << 5)
#define CALLFLAG_GODRAYS_EMISSIVE (1u << 6)
#define CALLFLAG_MAT_BLOOM       (1u << 7)
#define CALLFLAG_MAT_EMISSIVE    (1u << 8)
#define CALLFLAG_MAT_GODRAY      (1u << 9)
#define CALLFLAG_MAT_TRANS       (1u << 10)
#define CALLFLAG_MAT_SKY         (1u << 11)
#define CALLFLAG_MAT_HAS_SHADER  (1u << 12)

static unsigned R_StageOutputCallFlags (const material_stage_t *stage)
{
	unsigned flags = 0u;

	if (!stage)
		return flags;
	if (stage->outputs & MAT_STAGE_OUT_BLOOM)
		flags |= CALLFLAG_MAT_BLOOM;
	if (stage->outputs & MAT_STAGE_OUT_EMISSIVE)
		flags |= CALLFLAG_MAT_EMISSIVE;
	if (stage->outputs & MAT_STAGE_OUT_GODRAY_SOURCE)
		flags |= CALLFLAG_MAT_GODRAY;

	return flags;
}

qboolean R_TextureEmitsGodrays (texture_t *t)
{
	if (!t)
		return false;

	if (r_materials.value <= 0.f || !t->material)
		return false;

	if (t->material->stages)
	{
		size_t stage_count = VEC_SIZE (t->material->stages);

		for (size_t i = 0; i < stage_count; ++i)
		{
			const material_stage_t *stage = &t->material->stages[i];
			if (stage->outputs & MAT_STAGE_OUT_GODRAY_SOURCE)
				return true;
		}
	}

	return false;
}

qboolean R_SurfaceEmitsGodrays (msurface_t *s)
{
	if (!s)
		return false;

	if ((s->flags & SURF_DRAWSKY) && r_godrays.value > 0.f)
	{
		if (cl.worldmodel && s->texinfo && s->texinfo->texnum >= 0 && s->texinfo->texnum < cl.worldmodel->numtextures)
		{
			texture_t *t = cl.worldmodel->textures[s->texinfo->texnum];
			return R_TextureEmitsGodrays (t);
		}
		return false;
	}

	if (cl.worldmodel && s->texinfo && s->texinfo->texnum >= 0 && s->texinfo->texnum < cl.worldmodel->numtextures)
	{
		texture_t *t = cl.worldmodel->textures[s->texinfo->texnum];
		return R_TextureEmitsGodrays (t);
	}

	return false;
}

/*
=============
R_AddBModelCall
=============
*/
static void R_AddBModelCall (int index, int first_instance, int num_instances, texture_t *t, qboolean zfix,
	float polygon_offset_factor, float polygon_offset_units, float alpha_override, unsigned extra_flags,
	qboolean force_fullbright)
{
	GLuint		flags;
	float		alpha;
	gltexture_t	*tx, *fb, *em;

	if (num_bmodel_calls == MAX_BMODEL_DRAWS)
		R_FlushBModelCalls ();

	if (t)
	{
		tx = R_SanitizeGLTexture (t->gltexture, t->name, "base");
		fb = R_SanitizeGLTexture (t->fullbright, t->name, "fullbright");
		em = R_SanitizeGLTexture (t->emissive, t->name, "emissive");
		if (r_lightmap_cheatsafe)
			tx = fb = em = NULL;
		if (!gl_fullbrights.value && t->type != TEXTYPE_SKY && !force_fullbright)
			fb = NULL;
	}
	else
	{
		tx = fb = whitetexture;
		em = NULL;
	}

	if (!gl_zfix.value || map_checks.value)
		zfix = 0;

	flags = zfix | ((fb != NULL) << 1) | (r_fullbright_cheatsafe ? CALLFLAG_NOLIGHTMAP : 0u) | extra_flags;
	if (em != NULL && (extra_flags & CALLFLAG_MAT_EMISSIVE))
		flags |= CALLFLAG_EMISSIVE;
	if (t && t->type == TEXTYPE_CUTOUT)
		flags |= CALLFLAG_ALPHA_TEST;
	
	if (t)
	{
		if (alpha_override >= 0.f)
			alpha = alpha_override;
		else
			alpha = GL_WaterAlphaForTextureType (t->type);
	}
	else
	{
		alpha = 1.f;
	}

	if (gl_bindless_able)
	{
		bmodel_bindless_gpu_call_t *call = &bmodel_calls.bindless.params[num_bmodel_calls];
		call->flags = flags;
		call->tcgen = TCGEN_BASE;
		call->alpha = alpha;
		call->spec_mode = 0;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->specular[0] = 0.4f;
		call->specular[1] = 16.f;
		call->stage_color[0] = 1.f;
		call->stage_color[1] = 1.f;
		call->stage_color[2] = 1.f;
		call->stage_color[3] = 1.f;
		call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
		call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
		call->emissive = em ? em->bindless_handle : blacktexture->bindless_handle;
		call->normal_map = greytexture->bindless_handle;
		call->specular_map = whitetexture->bindless_handle;
		call->padding[0] = 0;
		call->padding[1] = 0;
	}
	else
	{
		bmodel_bound_gpu_call_t *call = &bmodel_calls.bound.params[num_bmodel_calls];
		gltexture_t **textures = bmodel_calls.bound.textures[num_bmodel_calls];
		call->flags = flags;
		call->tcgen = TCGEN_BASE;
		call->alpha = alpha;
		call->spec_mode = 0;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->specular[0] = 0.4f;
		call->specular[1] = 16.f;
		call->stage_color[0] = 1.f;
		call->stage_color[1] = 1.f;
		call->stage_color[2] = 1.f;
		call->stage_color[3] = 1.f;
		call->baseinstance = first_instance;
		call->padding[0] = 0;
		call->padding[1] = 0;
		call->padding[2] = 0;
		textures[BMODEL_TEX_SLOT_BASE] = tx ? tx : greytexture;
		textures[BMODEL_TEX_SLOT_FULLBRIGHT] = fb ? fb : blacktexture;
		textures[BMODEL_TEX_SLOT_EMISSIVE] = em ? em : blacktexture;
		textures[BMODEL_TEX_SLOT_NORMAL] = greytexture;
		textures[BMODEL_TEX_SLOT_SPECULAR_OR_ORM] = whitetexture;
	}

	SDL_assert (num_instances > 0);
	SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
	bmodel_call_remap[num_bmodel_calls].src = index;
	bmodel_call_remap[num_bmodel_calls].inst = first_instance * MAX_BMODEL_INSTANCES + (num_instances - 1);

	++num_bmodel_calls;
}

static void R_AddBModelCallWithTextures (int index, int first_instance, int num_instances, gltexture_t *tx, gltexture_t *fb, gltexture_t *em, gltexture_t *nm, gltexture_t *sm,
	tcgen_mode_t tcgen, qboolean zfix, float polygon_offset_factor, float polygon_offset_units, float alpha_override, unsigned extra_flags,
	const vec4_t stage_color, float spec_intensity, float spec_exponent, int spec_mode)
{
	GLuint flags;
	float alpha;

	if (num_bmodel_calls == MAX_BMODEL_DRAWS)
		R_FlushBModelCalls ();

	tx = R_SanitizeGLTexture (tx, "shaderstage", "base");
	fb = R_SanitizeGLTexture (fb, "shaderstage", "fullbright");
	em = R_SanitizeGLTexture (em, "shaderstage", "emissive");
	nm = R_SanitizeGLTexture (nm, "shaderstage", "normal");
	sm = R_SanitizeGLTexture (sm, "shaderstage", "specular");
	if (!sm)
		spec_mode = 0;

	flags = zfix | ((fb != NULL) << 1) | (r_fullbright_cheatsafe ? CALLFLAG_NOLIGHTMAP : 0u) | extra_flags;
	if (em != NULL && (extra_flags & CALLFLAG_MAT_EMISSIVE))
		flags |= CALLFLAG_EMISSIVE;

	alpha = (alpha_override >= 0.f) ? alpha_override : 1.f;

	if (gl_bindless_able)
	{
		bmodel_bindless_gpu_call_t *call = &bmodel_calls.bindless.params[num_bmodel_calls];
		call->flags = flags;
		call->tcgen = tcgen;
		call->alpha = alpha;
		call->spec_mode = spec_mode;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->specular[0] = spec_intensity;
		call->specular[1] = spec_exponent;
		call->stage_color[0] = stage_color ? stage_color[0] : 1.f;
		call->stage_color[1] = stage_color ? stage_color[1] : 1.f;
		call->stage_color[2] = stage_color ? stage_color[2] : 1.f;
		call->stage_color[3] = stage_color ? stage_color[3] : 1.f;
		call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
		call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
		call->emissive = em ? em->bindless_handle : blacktexture->bindless_handle;
		call->normal_map = nm ? nm->bindless_handle : greytexture->bindless_handle;
		call->specular_map = sm ? sm->bindless_handle : whitetexture->bindless_handle;
		call->padding[0] = 0;
		call->padding[1] = 0;
	}
	else
	{
		bmodel_bound_gpu_call_t *call = &bmodel_calls.bound.params[num_bmodel_calls];
		gltexture_t **textures = bmodel_calls.bound.textures[num_bmodel_calls];
		call->flags = flags;
		call->tcgen = tcgen;
		call->alpha = alpha;
		call->spec_mode = spec_mode;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->specular[0] = spec_intensity;
		call->specular[1] = spec_exponent;
		call->stage_color[0] = stage_color ? stage_color[0] : 1.f;
		call->stage_color[1] = stage_color ? stage_color[1] : 1.f;
		call->stage_color[2] = stage_color ? stage_color[2] : 1.f;
		call->stage_color[3] = stage_color ? stage_color[3] : 1.f;
		call->baseinstance = first_instance;
		call->padding[0] = 0;
		call->padding[1] = 0;
		call->padding[2] = 0;
		textures[BMODEL_TEX_SLOT_BASE] = tx ? tx : greytexture;
		textures[BMODEL_TEX_SLOT_FULLBRIGHT] = fb ? fb : blacktexture;
		textures[BMODEL_TEX_SLOT_EMISSIVE] = em ? em : blacktexture;
		textures[BMODEL_TEX_SLOT_NORMAL] = nm ? nm : greytexture;
		textures[BMODEL_TEX_SLOT_SPECULAR_OR_ORM] = sm ? sm : whitetexture;
	}

	SDL_assert (num_instances > 0);
	SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
	bmodel_call_remap[num_bmodel_calls].src = index;
	bmodel_call_remap[num_bmodel_calls].inst = first_instance * MAX_BMODEL_INSTANCES + (num_instances - 1);

	++num_bmodel_calls;
}

static GLenum R_MapDepthFunc (mat_depthfunc_t depth_func)
{
	switch (depth_func)
	{
	case MAT_DEPTHFUNC_LESS:
		return gl_clipcontrol_able ? GL_GREATER : GL_LESS;
	case MAT_DEPTHFUNC_EQUAL:
		return GL_EQUAL;
	case MAT_DEPTHFUNC_GREATER:
		return gl_clipcontrol_able ? GL_LESS : GL_GREATER;
	case MAT_DEPTHFUNC_GEQUAL:
		return gl_clipcontrol_able ? GL_LEQUAL : GL_GEQUAL;
	case MAT_DEPTHFUNC_ALWAYS:
		return GL_ALWAYS;
	case MAT_DEPTHFUNC_NEVER:
		return GL_NEVER;
	case MAT_DEPTHFUNC_LEQUAL:
	default:
		return gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL;
	}
}

static float R_Clamp01 (float value)
{
	if (value < 0.f)
		return 0.f;
	if (value > 1.f)
		return 1.f;
	return value;
}

static void R_EvalStageColorAlpha (const material_stage_t *stage, float time, vec4_t out)
{
	static qboolean warned_vertex_color = false;
	float wave_value;

	out[0] = 1.f;
	out[1] = 1.f;
	out[2] = 1.f;
	out[3] = 1.f;

	if (!stage)
		return;

	switch (stage->rgbgen)
	{
	case MAT_RGBGEN_VERTEX:
		if (!warned_vertex_color)
		{
			Con_Warning ("Material: rgbGen/alphaGen vertex requested but vertex colors are unavailable; using identity.\n");
			warned_vertex_color = true;
		}
		break;
	case MAT_RGBGEN_CONST:
		out[0] = stage->const_color[0];
		out[1] = stage->const_color[1];
		out[2] = stage->const_color[2];
		break;
	case MAT_RGBGEN_WAVE:
		wave_value = R_Clamp01 (Material_EvalWaveValue (&stage->rgb_wave, time));
		out[0] = wave_value;
		out[1] = wave_value;
		out[2] = wave_value;
		break;
	case MAT_RGBGEN_IDENTITY:
	default:
		break;
	}

	switch (stage->alphagen)
	{
	case MAT_ALPHAGEN_VERTEX:
		if (!warned_vertex_color)
		{
			Con_Warning ("Material: rgbGen/alphaGen vertex requested but vertex colors are unavailable; using identity.\n");
			warned_vertex_color = true;
		}
		break;
	case MAT_ALPHAGEN_CONST:
		out[3] = stage->const_alpha;
		break;
	case MAT_ALPHAGEN_WAVE:
		out[3] = R_Clamp01 (Material_EvalWaveValue (&stage->alpha_wave, time));
		break;
	case MAT_ALPHAGEN_IDENTITY:
	default:
		break;
	}
}

static unsigned R_MapBlendMode (const material_stage_t *stage)
{
	if (!stage)
		return GLS_BLEND_OPAQUE;

	switch (stage->blend_mode)
	{
	case MAT_BLEND_ALPHA:
	case MAT_BLEND_PREMULT:
	case MAT_BLEND_CUSTOM:
		return GLS_BLEND_ALPHA;
	case MAT_BLEND_ADD:
		return GLS_BLEND_ADD;
	case MAT_BLEND_MULT:
		return GLS_BLEND_MULTIPLY;
	case MAT_BLEND_REPLACE:
	default:
		return GLS_BLEND_OPAQUE;
	}
}

static unsigned R_MapCullMode (mat_cull_mode_t cull_mode)
{
	switch (cull_mode)
	{
	case MAT_CULL_FRONT:
		return GLS_CULL_FRONT;
	case MAT_CULL_NONE:
		return GLS_CULL_NONE;
	case MAT_CULL_BACK:
	default:
		return GLS_CULL_BACK;
	}
}

static gltexture_t *R_FindStageTexture (const qmodel_t *model, const material_stage_t *stage, float time)
{
	const char *path = NULL;

	if (!stage)
		return NULL;

	switch (stage->map_type)
	{
	case MAT_MAP_LIGHTMAP:
		return whitetexture;
	case MAT_MAP_WHITE:
		return whitetexture;
	case MAT_MAP_BLACK:
		return blacktexture;
	case MAT_MAP_CLAMPMAP:
	case MAT_MAP_MAP:
	default:
		break;
	}

	path = MaterialStage_GetAnimMapPath ((material_stage_t *)stage, time);
	if (!path || !path[0])
		path = stage->map_path;
	if (!path || !path[0])
		return NULL;

	return TexMgr_FindTexture ((qmodel_t *)model, path);
}

static gltexture_t *R_FindStageNormalTexture (const qmodel_t *model, const material_stage_t *stage)
{
	if (!stage || !stage->normal_map_path || !stage->normal_map_path[0])
		return NULL;
	return TexMgr_FindTexture ((qmodel_t *)model, stage->normal_map_path);
}

static gltexture_t *R_FindStageSpecularTexture (const qmodel_t *model, const material_stage_t *stage, int *out_mode)
{
	if (out_mode)
		*out_mode = 0;
	if (!stage)
		return NULL;
	if (stage->specular_map_path && stage->specular_map_path[0])
	{
		if (out_mode)
			*out_mode = 1; /* dedicated spec map: red = intensity */
		return TexMgr_FindTexture ((qmodel_t *)model, stage->specular_map_path);
	}
	if (stage->orm_map_path && stage->orm_map_path[0])
	{
		if (out_mode)
			*out_mode = 2; /* ORM alpha = roughness */
		return TexMgr_FindTexture ((qmodel_t *)model, stage->orm_map_path);
	}
	return NULL;
}

static qboolean R_StageUsesLightmap (const material_stage_t *stage, size_t stage_index)
{
	if (!stage)
		return false;
	if (stage->map_type == MAT_MAP_LIGHTMAP)
		return true;
	if (stage_index == 0 && stage->blend_mode == MAT_BLEND_REPLACE)
		return true;
	return false;
}

static tcgen_mode_t R_ResolveStageTcGen (const material_stage_t *stage)
{
	if (!stage)
		return TCGEN_BASE;

	switch (stage->tcgen)
	{
	case MAT_TCGEN_ENVIRONMENT:
		return TCGEN_ENVIRONMENT;
	case MAT_TCGEN_LIGHTMAP:
		return TCGEN_LIGHTMAP;
	case MAT_TCGEN_BASE:
	default:
		break;
	}

	if (stage->map_type == MAT_MAP_LIGHTMAP)
		return TCGEN_LIGHTMAP;

	return TCGEN_BASE;
}

static qboolean R_StageIsOpaqueBase (const material_t *material)
{
	const material_stage_t *stage;

	if (!material || !material->stages)
		return false;

	stage = &material->stages[0];
	return material->sort_key == MAT_SORT_OPAQUE
		&& stage->blend_mode == MAT_BLEND_REPLACE
		&& stage->depth_write
		&& stage->depth_func == MAT_DEPTHFUNC_LEQUAL;
}

/*
=============
R_ChooseBModelProgram
=============
*/
static GLuint R_ChooseBModelProgram (qboolean oit, qboolean alphatest)
{
	extern cvar_t r_softemu_lightmap_banding;

	switch (softemu)
	{
	case SOFTEMU_BANDED:
		if (r_softemu_lightmap_banding.value != 0.f)
			return glprogs.world[oit][2][alphatest];
		else
			return glprogs.world[oit][1][alphatest];

	case SOFTEMU_COARSE:
		if (r_softemu_lightmap_banding.value > 0.f)
			return glprogs.world[oit][2][alphatest];
		else
			return glprogs.world[oit][1][alphatest];

	default:
		if (r_softemu_lightmap_banding.value > 0.f)
			return glprogs.world[oit][2][alphatest];
		else
			return glprogs.world[oit][0][alphatest];
	}
}

typedef enum {
        BP_SOLID,
        BP_ALPHATEST,
        BP_SHADOW_SUN,
        BP_SHADOW_DLIGHT,
        BP_GODRAYS,
        BP_SKYLAYERS,
        BP_SKYCUBEMAP,
        BP_SKYSTENCIL,
        BP_SHOWTRIS,
        BP_DLIGHT_SOLID,
        BP_DLIGHT_ALPHA,
} brushpass_t;

static void R_DrawBrushModels_MaterialStages (entity_t **ents, int count, brushpass_t pass, qboolean translucent, mat_sort_key_t sort_key)
{
	int i, j;
	int totalinst, baseinst;
	unsigned current_state = ~0u;
	GLenum current_depth = GL_INVALID_ENUM;
	mat_blend_mode_t current_blend_mode = MAT_BLEND_REPLACE;
	render_blend_factor_t current_blend_src = R_BLEND_FACTOR_ONE;
	render_blend_factor_t current_blend_dst = R_BLEND_FACTOR_ZERO;
	GLuint program;
	GLuint buf;
	GLbyte *ofs;
	textype_t texbegin, texend;
	qboolean oit;

	if (!count || r_materials.value <= 0.f)
		return;

	if (pass != BP_SOLID && pass != BP_ALPHATEST)
		return;

	if (count > countof (bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, (int)countof (bmodel_instances));
		count = countof (bmodel_instances);
	}

	oit = translucent && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT;
	texbegin = (pass == BP_ALPHATEST) ? TEXTYPE_CUTOUT : 0;
	texend = (pass == BP_ALPHATEST) ? (TEXTYPE_CUTOUT + 1) : TEXTYPE_CUTOUT;
	program = R_ChooseBModelProgram (oit, pass == BP_ALPHATEST);

	for (i = 0, totalinst = 0; i < count; i++)
	{
		entity_t *ent = ents[i];
		if (!ent || !Mod_IsKnownModel (ent->model))
			continue;
		if (ent->model->texofs[texend] - ent->model->texofs[texbegin] > 0)
			R_InitBModelInstance (&bmodel_instances[totalinst++], ent);
	}

	if (!totalinst)
		return;

	R_ResetBModelCalls (program);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
	GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);
	GL_Bind (GL_TEXTURE5, greytexture);
	GL_Bind (GL_TEXTURE6, whitetexture);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof (bmodel_instances[0]) * totalinst, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof (bmodel_instances[0]) * totalinst);

	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model;

		if (!e || !Mod_IsKnownModel (e->model))
			continue;
		model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		qboolean isstatic = PTR_IN_RANGE (e, cl_static_entities, cl_static_entities + MAX_STATIC_ENTITIES);
			qboolean zfix = !isworld && !isstatic;
		int numtex = model->texofs[texend] - model->texofs[texbegin];

		if (model->texofs[TEXTYPE_COUNT] < 0
			|| model->texofs[TEXTYPE_COUNT] > model->numtextures
			|| model->texofs[texbegin] < 0
			|| model->texofs[texend] < model->texofs[texbegin]
			|| model->texofs[texend] > model->texofs[TEXTYPE_COUNT])
		{
			Con_DWarning ("R_DrawBrushModels: invalid texofs for %s (%d..%d of %d)\n",
				model->name, model->texofs[texbegin], model->texofs[texend], model->texofs[TEXTYPE_COUNT]);
			continue;
		}

		if (!numtex)
			continue;

		for (numinst = 1; i < count && numinst < MAX_BMODEL_INSTANCES; i++)
		{
			if (!ents[i] || ents[i]->model != model)
				break;
			numinst += (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin]) > 0;
		}

		for (j = model->texofs[texbegin]; j < model->texofs[texend]; j++)
		{
			texture_t *t = R_GetUsedTexture (model, j, NULL);
			const material_t *material;
			unsigned mat_flags = 0u;
			unsigned mat_call_flags = 0u;
			size_t stage_count;
			size_t stage_index;

			if (!t || !R_ValidPtr (t))
				continue;

			if (!t->material)
				continue;

			mat_flags = (r_materials.value > 0.f) ? t->material_flags : 0u;
			if (mat_flags & MATERIAL_FLAG_NODRAW)
				continue;

			material = t->material;
			if (material->sort_key != sort_key)
				continue;

			stage_count = material->stages ? VEC_SIZE (material->stages) : 0;
			if (!stage_count)
				continue;

			if (mat_flags & MATERIAL_FLAG_TRANS)
				mat_call_flags |= CALLFLAG_MAT_TRANS;
			if (mat_flags & MATERIAL_FLAG_SKY)
				mat_call_flags |= CALLFLAG_MAT_SKY;
			mat_call_flags |= CALLFLAG_MAT_HAS_SHADER;

			for (stage_index = 0; stage_index < stage_count; stage_index++)
			{
				const material_stage_t *stage = &material->stages[stage_index];
				gltexture_t *stage_tex;
				gltexture_t *fb = NULL;
				gltexture_t *em = NULL;
				gltexture_t *nm = NULL;
				gltexture_t *sm = NULL;
				vec4_t stage_color;
				unsigned extra_flags = mat_call_flags | R_StageOutputCallFlags (stage);
				unsigned stage_state;
				GLenum depth_func;
				qboolean blend_changed;
				qboolean uses_lightmap;
				qboolean stage_is_base = (stage_index == 0 && R_StageIsOpaqueBase (material));
				float spec_intensity = 0.4f;
				float spec_exponent = 16.f;
				int spec_mode = 0;

				if (stage_is_base)
					continue;

				if (stage->map_type == MAT_MAP_MAP && (!stage->map_path || !stage->map_path[0]) && stage_index == 0)
					stage_tex = t->gltexture;
				else
					stage_tex = R_FindStageTexture (model, stage, cl.time);

				if (!stage_tex)
					continue;

				R_EvalStageColorAlpha (stage, cl.time, stage_color);
				if (stage->outputs & MAT_STAGE_OUT_BLOOM)
				{
					stage_color[0] *= stage->bloom_scale;
					stage_color[1] *= stage->bloom_scale;
					stage_color[2] *= stage->bloom_scale;
				}

				if (stage_index == 0 && stage_tex == t->gltexture)
				{
					fb = t->fullbright;
					em = t->emissive;
					if (!gl_fullbrights.value && t->type != TEXTYPE_SKY)
						fb = NULL;
				}
				nm = R_FindStageNormalTexture (model, stage);
				sm = R_FindStageSpecularTexture (model, stage, &spec_mode);
				spec_intensity = 0.4f * q_max (stage->spec_intensity, 0.f);
				spec_exponent = q_max (1.f, 16.f * stage->spec_power);

				uses_lightmap = R_StageUsesLightmap (stage, stage_index);
				if (!uses_lightmap || !model->lightdata)
					extra_flags |= CALLFLAG_NOLIGHTMAP;
				if (t->type == TEXTYPE_CUTOUT)
					extra_flags |= CALLFLAG_ALPHA_TEST;

				stage_state = GLS_ATTRIBS (8) | R_MapBlendMode (stage) | R_MapCullMode (material->cull_mode);
				if (!stage->depth_write)
					stage_state |= GLS_NO_ZWRITE;

				depth_func = R_MapDepthFunc (stage->depth_func);
				blend_changed = stage->blend_mode != current_blend_mode;
				if (stage->blend_mode == MAT_BLEND_CUSTOM
					&& (stage->blend_src != current_blend_src || stage->blend_dst != current_blend_dst))
					blend_changed = true;

				if (stage_state != current_state || depth_func != current_depth || blend_changed)
				{
					if (num_bmodel_calls)
						R_FlushBModelCalls ();
					R_ResetBModelCalls (program);
					R_Backend_SetPipelineState (stage_state);
					current_state = stage_state;
					glDepthFunc (depth_func);
					current_depth = depth_func;

					if (stage->blend_mode == MAT_BLEND_PREMULT)
						R_Backend_SetBlendFactors (R_BLEND_FACTOR_ONE, R_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
					else if (stage->blend_mode == MAT_BLEND_CUSTOM)
						R_Backend_SetBlendFactors (stage->blend_src, stage->blend_dst);
					current_blend_mode = stage->blend_mode;
					current_blend_src = stage->blend_src;
					current_blend_dst = stage->blend_dst;
				}

				{
					float polygon_offset_factor;
					float polygon_offset_units;
					qboolean use_polygon_offset = zfix || material->polygon_offset;

					R_GetPolygonOffsetValues (material, use_polygon_offset, &polygon_offset_factor, &polygon_offset_units);
					R_AddBModelCallWithTextures (model->firstcmd + j, baseinst, numinst,
						stage_tex, fb, em, nm, sm, R_ResolveStageTcGen (stage),
						use_polygon_offset, polygon_offset_factor, polygon_offset_units, -1.f, extra_flags,
						stage_color, spec_intensity, spec_exponent, spec_mode);
				}
			}
		}

		baseinst += numinst;
	}

	if (num_bmodel_calls)
		R_FlushBModelCalls ();

	glDepthFunc (R_MapDepthFunc (MAT_DEPTHFUNC_LEQUAL));
}

static void R_DrawBrushModels_GodrayStages (entity_t **ents, int count)
{
	int i, j;
	int totalinst, baseinst;
	GLuint buf;
	GLbyte *ofs;
	textype_t texbegin, texend;

	if (!count || r_materials.value <= 0.f || !glprogs.godrays_source)
		return;

	if (count > countof (bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, (int)countof (bmodel_instances));
		count = countof (bmodel_instances);
	}

	texbegin = 0;
	texend = TEXTYPE_COUNT;

	for (i = 0, totalinst = 0; i < count; i++)
	{
		entity_t *ent = ents[i];
		if (!ent || !Mod_IsKnownModel (ent->model))
			continue;
		if (!R_BrushModelHasTextureTables (ent->model))
			continue;
		if (ent->model->texofs[texend] - ent->model->texofs[texbegin] > 0)
			R_InitBModelInstance (&bmodel_instances[totalinst++], ent);
	}

	if (!totalinst)
		return;

	R_ResetBModelCalls (glprogs.godrays_source);
	R_Backend_SetPipelineState (GLS_CULL_BACK | GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_ATTRIBS (8));
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
	GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);
	GL_Bind (GL_TEXTURE5, greytexture);
	GL_Bind (GL_TEXTURE6, whitetexture);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof (bmodel_instances[0]) * totalinst, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof (bmodel_instances[0]) * totalinst);

	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model;

		if (!e || !Mod_IsKnownModel (e->model))
			continue;
		model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		qboolean isstatic = PTR_IN_RANGE (e, cl_static_entities, cl_static_entities + MAX_STATIC_ENTITIES);
		qboolean zfix = !isworld && !isstatic;
		int numtex = model->texofs[texend] - model->texofs[texbegin];

		if (model->texofs[TEXTYPE_COUNT] < 0
			|| model->texofs[TEXTYPE_COUNT] > model->numtextures
			|| model->texofs[texbegin] < 0
			|| model->texofs[texend] < model->texofs[texbegin]
			|| model->texofs[texend] > model->texofs[TEXTYPE_COUNT])
		{
			Con_DWarning ("R_DrawBrushModels_GodrayStages: invalid texofs for %s (%d..%d of %d)\n",
				model->name, model->texofs[texbegin], model->texofs[texend], model->texofs[TEXTYPE_COUNT]);
			continue;
		}

		if (!numtex)
			continue;

		for (numinst = 1; i < count && numinst < MAX_BMODEL_INSTANCES; i++)
		{
			if (!ents[i] || ents[i]->model != model)
				break;
			numinst += (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin]) > 0;
		}

		for (j = model->texofs[texbegin]; j < model->texofs[texend]; j++)
		{
			texture_t *t = R_GetUsedTexture (model, j, NULL);
			const material_t *material;
			unsigned mat_flags = 0u;
			size_t stage_count;

			if (!t || !R_ValidPtr (t))
				continue;

			if (!t->material || !t->material->stages)
				continue;

			mat_flags = (r_materials.value > 0.f) ? t->material_flags : 0u;
			if (mat_flags & MATERIAL_FLAG_NODRAW)
				continue;

			material = t->material;
			stage_count = material->stages ? VEC_SIZE (material->stages) : 0;
			if (!stage_count)
				continue;

			for (size_t stage_index = 0; stage_index < stage_count; stage_index++)
			{
				const material_stage_t *stage = &material->stages[stage_index];
				gltexture_t *stage_tex;
				gltexture_t *fb = NULL;
				gltexture_t *em = NULL;
				vec4_t stage_color;
				unsigned extra_flags = CALLFLAG_MAT_HAS_SHADER;
				qboolean wants_emissive = (stage->outputs & MAT_STAGE_OUT_EMISSIVE) != 0u;
				qboolean wants_godray = (stage->outputs & MAT_STAGE_OUT_GODRAY_SOURCE) != 0u;

				if (!wants_godray)
					continue;

				if (stage->map_type == MAT_MAP_MAP && (!stage->map_path || !stage->map_path[0]) && stage_index == 0)
					stage_tex = t->gltexture;
				else
					stage_tex = R_FindStageTexture (model, stage, cl.time);

				if (!stage_tex)
					continue;

				R_EvalStageColorAlpha (stage, cl.time, stage_color);
				stage_color[0] *= stage->godray_scale;
				stage_color[1] *= stage->godray_scale;
				stage_color[2] *= stage->godray_scale;
				stage_color[3] *= stage->godray_scale;

				if (stage_index == 0 && stage_tex == t->gltexture)
				{
					fb = t->fullbright;
					em = t->emissive;
					if (!gl_fullbrights.value && t->type != TEXTYPE_SKY)
						fb = NULL;
				}

				extra_flags |= CALLFLAG_GODRAYS_LIGHT;

				if (wants_emissive)
				{
					extra_flags |= CALLFLAG_GODRAYS_EMISSIVE;
					extra_flags |= CALLFLAG_MAT_EMISSIVE;
				}

				if (t->type == TEXTYPE_CUTOUT)
					extra_flags |= CALLFLAG_ALPHA_TEST;

				{
					float polygon_offset_factor;
					float polygon_offset_units;
					qboolean use_polygon_offset = zfix || material->polygon_offset;

					R_GetPolygonOffsetValues (material, use_polygon_offset, &polygon_offset_factor, &polygon_offset_units);
					R_AddBModelCallWithTextures (model->firstcmd + j, baseinst, numinst,
						stage_tex, fb, em, R_FindStageNormalTexture (model, stage), NULL, R_ResolveStageTcGen (stage),
						use_polygon_offset, polygon_offset_factor, polygon_offset_units, -1.f, extra_flags,
						stage_color, 0.4f, 16.f, 0);
				}
			}
		}

		baseinst += numinst;
	}

	if (num_bmodel_calls)
		R_FlushBModelCalls ();

	glDepthFunc (R_MapDepthFunc (MAT_DEPTHFUNC_LEQUAL));
}

/*
=============
R_DrawBrushModels_Real
=============
*/
static void R_DrawBrushModels_Real (entity_t **ents, int count, brushpass_t pass, qboolean translucent)
{
        int i, j;
        int totalinst, baseinst;
        unsigned state;
        GLuint program;
        GLuint buf;
        GLbyte *ofs;
        textype_t texbegin, texend;
        qboolean oit;

	/* Dynamic-light receiver contract for brush/world-side categories:
	 *   category              dynamic dlight pass
	 *   ----------------------------------------
	 *   world opaque          yes
	 *   world translucent     no (unsupported)
	 *   water                 no (unsupported)
	 *   decals                no (pass-order excl.)
	 *   particles             no (pass-order excl.)
	 * Notes:
	 * - Alias opaque/translucent are handled in alias paths, not brush paths. */

        if (!count)
                return;

        if (count > countof(bmodel_instances))
        {
                Con_DWarning ("bmodel instance overflow: %d > %d\n", count, (int)countof(bmodel_instances));
                count = countof(bmodel_instances);
        }

        oit = translucent && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT;
        switch (pass)
        {
        default:
        case BP_SOLID:
                texbegin = 0;
                texend = TEXTYPE_CUTOUT;
                program = R_ChooseBModelProgram (oit, false);
                break;
        case BP_ALPHATEST:
                texbegin = TEXTYPE_CUTOUT;
                texend = TEXTYPE_CUTOUT + 1;
                program = R_ChooseBModelProgram (oit, true);
                break;
        case BP_SHADOW_SUN:
                texbegin = 0;
                texend = TEXTYPE_CUTOUT;
                program = glprogs.world_shadow[0];
                break;
        case BP_SHADOW_DLIGHT:
                texbegin = 0;
                texend = TEXTYPE_CUTOUT;
                program = glprogs.world_shadow[1];
                break;
        case BP_GODRAYS:
                texbegin = 0;
                texend = TEXTYPE_COUNT;
                program = glprogs.godrays_source;
                break;
        case BP_SKYLAYERS:
                texbegin = TEXTYPE_SKY;
                texend = TEXTYPE_SKY + 1;
                program = glprogs.skylayers[softemu == SOFTEMU_COARSE];
                break;
        case BP_SKYCUBEMAP:
                texbegin = TEXTYPE_SKY;
                texend = TEXTYPE_SKY + 1;
                program = glprogs.skycubemap[Sky_IsAnimated ()][softemu == SOFTEMU_COARSE];
                break;
        case BP_SKYSTENCIL:
                texbegin = TEXTYPE_SKY;
                texend = TEXTYPE_SKY + 1;
                program = glprogs.skystencil;
                break;
        case BP_SHOWTRIS:
                texbegin = 0;
                texend = TEXTYPE_COUNT;
                program = glprogs.world[0][0][0];
                break;
        case BP_DLIGHT_SOLID:
                /* Intentionally limited to opaque/cutout world surfaces.
                 * Translucent world + water are excluded by contract. */
                texbegin = 0;
                texend = TEXTYPE_CUTOUT;
                program = glprogs.world_dlight[0];
                break;
        case BP_DLIGHT_ALPHA:
                /* Cutout alpha-tested surfaces only; not generic translucency. */
                texbegin = TEXTYPE_CUTOUT;
                texend = TEXTYPE_CUTOUT + 1;
                program = glprogs.world_dlight[1];
                break;
        }

	for (i = 0, totalinst = 0; i < count; i++)
	{
		entity_t *ent = ents[i];
		if (!ent || !Mod_IsKnownModel (ent->model))
			continue;
		if (pass == BP_GODRAYS && !R_BrushModelHasTextureTables (ent->model))
			continue;
		if (ent->model->texofs[texend] - ent->model->texofs[texbegin] > 0)
			R_InitBModelInstance (&bmodel_instances[totalinst++], ent);
	}

        if (!totalinst)
                return;

        state = GLS_CULL_BACK | GLS_ATTRIBS(6);
        if (pass == BP_DLIGHT_SOLID || pass == BP_DLIGHT_ALPHA)
        {
                state |= GLS_BLEND_ADD | GLS_NO_ZWRITE;
        }
        else if (pass == BP_SHADOW_SUN || pass == BP_SHADOW_DLIGHT)
        {
                /* Shadow caster pass: disable face culling so one-sided BSP
                 * geometry still occludes lights from both sides. */
                state &= ~GLS_MASK_CULL;
                state |= GLS_CULL_NONE | GLS_BLEND_OPAQUE;
        }
        else if (pass == BP_GODRAYS)
                state |= GLS_BLEND_ADD | GLS_NO_ZWRITE;
        else if (!translucent)
                state |= GLS_BLEND_OPAQUE;
        else
                state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;

        R_ResetBModelCalls (program);
        R_Backend_SetPipelineState (state);
        GL_UseProgram (program);
if (pass <= BP_ALPHATEST)
{
GL_UseProgram (program);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
	GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);
	GL_Bind (GL_TEXTURE5, greytexture);
	GL_Bind (GL_TEXTURE6, whitetexture);
}
else if (pass == BP_SHADOW_SUN || pass == BP_SHADOW_DLIGHT)
{
	R_Shadow_ApplyWorldCasterUniforms (program);
}
else if (pass == BP_DLIGHT_SOLID || pass == BP_DLIGHT_ALPHA)
{
}
else if (pass == BP_SKYCUBEMAP)
	GL_Bind (GL_TEXTURE2, skybox->cubemap);

        GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * totalinst, &buf, &ofs);
        GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * totalinst);

	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model;

		if (!e || !Mod_IsKnownModel (e->model))
			continue;
		model = e->model;
		if (pass == BP_GODRAYS && !R_BrushModelHasTextureTables (model))
			continue;
		qboolean isworld = (e == &cl_entities[0]);
		qboolean isstatic = PTR_IN_RANGE (e, cl_static_entities, cl_static_entities + MAX_STATIC_ENTITIES);
		qboolean zfix = !isworld && !isstatic;
		int frame = isworld ? 0 : e->frame;
		int numtex = model->texofs[texend] - model->texofs[texbegin];

		if (model->texofs[TEXTYPE_COUNT] < 0
			|| model->texofs[TEXTYPE_COUNT] > model->numtextures
			|| model->texofs[texbegin] < 0
			|| model->texofs[texend] < model->texofs[texbegin]
			|| model->texofs[texend] > model->texofs[TEXTYPE_COUNT])
		{
			Con_DWarning ("R_DrawBrushModels: invalid texofs for %s (%d..%d of %d)\n",
				model->name, model->texofs[texbegin], model->texofs[texend], model->texofs[TEXTYPE_COUNT]);
			continue;
		}

		if (!numtex)
			continue;

		for (numinst = 1; i < count && numinst < MAX_BMODEL_INSTANCES; i++)
		{
			if (!ents[i] || ents[i]->model != model)
				break;
			numinst += (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin]) > 0;
		}

		for (j = model->texofs[texbegin]; j < model->texofs[texend]; j++)
		{
			int texnum = -1;
			texture_t *t = R_GetUsedTexture (model, j, &texnum);
			unsigned extra_flags = 0u;
			qboolean force_fullbright = false;
			unsigned mat_flags = 0u;
			unsigned mat_call_flags = 0u;

			if (!t || !R_ValidPtr (t))
				continue;

			mat_flags = (r_materials.value > 0.f) ? t->material_flags : 0u;
			if (mat_flags & MATERIAL_FLAG_NODRAW)
				continue;

			if (pass == BP_GODRAYS)
			{
				if (r_materials.value <= 0.f || !t->material || !t->material->stages)
					continue;
				continue;
			}

			if (r_materials.value > 0.f && t->material)
			{
				mat_call_flags |= CALLFLAG_MAT_HAS_SHADER;
				if (t->material->stages)
					mat_call_flags |= R_StageOutputCallFlags (&t->material->stages[0]);
			}
			if (mat_flags & MATERIAL_FLAG_TRANS)
				mat_call_flags |= CALLFLAG_MAT_TRANS;
			if (mat_flags & MATERIAL_FLAG_SKY)
				mat_call_flags |= CALLFLAG_MAT_SKY;

			if ((pass == BP_SOLID || pass == BP_ALPHATEST) && r_materials.value > 0.f && t->material && t->material->stages)
			{
				if (!R_StageIsOpaqueBase (t->material))
					continue;
			}

			if (r_materials.value > 0.f && t->material && t->material->polygon_offset)
				zfix = true;

			extra_flags |= mat_call_flags;
			if (!model->lightdata)
				extra_flags |= CALLFLAG_NOLIGHTMAP;
			{
				float polygon_offset_factor;
				float polygon_offset_units;
				const material_t *material = (r_materials.value > 0.f) ? t->material : NULL;

				R_GetPolygonOffsetValues (material, zfix, &polygon_offset_factor, &polygon_offset_units);
				R_AddBModelCall (model->firstcmd + j, baseinst, numinst,
					pass != BP_SHOWTRIS ? R_TextureAnimation (t, frame) : 0,
					zfix, polygon_offset_factor, polygon_offset_units, -1, extra_flags, force_fullbright);
			}
		}
		
		baseinst += numinst;
        }

	GL_UseProgram (program);
	R_FlushBModelCalls ();

	if (pass == BP_SOLID || pass == BP_ALPHATEST)
	{
		static const mat_sort_key_t sort_order[] =
		{
			MAT_SORT_SKY,
			MAT_SORT_OPAQUE,
			MAT_SORT_SEE_THROUGH,
			MAT_SORT_DECAL,
			MAT_SORT_BANNER,
			MAT_SORT_UNDERWATER,
			MAT_SORT_ADDITIVE,
			MAT_SORT_NEAREST
		};
		size_t sort_index;

		for (sort_index = 0; sort_index < countof (sort_order); ++sort_index)
			R_DrawBrushModels_MaterialStages (ents, count, pass, translucent, sort_order[sort_index]);
	}
}


/*
=============
R_EntHasWater
=============
*/
static qboolean R_EntHasWater (entity_t *ent, qboolean translucent)
{
	int i;

	if (!ent || !Mod_IsKnownModel (ent->model))
		return false;

	for (i = TEXTYPE_FIRSTLIQUID; i < TEXTYPE_LASTLIQUID+1; i++)
	{
		int numtex = ent->model->texofs[i+1] - ent->model->texofs[i];
		if (numtex && (GL_WaterAlphaForEntityTextureType (ent, (textype_t)i) < 1.f) == translucent)
			return true;
	}
	return false;
}

static void R_Water_SetTimeUniform (GLuint program, float shader_time)
{
	GLint loc;

	if (!program)
		return;

	/* Teleport FBM uses u_time; water/world variants read Time from FrameDataUBO. */
	loc = GL_GetUniformLocationFunc ? GL_GetUniformLocationFunc (program, "u_time") : -1;
	if (loc >= 0)
		GL_Uniform1fFunc (loc, shader_time);
}

/*
=============
R_DrawBrushModels_Water
=============
*/
void R_DrawBrushModels_Water (entity_t **ents, int count, qboolean translucent)
{
	int i, j;
	int totalinst, baseinst;
	unsigned state;
	GLuint buf, program, water_program, teleport_program;
	GLuint water_program_fallback, teleport_program_fallback;
	GLbyte *ofs;
	qboolean oit;
	double shader_time;

	if (count > countof(bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, (int)countof(bmodel_instances));
		count = countof(bmodel_instances);
	}

	// fill instance data
	for (i = 0, totalinst = 0; i < count; i++)
		if (R_EntHasWater (ents[i], translucent))
			R_InitBModelInstance (&bmodel_instances[totalinst++], ents[i]);

	if (!totalinst)
		return;

	GL_BeginGroup (translucent ? "Water (translucent)" : "Water (opaque)");

	// setup state
        state = GLS_CULL_BACK | GLS_ATTRIBS(6);
	if (translucent)
		state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;
	else
		state |= GLS_BLEND_OPAQUE;

	oit = translucent && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT;
	if (cl.worldmodel->haslitwater && r_litwater.value)
	{
		water_program = glprogs.world[oit][q_max(0, (int)softemu - 1)][WORLDSHADER_WATER];
		water_program_fallback = glprogs.world[0][q_max(0, (int)softemu - 1)][WORLDSHADER_WATER];
	}
	else
	{
		water_program = glprogs.water[oit][softemu == SOFTEMU_COARSE];
		water_program_fallback = glprogs.water[0][softemu == SOFTEMU_COARSE];
	}
	teleport_program = glprogs.teleport[oit][softemu == SOFTEMU_COARSE];
	teleport_program_fallback = glprogs.teleport[0][softemu == SOFTEMU_COARSE];

	/* Some runtime combinations may miss OIT shader variants.
	 * Fall back to the non-OIT water shaders instead of binding program 0. */
	if (!water_program && oit)
		water_program = water_program_fallback;
	if (!teleport_program && oit)
		teleport_program = teleport_program_fallback;
	if (!water_program || !teleport_program)
	{
		Con_DWarning ("R_DrawBrushModels_Water: missing shader program(s) water=%u teleport=%u (oit=%d)\n",
			(unsigned)water_program, (unsigned)teleport_program, oit ? 1 : 0);
		GL_EndGroup ();
		return;
	}
	program = water_program;
	shader_time = cl.time;

	R_ResetBModelCalls (program);
	GL_UseProgram (program);
	R_Water_SetTimeUniform (program, (float)shader_time);
	R_Backend_SetPipelineState (state);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
	GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);
	GL_Bind (GL_TEXTURE5, greytexture);
	GL_Bind (GL_TEXTURE6, whitetexture);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * totalinst, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * totalinst);

	// generate drawcalls
	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		if (!e || !e->model)
			continue;
		qmodel_t *model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		int frame = isworld ? 0 : e->frame;

		if (!R_EntHasWater (e, translucent))
			continue;

		for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
			numinst += R_EntHasWater (ents[i], translucent);

		for (j = model->texofs[TEXTYPE_FIRSTLIQUID]; j < model->texofs[TEXTYPE_LASTLIQUID+1]; j++)
		{
			texture_t *t = R_GetUsedTexture (model, j, NULL);
			unsigned extra_flags = 0u;
			unsigned mat_flags = 0u;
			GLuint target_program;

			if (!t)
				continue;
			if (r_materials.value > 0.f && (t->material_flags & MATERIAL_FLAG_NODRAW))
				continue;

			mat_flags = (r_materials.value > 0.f) ? t->material_flags : 0u;
			if (r_materials.value > 0.f && t->material)
				extra_flags |= CALLFLAG_MAT_HAS_SHADER;
			if (r_materials.value > 0.f && t->material && t->material->stages)
				extra_flags |= R_StageOutputCallFlags (&t->material->stages[0]);
			if (mat_flags & MATERIAL_FLAG_TRANS)
				extra_flags |= CALLFLAG_MAT_TRANS;
			if (mat_flags & MATERIAL_FLAG_SKY)
				extra_flags |= CALLFLAG_MAT_SKY;

			float alpha = GL_WaterAlphaForEntityTextureType (e, t->type);
			if ((alpha < 1.f) != translucent)
				continue;

			target_program = (t->type == TEXTYPE_TELE) ? teleport_program : water_program;
			if (target_program != program)
			{
				R_FlushBModelCalls ();
				program = target_program;
				R_ResetBModelCalls (program);
				GL_UseProgram (program);
				R_Water_SetTimeUniform (program, (float)shader_time);
			}
			{
				float polygon_offset_factor;
				float polygon_offset_units;
				qboolean zfix = !isworld;
				const material_t *material = (r_materials.value > 0.f) ? t->material : NULL;

				R_GetPolygonOffsetValues (material, zfix, &polygon_offset_factor, &polygon_offset_units);
				R_AddBModelCall (model->firstcmd + j, baseinst, numinst,
					R_TextureAnimation (t, frame),
					zfix, polygon_offset_factor, polygon_offset_units, alpha, extra_flags, false);
			}
		}

		baseinst += numinst;
	}

	R_FlushBModelCalls ();

	GL_EndGroup ();
}

/*
=============
R_GetBModelAlphaPasses
=============
*/
static uint32_t R_GetBModelAlphaPasses (const entity_t *ent)
{
	const qmodel_t *mod = ent->model;
	uint32_t mask = 0;

	if (mod->texofs[TEXTYPE_CUTOUT] != mod->texofs[TEXTYPE_DEFAULT])
		mask |= (1 << BP_SOLID);
	if (mod->texofs[TEXTYPE_SKY] != mod->texofs[TEXTYPE_CUTOUT])
		mask |= (1 << BP_ALPHATEST);

	return mask;
}

/*
=============
R_CanMergeBModelAlphaPasses
=============
*/
static qboolean R_CanMergeBModelAlphaPasses (uint32_t mask_a, uint32_t mask_b)
{
	COMPILE_TIME_ASSERT (check_bit_0, BP_SOLID == 0);
	COMPILE_TIME_ASSERT (check_bit_1, BP_ALPHATEST == 1);

	enum
	{
		#define ALLOW_MERGE(a, b) (1 << ((a)|((b)<<2)))

		MERGE_LUT =
			ALLOW_MERGE (0, 0) |
			ALLOW_MERGE (0, 1) |
			ALLOW_MERGE (0, 2) |
			ALLOW_MERGE (0, 3) |

			ALLOW_MERGE (1, 0) |
			ALLOW_MERGE (1, 1) |
			ALLOW_MERGE (1, 2) |
			ALLOW_MERGE (1, 3) |

			ALLOW_MERGE (2, 0) |
			ALLOW_MERGE (2, 2) |

			ALLOW_MERGE (3, 0) |
			ALLOW_MERGE (3, 2)
		,

		#undef ALLOW_MERGE
	};

	return (MERGE_LUT & (1 << (mask_a | (mask_b << 2)))) != 0;
}

/*
=============
R_DrawBrushModels
=============
*/
void R_DrawBrushModels (entity_t **ents, int count)
{
        qboolean translucent;
        if (!count)
                return;
	translucent = (ents[0] != &cl_entities[0]) && !ENTALPHA_OPAQUE (ents[0]->alpha);
	if (!translucent || R_GetEffectiveAlphaMode () == ALPHAMODE_OIT)
	{
		R_DrawBrushModels_Real (ents, count, BP_SOLID, translucent);
		R_DrawBrushModels_Real (ents, count, BP_ALPHATEST, translucent);
	}
	else
	{
		int i, j;
		for (i = 0; i < count; /**/)
		{
			uint32_t mask = R_GetBModelAlphaPasses (ents[i]);
			if (!mask)
			{
				i++;
				continue;
			}
			for (j = i + 1; j < count; j++)
			{
				uint32_t nextmask = R_GetBModelAlphaPasses (ents[j]);
				if (!R_CanMergeBModelAlphaPasses (mask, nextmask))
					break;
				mask |= nextmask;
			}
			if (mask & (1 << BP_SOLID))
				R_DrawBrushModels_Real (ents + i, j - i, BP_SOLID, true);
			if (mask & (1 << BP_ALPHATEST))
				R_DrawBrushModels_Real (ents + i, j - i, BP_ALPHATEST, true);
			i = j;
		}
	}
}

void R_DrawBrushModels_DLights (entity_t **ents, int count)
{
        if (!count)
                return;

        R_DrawBrushModels_Real (ents, count, BP_DLIGHT_SOLID, false);
        R_DrawBrushModels_Real (ents, count, BP_DLIGHT_ALPHA, false);
}

void R_DrawBrushModels_Shadow (entity_t **ents, int count, qboolean dlight)
{
	if (!count)
		return;

	R_DrawBrushModels_Real (ents, count, dlight ? BP_SHADOW_DLIGHT : BP_SHADOW_SUN, false);
}

/*
=============
R_DrawBrushModels_Godrays
=============
*/
void R_DrawBrushModels_Godrays (entity_t **ents, int count)
{
	if (!count || !glprogs.godrays_source)
		return;

	R_DrawBrushModels_GodrayStages (ents, count);
}


/*
=============
R_DrawBrushModels_SkyLayers
=============
*/
void R_DrawBrushModels_SkyLayers (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SKYLAYERS, false);
}

/*
=============
R_DrawBrushModels_SkyCubemap
=============
*/
void R_DrawBrushModels_SkyCubemap (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SKYCUBEMAP, false);
}

/*
=============
R_DrawBrushModels_SkyStencil
=============
*/
void R_DrawBrushModels_SkyStencil (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SKYSTENCIL, false);
}


/*
=============
R_DrawBrushModels_ShowTris
=============
*/
void R_DrawBrushModels_ShowTris (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SHOWTRIS, false);
}

