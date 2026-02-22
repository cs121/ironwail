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
#include "mat_shader.h"

extern cvar_t gl_fullbrights, r_oldskyleaf, r_showtris; //johnfitz
extern cvar_t r_godrays_emit_sky;
extern cvar_t r_godrays_emit_emissive;
extern cvar_t r_godrays_emit_lighttex;
extern cvar_t r_godrays_lighttex_name_match;
extern cvar_t r_godray_sky_enable;
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_oit;

extern gltexture_t *lightmap_texture;
extern gltexture_t *lightmap_dir_texture;
extern cvar_t r_lightingdir;
extern cvar_t r_dlight_mode;

extern cvar_t r_shadows;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadow_bias;
extern cvar_t r_shadow_normalbias;
extern cvar_t r_shadow_pcf;
extern cvar_t r_shadow_pcf_taps;
extern cvar_t r_shadow_debug;
extern cvar_t r_shadow_dlights;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_dlight_pcf_taps;

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
	GLuint		padding[3];
} gpumark_frame_t;

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

static inline qboolean R_ValidPtr (const void *ptr);

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
	static int last_bad_ptr_frame = -1;
	static int last_bad_texnum_frame = -1;
	texture_t *tex;

	if (!model || !model->usedtextures || !model->textures)
		return NULL;
	if (!R_ValidPtr (model->usedtextures) || !R_ValidPtr (model->textures))
	{
		if (r_framecount != last_warn_frame)
		{
			last_warn_frame = r_framecount;
			Con_DWarning ("R_GetUsedTexture: invalid texture table pointers on %s (used=%p textures=%p)\n",
				model->name, (void *)model->usedtextures, (void *)model->textures);
		}
		return NULL;
	}

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
	if (!tex)
		return NULL;

	if (!R_ValidPtr (tex))
	{
		if (r_framecount != last_bad_ptr_frame)
		{
			last_bad_ptr_frame = r_framecount;
			Con_DWarning ("R_GetUsedTexture: invalid texture pointer on %s (used_index=%d texnum=%d ptr=%p)\n",
				model->name, used_index, texnum, (void *)tex);
		}
		return NULL;
	}

	return tex;
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
static void R_MarkVisSurfaces (byte* vis)
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
		frame.frustum[i][0] = frustum[i].normal[0];
		frame.frustum[i][1] = frustum[i].normal[1];
		frame.frustum[i][2] = frustum[i].normal[2];
		frame.frustum[i][3] = frustum[i].dist;
	}
	frame.vieworg[0] = r_refdef.vieworg[0];
	frame.vieworg[1] = r_refdef.vieworg[1];
	frame.vieworg[2] = r_refdef.vieworg[2];
	frame.oldskyleaf = r_oldskyleaf.value != 0.f;
	frame.framecount = r_framecount;

	COMPILE_TIME_ASSERT (vis_alignment_must_be_power_of_2, (VIS_ALIGN & (VIS_ALIGN - 1)) == 0);
	COMPILE_TIME_ASSERT (vis_alignment_must_be_multiple_of_uint, (VIS_ALIGN & 3) == 0);
	vissize = (vissize + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK; // round up

	GL_UseProgram (glprogs.clear_indirect);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_indirect_buffer, 0, cl.worldmodel->texofs[TEXTYPE_COUNT] * sizeof(bmodel_draw_indirect_t));
	GL_DispatchComputeFunc ((cl.worldmodel->texofs[TEXTYPE_COUNT] + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	GL_UseProgram (glprogs.cull_mark);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_ibo, 0, gl_bmodel_ibo_size);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, vis, vissize, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, buf, (GLintptr)ofs, vissize);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, gl_bmodel_marksurf_buffer, 0, gl_bmodel_marksurf_buffer_size);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_surf_buffer, 0, cl.worldmodel->numsurfaces * sizeof(bmodel_gpu_surf_t));
	GL_Upload (GL_UNIFORM_BUFFER, &frame, sizeof(frame), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 1, buf, (GLintptr)ofs, sizeof(frame));

	GL_DispatchComputeFunc ((nummark + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);

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

	R_MarkVisSurfaces (vis);
	R_AddStaticModels (vis);
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
	float		padding[3];
} bmodel_gpu_instance_t;

typedef struct bmodel_bindless_gpu_call_s {
	GLuint		flags;
	GLuint		tcgen;
	GLfloat		alpha;
	GLfloat		_pad0;
	GLfloat		polygon_offset[2];
	GLfloat		_pad1[2];
	GLfloat		stage_color[4];
	GLuint64	texture;
	GLuint64	fullbright;
	GLuint64	emissive;
	GLuint64	_pad2;
} bmodel_bindless_gpu_call_t;

typedef struct bmodel_bound_gpu_call_s {
	GLuint		flags;
	GLuint		tcgen;
	GLfloat		alpha;
	GLfloat		_pad0;
	GLfloat		polygon_offset[2];
	GLfloat		_pad1[2];
	GLfloat		stage_color[4];
	GLint		baseinstance;
	GLint		padding[3];
} bmodel_bound_gpu_call_t;

typedef struct bmodel_gpu_call_remap_s {
	GLuint		src;
	GLuint		inst;
} bmodel_gpu_call_remap_t;

static bmodel_gpu_instance_t		bmodel_instances[MAX_VISEDICTS + 1]; // +1 for worldspawn
static union {
	struct {
		bmodel_bindless_gpu_call_t	params[MAX_BMODEL_DRAWS];
	} bindless;
	struct {
		bmodel_bound_gpu_call_t		params[MAX_BMODEL_DRAWS];
		gltexture_t					*textures[MAX_BMODEL_DRAWS][3];
	} bound;
} bmodel_calls;
static bmodel_gpu_call_remap_t		bmodel_call_remap[MAX_BMODEL_DRAWS];
static int							num_bmodel_calls;
static GLuint						bmodel_batch_program;

static void R_GetPolygonOffsetValues (const shader_material_t *material, qboolean use_offset,
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
        memset (&inst->padding, 0, sizeof(inst->padding));
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
	GL_DispatchComputeFunc ((num_bmodel_calls + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT);

	GL_UseProgram (bmodel_batch_program);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, cmdbuf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, pos));
        GL_VertexAttribPointerFunc (1, 4, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, st));
        GL_VertexAttribPointerFunc (2, 1, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, lmofs));
        GL_VertexAttribIPointerFunc (3, 4, GL_UNSIGNED_BYTE, sizeof (glvert_t), (void *) offsetof (glvert_t, styles));
        GL_VertexAttribPointerFunc (4, 3, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, normal));
        GL_VertexAttribPointerFunc (5, 3, GL_FLOAT, GL_FALSE, sizeof (glvert_t), (void *) offsetof (glvert_t, lightgrid));

	if (gl_bindless_able)
	{
		GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_calls.bindless.params, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls, &buf, &ofs);
		GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls);
		GL_MultiDrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, (const void *)dstcmdofs, num_bmodel_calls, sizeof (bmodel_draw_indirect_t));
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
			GL_Bind (GL_TEXTURE4, bmodel_calls.bound.textures[i][2]);
			GL_DrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, (const byte *)(dstcmdofs + i * sizeof (bmodel_draw_indirect_t)));
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

static unsigned R_StageOutputCallFlags (const mat_shader_stage_t *stage)
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

	if (!R_ValidPtr (t))
		return false;

	if (r_shaders.value <= 0.f || !t->shader)
		return false;

	if (t->shader->stages)
	{
		size_t stage_count = VEC_SIZE (t->shader->stages);
		qboolean has_godray = false;
		qboolean has_emissive = false;

		for (size_t i = 0; i < stage_count; ++i)
		{
			const mat_shader_stage_t *stage = &t->shader->stages[i];
			if (stage->outputs & MAT_STAGE_OUT_GODRAY_SOURCE)
				has_godray = true;
			if (stage->outputs & MAT_STAGE_OUT_EMISSIVE)
				has_emissive = true;
		}

		if (has_godray && r_godrays_emit_lighttex.value > 0.f)
			return true;
		if (has_godray && has_emissive && r_godrays_emit_emissive.value > 0.f)
			return true;
	}

	return false;
}

qboolean R_SurfaceEmitsGodrays (msurface_t *s)
{
	if (!s)
		return false;

	if ((s->flags & SURF_DRAWSKY) && r_godrays_emit_sky.value > 0.f && r_godray_sky_enable.value > 0.f)
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
		call->_pad0 = 0.f;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->_pad1[0] = 0.f;
		call->_pad1[1] = 0.f;
		call->stage_color[0] = 1.f;
		call->stage_color[1] = 1.f;
		call->stage_color[2] = 1.f;
		call->stage_color[3] = 1.f;
		call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
		call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
		call->emissive = em ? em->bindless_handle : blacktexture->bindless_handle;
		call->_pad2 = 0;
	}
	else
	{
		bmodel_bound_gpu_call_t *call = &bmodel_calls.bound.params[num_bmodel_calls];
		gltexture_t **textures = bmodel_calls.bound.textures[num_bmodel_calls];
		call->flags = flags;
		call->tcgen = TCGEN_BASE;
		call->alpha = alpha;
		call->_pad0 = 0.f;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->_pad1[0] = 0.f;
		call->_pad1[1] = 0.f;
		call->stage_color[0] = 1.f;
		call->stage_color[1] = 1.f;
		call->stage_color[2] = 1.f;
		call->stage_color[3] = 1.f;
		call->baseinstance = first_instance;
		call->padding[0] = 0;
		call->padding[1] = 0;
		call->padding[2] = 0;
		textures[0] = tx ? tx : greytexture;
		textures[1] = fb ? fb : blacktexture;
		textures[2] = em ? em : blacktexture;
	}

	SDL_assert (num_instances > 0);
	SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
	bmodel_call_remap[num_bmodel_calls].src = index;
	bmodel_call_remap[num_bmodel_calls].inst = first_instance * MAX_BMODEL_INSTANCES + (num_instances - 1);

	++num_bmodel_calls;
}

static void R_AddBModelCallWithTextures (int index, int first_instance, int num_instances, gltexture_t *tx, gltexture_t *fb, gltexture_t *em,
	tcgen_mode_t tcgen, qboolean zfix, float polygon_offset_factor, float polygon_offset_units, float alpha_override, unsigned extra_flags,
	const vec4_t stage_color)
{
	GLuint flags;
	float alpha;

	if (num_bmodel_calls == MAX_BMODEL_DRAWS)
		R_FlushBModelCalls ();

	tx = R_SanitizeGLTexture (tx, "shaderstage", "base");
	fb = R_SanitizeGLTexture (fb, "shaderstage", "fullbright");
	em = R_SanitizeGLTexture (em, "shaderstage", "emissive");

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
		call->_pad0 = 0.f;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->_pad1[0] = 0.f;
		call->_pad1[1] = 0.f;
		call->stage_color[0] = stage_color ? stage_color[0] : 1.f;
		call->stage_color[1] = stage_color ? stage_color[1] : 1.f;
		call->stage_color[2] = stage_color ? stage_color[2] : 1.f;
		call->stage_color[3] = stage_color ? stage_color[3] : 1.f;
		call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
		call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
		call->emissive = em ? em->bindless_handle : blacktexture->bindless_handle;
		call->_pad2 = 0;
	}
	else
	{
		bmodel_bound_gpu_call_t *call = &bmodel_calls.bound.params[num_bmodel_calls];
		gltexture_t **textures = bmodel_calls.bound.textures[num_bmodel_calls];
		call->flags = flags;
		call->tcgen = tcgen;
		call->alpha = alpha;
		call->_pad0 = 0.f;
		call->polygon_offset[0] = polygon_offset_factor;
		call->polygon_offset[1] = polygon_offset_units;
		call->_pad1[0] = 0.f;
		call->_pad1[1] = 0.f;
		call->stage_color[0] = stage_color ? stage_color[0] : 1.f;
		call->stage_color[1] = stage_color ? stage_color[1] : 1.f;
		call->stage_color[2] = stage_color ? stage_color[2] : 1.f;
		call->stage_color[3] = stage_color ? stage_color[3] : 1.f;
		call->baseinstance = first_instance;
		call->padding[0] = 0;
		call->padding[1] = 0;
		call->padding[2] = 0;
		textures[0] = tx ? tx : greytexture;
		textures[1] = fb ? fb : blacktexture;
		textures[2] = em ? em : blacktexture;
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

static void R_EvalStageColorAlpha (const mat_shader_stage_t *stage, float time, vec4_t out)
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
			Con_Warning ("MatShader: rgbGen/alphaGen vertex requested but vertex colors are unavailable; using identity.\n");
			warned_vertex_color = true;
		}
		break;
	case MAT_RGBGEN_CONST:
		out[0] = stage->const_color[0];
		out[1] = stage->const_color[1];
		out[2] = stage->const_color[2];
		break;
	case MAT_RGBGEN_WAVE:
		wave_value = R_Clamp01 (Mat_Shader_EvalWaveValue (&stage->rgb_wave, time));
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
			Con_Warning ("MatShader: rgbGen/alphaGen vertex requested but vertex colors are unavailable; using identity.\n");
			warned_vertex_color = true;
		}
		break;
	case MAT_ALPHAGEN_CONST:
		out[3] = stage->const_alpha;
		break;
	case MAT_ALPHAGEN_WAVE:
		out[3] = R_Clamp01 (Mat_Shader_EvalWaveValue (&stage->alpha_wave, time));
		break;
	case MAT_ALPHAGEN_IDENTITY:
	default:
		break;
	}
}

static unsigned R_MapBlendMode (const mat_shader_stage_t *stage)
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

static gltexture_t *R_FindStageTexture (const qmodel_t *model, const mat_shader_stage_t *stage, float time)
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

	path = MatStage_GetAnimMapPath ((mat_shader_stage_t *)stage, time);
	if (!path || !path[0])
		path = stage->map_path;
	if (!path || !path[0])
		return NULL;

	return TexMgr_FindTexture ((qmodel_t *)model, path);
}

static qboolean R_StageUsesLightmap (const mat_shader_stage_t *stage, size_t stage_index)
{
	if (!stage)
		return false;
	if (stage->map_type == MAT_MAP_LIGHTMAP)
		return true;
	if (stage_index == 0 && stage->blend_mode == MAT_BLEND_REPLACE)
		return true;
	return false;
}

static tcgen_mode_t R_ResolveStageTcGen (const mat_shader_stage_t *stage)
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

static qboolean R_StageIsOpaqueBase (const shader_material_t *material)
{
	const mat_shader_stage_t *stage;

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
        BP_SHADOW,
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
	int current_blend_src = GL_ONE;
	int current_blend_dst = GL_ZERO;
	GLuint program;
	GLuint buf;
	GLbyte *ofs;
	textype_t texbegin, texend;
	qboolean oit;

	if (!count || r_shaders.value <= 0.f)
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
			texture_t *t = R_GetUsedTexture (model, j, NULL);
			const shader_material_t *material;
			unsigned mat_flags = 0u;
			unsigned mat_call_flags = 0u;
			size_t stage_count;
			size_t stage_index;

			if (!t || !R_ValidPtr (t))
				continue;

			if (!t->shader)
				continue;

			mat_flags = (r_shaders.value > 0.f) ? t->shader_flags : 0u;
			if (mat_flags & MAT_SHADERFLAG_NODRAW)
				continue;

			material = t->shader;
			if (material->sort_key != sort_key)
				continue;

			stage_count = material->stages ? VEC_SIZE (material->stages) : 0;
			if (!stage_count)
				continue;

			if (mat_flags & MAT_SHADERFLAG_TRANS)
				mat_call_flags |= CALLFLAG_MAT_TRANS;
			if (mat_flags & MAT_SHADERFLAG_SKY)
				mat_call_flags |= CALLFLAG_MAT_SKY;
			mat_call_flags |= CALLFLAG_MAT_HAS_SHADER;

			for (stage_index = 0; stage_index < stage_count; stage_index++)
			{
				const mat_shader_stage_t *stage = &material->stages[stage_index];
				gltexture_t *stage_tex;
				gltexture_t *fb = NULL;
				gltexture_t *em = NULL;
				vec4_t stage_color;
				unsigned extra_flags = mat_call_flags | R_StageOutputCallFlags (stage);
				unsigned stage_state;
				GLenum depth_func;
				qboolean blend_changed;
				qboolean uses_lightmap;
				qboolean stage_is_base = (stage_index == 0 && R_StageIsOpaqueBase (material));

				if (stage_is_base)
					continue;

				if (stage->map_type == MAT_MAP_MAP && (!stage->map_path || !stage->map_path[0]) && stage_index == 0)
					stage_tex = t->gltexture;
				else
					stage_tex = R_FindStageTexture (model, stage, cl.time);

				if (!stage_tex)
					continue;

				R_EvalStageColorAlpha (stage, cl.time, stage_color);

				if (stage_index == 0 && stage_tex == t->gltexture)
				{
					fb = t->fullbright;
					em = t->emissive;
					if (!gl_fullbrights.value && t->type != TEXTYPE_SKY)
						fb = NULL;
				}

				uses_lightmap = R_StageUsesLightmap (stage, stage_index);
				if (!uses_lightmap)
					extra_flags |= CALLFLAG_NOLIGHTMAP;
				if (t->type == TEXTYPE_CUTOUT)
					extra_flags |= CALLFLAG_ALPHA_TEST;

				stage_state = GLS_ATTRIBS (6) | R_MapBlendMode (stage) | R_MapCullMode (material->cull_mode);
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
					GL_SetState (stage_state);
					current_state = stage_state;
					glDepthFunc (depth_func);
					current_depth = depth_func;

					if (stage->blend_mode == MAT_BLEND_PREMULT)
						glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
					else if (stage->blend_mode == MAT_BLEND_CUSTOM)
						glBlendFunc (stage->blend_src, stage->blend_dst);
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
						stage_tex, fb, em, R_ResolveStageTcGen (stage),
						use_polygon_offset, polygon_offset_factor, polygon_offset_units, -1.f, extra_flags, stage_color);
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

	if (!count || r_shaders.value <= 0.f || !glprogs.godrays_source)
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
	GL_SetState (GLS_CULL_BACK | GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_ATTRIBS (6));
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
	GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);

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
			const shader_material_t *material;
			unsigned mat_flags = 0u;
			size_t stage_count;

			if (!t || !R_ValidPtr (t))
				continue;

			if (!t->shader || !t->shader->stages)
				continue;

			mat_flags = (r_shaders.value > 0.f) ? t->shader_flags : 0u;
			if (mat_flags & MAT_SHADERFLAG_NODRAW)
				continue;

			material = t->shader;
			stage_count = material->stages ? VEC_SIZE (material->stages) : 0;
			if (!stage_count)
				continue;

			for (size_t stage_index = 0; stage_index < stage_count; stage_index++)
			{
				const mat_shader_stage_t *stage = &material->stages[stage_index];
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

				if (r_godrays_emit_lighttex.value > 0.f)
					extra_flags |= CALLFLAG_GODRAYS_LIGHT;

				if (wants_emissive && r_godrays_emit_emissive.value > 0.f)
				{
					extra_flags |= CALLFLAG_GODRAYS_EMISSIVE;
					extra_flags |= CALLFLAG_MAT_EMISSIVE;
				}

				if ((extra_flags & (CALLFLAG_GODRAYS_LIGHT | CALLFLAG_GODRAYS_EMISSIVE)) == 0u)
					continue;

				if (t->type == TEXTYPE_CUTOUT)
					extra_flags |= CALLFLAG_ALPHA_TEST;

				{
					float polygon_offset_factor;
					float polygon_offset_units;
					qboolean use_polygon_offset = zfix || material->polygon_offset;

					R_GetPolygonOffsetValues (material, use_polygon_offset, &polygon_offset_factor, &polygon_offset_units);
					R_AddBModelCallWithTextures (model->firstcmd + j, baseinst, numinst,
						stage_tex, fb, em, R_ResolveStageTcGen (stage),
						use_polygon_offset, polygon_offset_factor, polygon_offset_units, -1.f, extra_flags, stage_color);
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
        case BP_SHADOW:
                texbegin = 0;
                texend = TEXTYPE_CUTOUT;
                program = glprogs.shadow_depth;
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
                texbegin = 0;
                texend = TEXTYPE_CUTOUT;
                program = glprogs.world_dlight[0];
                break;
        case BP_DLIGHT_ALPHA:
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
                state |= GLS_BLEND_ADD | GLS_NO_ZWRITE;
        else if (pass == BP_GODRAYS)
                state |= GLS_BLEND_ADD | GLS_NO_ZWRITE;
        else if (pass == BP_SHADOW)
        {
                state &= ~GLS_MASK_CULL;
                state |= GLS_BLEND_OPAQUE | GLS_CULL_FRONT;
        }
        else if (!translucent)
                state |= GLS_BLEND_OPAQUE;
        else
                state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;

        R_ResetBModelCalls (program);
        GL_SetState (state);
        GL_UseProgram (program);
#if defined(SHDLOG)
        {
                GLint gl_current_program = 0;
                glGetIntegerv (GL_CURRENT_PROGRAM, &gl_current_program);
                if ((GLuint)gl_current_program != program)
                {
                        GL_ClearCachedProgram ();
                        GL_UseProgram (program);
                }
        }
#endif
if (pass <= BP_ALPHATEST)
{
GL_UseProgram (program);
#if defined(SHDLOG)
{
GLint gl_current_program = 0;
glGetIntegerv (GL_CURRENT_PROGRAM, &gl_current_program);
if ((GLuint)gl_current_program != program)
{
        GL_ClearCachedProgram ();
        GL_UseProgram (program);
}
}
#endif
GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);
R_Shadow_BindShadowMap (GL_TEXTURE5);
	// FIX: Always bind the raw shadow depth texture on TEXTURE6 so that
	// ShadowMapRaw (sampler2D, no hardware compare) works correctly in ALL
	// debug modes (ShadowDebug.y > 2.5 path in shadow_sample.glsl).
	// Previously this was only done for r_shadow_debug >= 3, leaving
	// ShadowMapRaw unbound (reading 0) for intermediate debug modes 2.5..3.
	// Note: TEXTURE6 must stay GL_NONE compare mode – only TEXTURE5 uses
	// GL_COMPARE_REF_TO_TEXTURE (set inside R_Shadow_BindShadowMap).
	GL_BindNative (GL_TEXTURE6, GL_TEXTURE_2D, R_Shadow_GetShadowMapTextureId ());
R_Shadow_Log_ReceiverPassSnapshot ("WORLD", program, (GLint)GL_GetCurrentProgram (), GL_TEXTURE5, R_Shadow_GetShadowMapTextureId (), r_shadows.value > 0.f && r_shadow_sun.value > 0.f, r_shadow_bias.value, r_shadow_normalbias.value, r_shadow_pcf.value > 0.f ? 1.f : 0.f, r_shadow_pcf_taps.value, r_framedata.shadow_viewproj);
		R_Shadow_DebugValidateBinding ("WORLD", GL_TEXTURE5, R_Shadow_GetShadowMapTextureId ());
}
else if (pass == BP_DLIGHT_SOLID || pass == BP_DLIGHT_ALPHA)
{
R_Shadow_BindDlightShadowMap (GL_TEXTURE5);
		R_Shadow_Log_ReceiverPassSnapshot ("WORLD_DLIGHT", program, (GLint)GL_GetCurrentProgram (), GL_TEXTURE5, R_Shadow_GetDlightShadowMapTextureId (),
			r_shadows.value > 0.f && r_shadow_dlights.value > 0.f, r_shadow_dlight_bias.value, 0.f,
			r_shadow_dlight_pcf_taps.value > 0.f ? 1.f : 0.f, r_shadow_dlight_pcf_taps.value, r_framedata.shadow_viewproj);
		R_Shadow_DebugValidateBinding ("WORLD_DLIGHT", GL_TEXTURE5, R_Shadow_GetDlightShadowMapTextureId ());
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

			mat_flags = (r_shaders.value > 0.f) ? t->shader_flags : 0u;
			if (mat_flags & MAT_SHADERFLAG_NODRAW)
				continue;

			if (pass == BP_GODRAYS)
			{
				if (r_shaders.value <= 0.f || !t->shader || !t->shader->stages)
					continue;
				continue;
			}

			if (r_shaders.value > 0.f && t->shader)
			{
				mat_call_flags |= CALLFLAG_MAT_HAS_SHADER;
				if (t->shader->stages)
					mat_call_flags |= R_StageOutputCallFlags (&t->shader->stages[0]);
			}
			if (mat_flags & MAT_SHADERFLAG_TRANS)
				mat_call_flags |= CALLFLAG_MAT_TRANS;
			if (mat_flags & MAT_SHADERFLAG_SKY)
				mat_call_flags |= CALLFLAG_MAT_SKY;

			if ((pass == BP_SOLID || pass == BP_ALPHATEST) && r_shaders.value > 0.f && t->shader && t->shader->stages)
			{
				if (!R_StageIsOpaqueBase (t->shader))
					continue;
			}

			if (r_shaders.value > 0.f && t->shader && t->shader->polygon_offset)
				zfix = true;

			extra_flags |= mat_call_flags;
			{
				float polygon_offset_factor;
				float polygon_offset_units;
				const shader_material_t *material = (r_shaders.value > 0.f) ? t->shader : NULL;

				R_GetPolygonOffsetValues (material, zfix, &polygon_offset_factor, &polygon_offset_units);
				R_AddBModelCall (model->firstcmd + j, baseinst, numinst,
					pass != BP_SHOWTRIS ? R_TextureAnimation (t, frame) : 0,
					zfix, polygon_offset_factor, polygon_offset_units, -1, extra_flags, force_fullbright);
			}
		}
		
		baseinst += numinst;
        }

	GL_UseProgram (program);
#if defined(SHDLOG)
	{
		GLint gl_current_program = 0;
		glGetIntegerv (GL_CURRENT_PROGRAM, &gl_current_program);
		if ((GLuint)gl_current_program != program)
		{
			GL_ClearCachedProgram ();
			GL_UseProgram (program);
		}
	}
#endif
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
	GLuint buf, program;
	GLbyte *ofs;
	qboolean oit;

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
		program = glprogs.world[oit][q_max(0, (int)softemu - 1)][WORLDSHADER_WATER];
	else
		program = glprogs.water[oit][softemu == SOFTEMU_COARSE];

R_ResetBModelCalls (program);
GL_SetState (state);
GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
GL_Bind (GL_TEXTURE3, (r_lightingdir.value > 0.f && lightmap_dir_texture) ? lightmap_dir_texture : greytexture);

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

			if (!t)
				continue;
			if (r_shaders.value > 0.f && (t->shader_flags & MAT_SHADERFLAG_NODRAW))
				continue;

			mat_flags = (r_shaders.value > 0.f) ? t->shader_flags : 0u;
			if (r_shaders.value > 0.f && t->shader)
				extra_flags |= CALLFLAG_MAT_HAS_SHADER;
			if (r_shaders.value > 0.f && t->shader && t->shader->stages)
				extra_flags |= R_StageOutputCallFlags (&t->shader->stages[0]);
			if (mat_flags & MAT_SHADERFLAG_TRANS)
				extra_flags |= CALLFLAG_MAT_TRANS;
			if (mat_flags & MAT_SHADERFLAG_SKY)
				extra_flags |= CALLFLAG_MAT_SKY;

			float alpha = GL_WaterAlphaForEntityTextureType (e, t->type);
			if ((alpha < 1.f) != translucent)
				continue;
			{
				float polygon_offset_factor;
				float polygon_offset_units;
				qboolean zfix = !isworld;
				const shader_material_t *material = (r_shaders.value > 0.f) ? t->shader : NULL;

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

void R_DrawBrushModels_Shadow (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SHADOW, false);
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
