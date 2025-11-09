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
#include "gl_texmgr.h"
#include "renderer/r_iwshader.h"

#include <math.h>
#include <stdlib.h>

extern cvar_t gl_fullbrights, r_oldskyleaf, r_showtris; //johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_oit;

extern gltexture_t *lightmap_texture;
extern gltexture_t *deluxemap_texture;

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

static void R_IWShader_FillTCGenInfo(const iwStage_t *stage, GLuint *mode, float basis0[4], float basis1[4])
{
        iwTCAlign_t align = stage ? stage->tcAlign : IW_TC_ALIGN_OBJECT;
        *mode = (GLuint) align;
        if (align == IW_TC_ALIGN_WORLD)
        {
                basis0[0] = 1.f; basis0[1] = 0.f; basis0[2] = 0.f; basis0[3] = 0.f;
                basis1[0] = 0.f; basis1[1] = 1.f; basis1[2] = 0.f; basis1[3] = 0.f;
        }
        else
        {
                basis0[0] = basis0[1] = basis0[2] = basis0[3] = 0.f;
                basis1[0] = basis1[1] = basis1[2] = basis1[3] = 0.f;
        }
}

typedef struct bmodel_gpu_instance_s {
	float		world[12];	// world matrix (transposed mat4x3)
	float		prev_world[12];	// previous world matrix (transposed mat4x3)
	float		alpha;
	float		padding[3];
} bmodel_gpu_instance_t;

typedef struct bmodel_bindless_gpu_call_s {
        GLuint          flags;
        GLfloat         alpha;
        GLuint64        texture;
        GLuint64        fullbright;
        GLuint64        emissive;
        GLuint          tcgen_mode[4];
        float           tcgen_basis0[4];
        float           tcgen_basis1[4];
        float           emissive_tcgen_basis0[4];
        float           emissive_tcgen_basis1[4];
        float           tcmod_matrix[4];
        float           tcmod_translate[4];
        float           emissive_matrix[4];
        float           emissive_translate[4];
        float           emissive_color[4];
} bmodel_bindless_gpu_call_t;

typedef struct bmodel_bound_gpu_call_s {
        GLuint          flags;
        GLfloat         alpha;
        GLint           baseinstance;
        GLint           padding;
        GLuint          tcgen_mode[4];
        float           tcgen_basis0[4];
        float           tcgen_basis1[4];
        float           emissive_tcgen_basis0[4];
        float           emissive_tcgen_basis1[4];
        float           tcmod_matrix[4];
        float           tcmod_translate[4];
        float           emissive_matrix[4];
        float           emissive_translate[4];
        float           emissive_color[4];
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
		gltexture_t						*textures[MAX_BMODEL_DRAWS][3];
	} bound;
} bmodel_calls;
static bmodel_gpu_call_remap_t		bmodel_call_remap[MAX_BMODEL_DRAWS];
static int								num_bmodel_calls;
static GLuint							bmodel_batch_program;

typedef enum {
	BMODEL_QUEUE_OPAQUE = 0,
	BMODEL_QUEUE_DECAL,
	BMODEL_QUEUE_ALPHA_TEST,
	BMODEL_QUEUE_TRANSLUCENT,
	BMODEL_QUEUE_OVERLAY,
	BMODEL_QUEUE_COUNT
} bmodel_queue_t;

static const char *const bmodel_queue_names[BMODEL_QUEUE_COUNT] = {
	"OPAQUE",
	"DECAL",
	"ALPHA_TEST",
	"TRANSLUCENT",
	"OVERLAY"
};

typedef struct {
	unsigned				cullMask;
	iwCull_t				cullMode;
	qboolean			depthTest;
	qboolean			depthWrite;
	qboolean			colorMaskEnabled;
	GLboolean		colorMask[4];
	iwColorMask_t	colorMaskBits;
	qboolean			polygonOffsetEnabled;
	float				polygonOffsetFactor;
	float				polygonOffsetUnits;
	qboolean			clampDiffuse;
	qboolean			clampEmissive;
	bmodel_queue_t	queue;
	float				sortKey;
	int					sortValue;
	const iwMaterial_t	*material;
} bmodel_cpu_call_t;

static bmodel_cpu_call_t bmodel_cpu_calls[MAX_BMODEL_DRAWS];
static int				bmodel_call_order[MAX_BMODEL_DRAWS];
static qboolean	bmodel_calls_require_bound;

static int R_CompareBModelCalls(const void *a, const void *b)
{
        int ia = *(const int *)a;
        int ib = *(const int *)b;
        const bmodel_cpu_call_t *ca = &bmodel_cpu_calls[ia];
        const bmodel_cpu_call_t *cb = &bmodel_cpu_calls[ib];

        if (ca->queue != cb->queue)
                return (ca->queue < cb->queue) ? -1 : 1;

        if (ca->queue == BMODEL_QUEUE_TRANSLUCENT)
        {
                if (ca->sortKey > cb->sortKey)
                        return -1;
                if (ca->sortKey < cb->sortKey)
                        return 1;
        }
        else if (ca->sortValue != cb->sortValue)
        {
                return (ca->sortValue < cb->sortValue) ? -1 : 1;
        }

        if (ia < ib)
                return -1;
        if (ia > ib)
                return 1;
        return 0;
}

static void R_SortBModelCalls(void)
{
        if (num_bmodel_calls <= 1)
                return;

        for (int i = 0; i < num_bmodel_calls; ++i)
                bmodel_call_order[i] = i;

        qsort(bmodel_call_order, num_bmodel_calls, sizeof(bmodel_call_order[0]), R_CompareBModelCalls);

        qboolean changed = false;
        for (int i = 0; i < num_bmodel_calls; ++i)
        {
                if (bmodel_call_order[i] != i)
                {
                        changed = true;
                        break;
                }
        }

        if (!changed)
                return;

        static bmodel_bindless_gpu_call_t bindless_tmp[MAX_BMODEL_DRAWS];
        static bmodel_bound_gpu_call_t bound_tmp[MAX_BMODEL_DRAWS];
        static gltexture_t *textures_tmp[MAX_BMODEL_DRAWS][3];
        static bmodel_gpu_call_remap_t remap_tmp[MAX_BMODEL_DRAWS];
        static bmodel_cpu_call_t cpu_tmp[MAX_BMODEL_DRAWS];

        for (int i = 0; i < num_bmodel_calls; ++i)
        {
                int src = bmodel_call_order[i];
                bindless_tmp[i] = bmodel_calls.bindless.params[src];
                bound_tmp[i] = bmodel_calls.bound.params[src];
                for (int t = 0; t < 3; ++t)
                        textures_tmp[i][t] = bmodel_calls.bound.textures[src][t];
                remap_tmp[i] = bmodel_call_remap[src];
                cpu_tmp[i] = bmodel_cpu_calls[src];
        }

        for (int i = 0; i < num_bmodel_calls; ++i)
        {
                bmodel_calls.bindless.params[i] = bindless_tmp[i];
                bmodel_calls.bound.params[i] = bound_tmp[i];
                for (int t = 0; t < 3; ++t)
                        bmodel_calls.bound.textures[i][t] = textures_tmp[i][t];
                bmodel_call_remap[i] = remap_tmp[i];
                bmodel_cpu_calls[i] = cpu_tmp[i];
        }
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
        bmodel_calls_require_bound = false;
}

/*
=============
R_FlushBModelCalls
=============
*/
static void R_FlushBModelCalls (void)
{
        GLuint  cmdbuf, buf;
        GLbyte  *ofs;
        size_t  dstcmdofs;

        if (!num_bmodel_calls)
                return;

        R_SortBModelCalls();

        qboolean use_bindless = gl_bindless_able && !bmodel_calls_require_bound;

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

        if (use_bindless)
        {
                GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_calls.bindless.params, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls, &buf, &ofs);
                GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls);
        }
        else
        {
                GL_Upload (GL_SHADER_STORAGE_BUFFER, &bmodel_calls.bound.params, sizeof (bmodel_calls.bound.params[0]) * num_bmodel_calls, &buf, &ofs);
                GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bound.params[0]) * num_bmodel_calls);
        }

        for (int i = 0; i < num_bmodel_calls; ++i)
        {
                bmodel_cpu_call_t *cpu = &bmodel_cpu_calls[i];

                unsigned prev_state = glstate;
                unsigned desired_state = (prev_state & ~(GLS_MASK_CULL | GLS_NO_ZTEST | GLS_NO_ZWRITE));
                desired_state = (desired_state & ~GLS_MASK_CULL) | cpu->cullMask;
                if (!cpu->depthTest)
                        desired_state |= GLS_NO_ZTEST;
                else
                        desired_state &= ~GLS_NO_ZTEST;
                if (!cpu->depthWrite)
                        desired_state |= GLS_NO_ZWRITE;
                else
                        desired_state &= ~GLS_NO_ZWRITE;

                if (desired_state != glstate)
                        GL_SetState (desired_state);

                GLboolean prev_mask[4];
                GL_GetColorMask (prev_mask);
                qboolean restore_color = false;
                if (cpu->colorMaskEnabled)
                {
                        GL_SetColorMask (cpu->colorMask[0], cpu->colorMask[1], cpu->colorMask[2], cpu->colorMask[3]);
                        restore_color = true;
                }
                else if (!(prev_mask[0] && prev_mask[1] && prev_mask[2] && prev_mask[3]))
                {
                        GL_SetColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        restore_color = true;
                }

                qboolean prev_poly_enabled;
                float prev_poly_factor, prev_poly_units;
                GL_GetPolygonOffset (&prev_poly_enabled, &prev_poly_factor, &prev_poly_units);
                qboolean restore_poly = false;
                if (cpu->polygonOffsetEnabled)
                {
                        if (!prev_poly_enabled || prev_poly_factor != cpu->polygonOffsetFactor || prev_poly_units != cpu->polygonOffsetUnits)
                                GL_SetPolygonOffset (true, cpu->polygonOffsetFactor, cpu->polygonOffsetUnits);
                        restore_poly = true;
                }
                else if (prev_poly_enabled)
                {
                        GL_SetPolygonOffset (false, 0.f, 0.f);
                        restore_poly = true;
                }

                GLint diffuse_wrap[2] = {0, 0};
                GLint emissive_wrap[2] = {0, 0};
                qboolean restore_diffuse_wrap = false;
                qboolean restore_emissive_wrap = false;

                if (!use_bindless)
                {
                        gltexture_t **textures = bmodel_calls.bound.textures[i];
                        gltexture_t *diffuse = textures[0] ? textures[0] : greytexture;
                        gltexture_t *fullbright = textures[1] ? textures[1] : blacktexture;
                        gltexture_t *emissive = textures[2] ? textures[2] : blacktexture;

                        GL_Bind (GL_TEXTURE0, diffuse);
                        GL_Bind (GL_TEXTURE1, fullbright);
                        GL_Bind (GL_TEXTURE4, emissive);

                        if (cpu->clampDiffuse && diffuse)
                        {
                                glGetTexParameteriv (diffuse->target, GL_TEXTURE_WRAP_S, &diffuse_wrap[0]);
                                glGetTexParameteriv (diffuse->target, GL_TEXTURE_WRAP_T, &diffuse_wrap[1]);
                                glTexParameteri (diffuse->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                                glTexParameteri (diffuse->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                                restore_diffuse_wrap = true;
                        }

                        if (cpu->clampEmissive && emissive)
                        {
                                glGetTexParameteriv (emissive->target, GL_TEXTURE_WRAP_S, &emissive_wrap[0]);
                                glGetTexParameteriv (emissive->target, GL_TEXTURE_WRAP_T, &emissive_wrap[1]);
                                glTexParameteri (emissive->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                                glTexParameteri (emissive->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                                restore_emissive_wrap = true;
                        }
                }

                GL_Uniform1iFunc (0, i);

                GL_DrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT,
                        (const GLvoid *)(dstcmdofs + (size_t)i * sizeof (bmodel_draw_indirect_t)));

                if (!use_bindless)
                {
                        gltexture_t **textures = bmodel_calls.bound.textures[i];
                        gltexture_t *diffuse = textures[0] ? textures[0] : greytexture;
                        gltexture_t *emissive = textures[2] ? textures[2] : blacktexture;

                        if (restore_diffuse_wrap && diffuse)
                        {
                                glTexParameteri (diffuse->target, GL_TEXTURE_WRAP_S, diffuse_wrap[0]);
                                glTexParameteri (diffuse->target, GL_TEXTURE_WRAP_T, diffuse_wrap[1]);
                        }
                        if (restore_emissive_wrap && emissive)
                        {
                                glTexParameteri (emissive->target, GL_TEXTURE_WRAP_S, emissive_wrap[0]);
                                glTexParameteri (emissive->target, GL_TEXTURE_WRAP_T, emissive_wrap[1]);
                        }
                }

                if (restore_poly)
                        GL_SetPolygonOffset (prev_poly_enabled, prev_poly_factor, prev_poly_units);
                if (restore_color)
                        GL_SetColorMask (prev_mask[0], prev_mask[1], prev_mask[2], prev_mask[3]);
                if (glstate != prev_state)
                        GL_SetState (prev_state);
        }

        num_bmodel_calls = 0;
}


#define CALLFLAG_EMISSIVE        (1u << 3)
#define CALLFLAG_ALPHA_TEST      (1u << 4)

static gltexture_t *R_IWShader_FindTextureForPath(const texture_t *base, const char *path)
{
        gltexture_t *tex = NULL;

        if (!path || !*path)
                return base ? base->gltexture : NULL;

        if (path[0] == '$')
        {
                if (!q_strcasecmp(path, "$white"))
                        return whitetexture;
                if (!q_strcasecmp(path, "$black"))
                        return blacktexture;
                if (!q_strcasecmp(path, "$grey") || !q_strcasecmp(path, "$gray"))
                        return greytexture;
                return base ? base->gltexture : NULL;
        }

        qmodel_t *owner = (base && base->gltexture) ? base->gltexture->owner : NULL;
        if (owner)
        {
                char full_name[MAX_OSPATH];
                q_snprintf(full_name, sizeof(full_name), "%s:%s", owner->name, path);
                tex = TexMgr_FindTexture(owner, full_name);
                if (!tex)
                        tex = TexMgr_FindTexture(owner, path);
                if (!tex && owner->textures)
                {
                        for (int i = 0; i < owner->numtextures; ++i)
                        {
                                texture_t *other = owner->textures[i];
                                if (!other || !other->gltexture)
                                        continue;
                                if (!q_strcasecmp(other->name, path))
                                        return other->gltexture;
                        }
                }
        }

        if (!tex)
                tex = TexMgr_FindTexture(NULL, path);
        if (!tex && base)
                tex = base->gltexture;
        return tex;
}

static gltexture_t *R_IWShader_FindStageTexture(const texture_t *base, const iwStage_t *stage, float time)
{
        if (!stage)
                return base ? base->gltexture : NULL;

        const char *path = stage->mapPath;
        if (stage->animMap && stage->numAnimFrames > 0)
        {
                float fps = stage->animFps != 0.f ? fabsf(stage->animFps) : 1.f;
                int frame = (int)floor(time * fps);
                frame %= stage->numAnimFrames;
                if (frame < 0)
                        frame += stage->numAnimFrames;
                if (frame >= 0 && frame < stage->numAnimFrames)
                        path = stage->animPaths[frame];
        }

        return R_IWShader_FindTextureForPath(base, path);
}

static void R_IWShader_EmissiveColor(const iwStage_t *stage, float color[3])
{
        color[0] = color[1] = color[2] = 1.f;
        if (!stage)
                return;
        if (stage->rgbgen == IW_RGB_CONST)
        {
                color[0] = stage->rgbConst[0];
                color[1] = stage->rgbConst[1];
                color[2] = stage->rgbConst[2];
        }
}

typedef enum
{
        BP_SOLID,
        BP_ALPHATEST,
        BP_SKYLAYERS,
        BP_SKYCUBEMAP,
        BP_SKYSTENCIL,
        BP_SHOWTRIS,
} brushpass_t;

/*
=============
R_AddBModelCall
=============
*/
static void R_AddBModelCall (brushpass_t pass, qboolean translucent_pass, int index, int first_instance, int num_instances, texture_t *t, qboolean zfix, entity_t *base_entity)
{
        GLuint          flags;
        float           alpha;
        gltexture_t     *tx, *fb, *em;
        const iwMaterial_t *material = NULL;
        iwTexMatrix_t   tex_matrix;
        iwTexMatrix_t   emissive_matrix;
        float           emissive_color[3];
        const iwStage_t *base_stage = NULL;
        const iwStage_t *emissive_stage = NULL;
        float           material_alpha = -1.f;

        IW_TexMatrixIdentity (&tex_matrix);
        IW_TexMatrixIdentity (&emissive_matrix);
        R_IWShader_EmissiveColor (NULL, emissive_color);

        if (t && t->gltexture)
        {
                const char *material_name = NULL;

                if (t->name[0])
                        material_name = t->name;
                else if (t->gltexture->name[0])
                        material_name = t->gltexture->name;
                else if (t->gltexture->source_file[0])
                        material_name = t->gltexture->source_file;

                material = IW_MaterialForTexture (material_name);
        }
        else
        {
                material = IW_MaterialForTexture (NULL);
        }

        if (material)
        {
                IW_MaterialTexMatrix (material, r_framedata.time, &tex_matrix);
                if (material->numStages > 0)
                {
                        base_stage = &material->stages[0];
                        if ((base_stage->blendMode == IW_BLEND_ALPHA || base_stage->blendMode == IW_BLEND_ADD_ALPHA) &&
                            base_stage->alphagen == IW_A_CONST)
                                material_alpha = q_clamp(base_stage->aConst, 0.f, 1.f);
                }

                for (int s = 0; s < material->numStages; ++s)
                {
                        const iwStage_t *stage = &material->stages[s];
                        if (!stage->emissive)
                                continue;
                        emissive_stage = stage;
                        break;
                }
        }

        if (num_bmodel_calls == MAX_BMODEL_DRAWS)
                R_FlushBModelCalls ();

        if (t)
        {
                tx = t->gltexture;
                fb = t->fullbright;
                em = t->emissive;
                if (r_lightmap_cheatsafe)
                        tx = fb = em = NULL;
                if (!gl_fullbrights.value && t->type != TEXTYPE_SKY)
                        fb = NULL;
        }
        else
        {
                tx = fb = whitetexture;
                em = NULL;
        }

        if (emissive_stage)
        {
                gltexture_t *stage_tex = R_IWShader_FindStageTexture(t, emissive_stage, r_framedata.time);
                if (stage_tex)
                        em = stage_tex;
                R_IWShader_EmissiveColor(emissive_stage, emissive_color);
                IW_StageTexMatrix(emissive_stage, r_framedata.time, &emissive_matrix);
        }

        if (!gl_zfix.value || map_checks.value)
                zfix = 0;

        flags = zfix | ((fb != NULL) << 1) | ((r_fullbright_cheatsafe != false) << 2);
        if (em != NULL)
                flags |= CALLFLAG_EMISSIVE;
        if (t && t->type == TEXTYPE_CUTOUT)
                flags |= CALLFLAG_ALPHA_TEST;
        alpha = t ? GL_WaterAlphaForTextureType (t->type) : 1.f;
        if (material_alpha >= 0.f)
                alpha = material_alpha;

        GLuint base_tcgen_mode, emissive_tcgen_mode;
        float base_tcgen_basis0[4];
        float base_tcgen_basis1[4];
        float emissive_tcgen_basis0[4];
        float emissive_tcgen_basis1[4];
        R_IWShader_FillTCGenInfo(base_stage, &base_tcgen_mode, base_tcgen_basis0, base_tcgen_basis1);
        R_IWShader_FillTCGenInfo(emissive_stage, &emissive_tcgen_mode, emissive_tcgen_basis0, emissive_tcgen_basis1);

        if (gl_bindless_able)
        {
                bmodel_bindless_gpu_call_t *call = &bmodel_calls.bindless.params[num_bmodel_calls];
                call->flags = flags;
                call->alpha = alpha;
                call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
                call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
                call->emissive = em ? em->bindless_handle : blacktexture->bindless_handle;
                call->tcgen_mode[0] = base_tcgen_mode;
                call->tcgen_mode[1] = emissive_tcgen_mode;
                call->tcgen_mode[2] = 0u;
                call->tcgen_mode[3] = 0u;
                memcpy (call->tcgen_basis0, base_tcgen_basis0, sizeof (base_tcgen_basis0));
                memcpy (call->tcgen_basis1, base_tcgen_basis1, sizeof (base_tcgen_basis1));
                memcpy (call->emissive_tcgen_basis0, emissive_tcgen_basis0, sizeof (emissive_tcgen_basis0));
                memcpy (call->emissive_tcgen_basis1, emissive_tcgen_basis1, sizeof (emissive_tcgen_basis1));
                memcpy (call->tcmod_matrix, tex_matrix.matrix, sizeof (tex_matrix.matrix));
                call->tcmod_translate[0] = tex_matrix.translate[0];
                call->tcmod_translate[1] = tex_matrix.translate[1];
                call->tcmod_translate[2] = 0.f;
                call->tcmod_translate[3] = 0.f;
                memcpy (call->emissive_matrix, emissive_matrix.matrix, sizeof (emissive_matrix.matrix));
                call->emissive_translate[0] = emissive_matrix.translate[0];
                call->emissive_translate[1] = emissive_matrix.translate[1];
                call->emissive_translate[2] = 0.f;
                call->emissive_translate[3] = 0.f;
                call->emissive_color[0] = emissive_color[0];
                call->emissive_color[1] = emissive_color[1];
                call->emissive_color[2] = emissive_color[2];
                call->emissive_color[3] = (flags & CALLFLAG_EMISSIVE) ? 1.f : 0.f;
        }
        else
        {
                bmodel_bound_gpu_call_t *call = &bmodel_calls.bound.params[num_bmodel_calls];
                gltexture_t **textures = bmodel_calls.bound.textures[num_bmodel_calls];
                call->flags = flags;
                call->alpha = alpha;
                call->baseinstance = first_instance;
                call->padding = 0;
                call->tcgen_mode[0] = base_tcgen_mode;
                call->tcgen_mode[1] = emissive_tcgen_mode;
                call->tcgen_mode[2] = 0u;
                call->tcgen_mode[3] = 0u;
                memcpy (call->tcgen_basis0, base_tcgen_basis0, sizeof (base_tcgen_basis0));
                memcpy (call->tcgen_basis1, base_tcgen_basis1, sizeof (base_tcgen_basis1));
                memcpy (call->emissive_tcgen_basis0, emissive_tcgen_basis0, sizeof (emissive_tcgen_basis0));
                memcpy (call->emissive_tcgen_basis1, emissive_tcgen_basis1, sizeof (emissive_tcgen_basis1));
                memcpy (call->tcmod_matrix, tex_matrix.matrix, sizeof (tex_matrix.matrix));
                call->tcmod_translate[0] = tex_matrix.translate[0];
                call->tcmod_translate[1] = tex_matrix.translate[1];
                call->tcmod_translate[2] = 0.f;
                call->tcmod_translate[3] = 0.f;
                memcpy (call->emissive_matrix, emissive_matrix.matrix, sizeof (emissive_matrix.matrix));
                call->emissive_translate[0] = emissive_matrix.translate[0];
                call->emissive_translate[1] = emissive_matrix.translate[1];
                call->emissive_translate[2] = 0.f;
                call->emissive_translate[3] = 0.f;
                call->emissive_color[0] = emissive_color[0];
                call->emissive_color[1] = emissive_color[1];
                call->emissive_color[2] = emissive_color[2];
                call->emissive_color[3] = (flags & CALLFLAG_EMISSIVE) ? 1.f : 0.f;
                textures[0] = tx ? tx : greytexture;
                textures[1] = fb ? fb : blacktexture;
                textures[2] = em ? em : blacktexture;
        }

        iwColorMask_t mask = base_stage ? base_stage->colorMask : IW_COLORMASK_RGBA;
        qboolean colorMaskEnabled = (mask != IW_COLORMASK_RGBA);
        GLboolean maskValues[4] = {
                (mask & IW_COLORMASK_R) != 0,
                (mask & IW_COLORMASK_G) != 0,
                (mask & IW_COLORMASK_B) != 0,
                (mask & IW_COLORMASK_A) != 0
        };

        qboolean clampDiffuse = base_stage && base_stage->clamp;
        qboolean clampEmissive = emissive_stage && emissive_stage->clamp;
        if (clampDiffuse || clampEmissive)
                bmodel_calls_require_bound = true;

        iwCull_t cullMode = material ? material->cull : IW_CULL_BACK;
        unsigned cullMask = GLS_CULL_BACK;
        if (cullMode == IW_CULL_FRONT)
                cullMask = GLS_CULL_FRONT;
        else if (cullMode == IW_CULL_NONE)
                cullMask = GLS_CULL_NONE;

        bmodel_queue_t queue = BMODEL_QUEUE_OPAQUE;
        if (pass == BP_SKYLAYERS || pass == BP_SKYCUBEMAP || pass == BP_SKYSTENCIL)
                queue = BMODEL_QUEUE_OVERLAY;
        else if (translucent_pass)
                queue = BMODEL_QUEUE_TRANSLUCENT;
        else if (flags & CALLFLAG_ALPHA_TEST)
                queue = BMODEL_QUEUE_ALPHA_TEST;

        if (material)
        {
                switch (material->sort)
                {
                case IW_SORT_DECAL:
                        queue = BMODEL_QUEUE_DECAL;
                        break;
                case IW_SORT_ALPHA:
                case IW_SORT_ADDITIVE:
                        queue = BMODEL_QUEUE_TRANSLUCENT;
                        break;
                case IW_SORT_SKY:
                        queue = BMODEL_QUEUE_OVERLAY;
                        break;
                case IW_SORT_CUSTOM:
                        if (material->sortValue >= 0)
                        {
                                int bucket = material->sortValue;
                                if (bucket < 0)
                                        bucket = 0;
                                if (bucket >= BMODEL_QUEUE_COUNT)
                                        bucket = BMODEL_QUEUE_COUNT - 1;
                                queue = (bmodel_queue_t)bucket;
                        }
                        break;
                default:
                        break;
                }
        }

        qboolean depthTest = base_stage ? (base_stage->depthTest != 0) : true;
        qboolean defaultDepthWrite = (queue != BMODEL_QUEUE_TRANSLUCENT);
        qboolean depthWrite;
        if (base_stage)
        {
                if (base_stage->depthWrite == IW_DEPTHWRITE_AUTO)
                        depthWrite = defaultDepthWrite;
                else
                        depthWrite = base_stage->depthWrite != 0;
        }
        else
        {
                depthWrite = defaultDepthWrite;
        }

        qboolean polygonOffsetEnabled = material && material->polygonOffset.enabled;
        float polygonOffsetFactor = polygonOffsetEnabled ? material->polygonOffset.factor : 0.f;
        float polygonOffsetUnits = polygonOffsetEnabled ? material->polygonOffset.units : 0.f;

        int sortValue = 0;
        if (material && material->sortValue >= 0)
                sortValue = material->sortValue;

        float sortKey = 0.f;
        if (queue == BMODEL_QUEUE_TRANSLUCENT && base_entity)
        {
                vec3_t diff;
                VectorSubtract (base_entity->origin, r_refdef.vieworg, diff);
                sortKey = DotProduct (diff, diff);
        }
        else
        {
                sortKey = (float)sortValue;
        }

        if (material)
        {
                IW_Debugf ("Cull=%d", material->cull);
                IW_DebugSetMaterialState (material, bmodel_queue_names[queue], depthTest, depthWrite,
                        mask, cullMode, polygonOffsetEnabled, polygonOffsetFactor, polygonOffsetUnits);
        }

        bmodel_cpu_call_t *cpu = &bmodel_cpu_calls[num_bmodel_calls];
        cpu->cullMask = cullMask;
        cpu->cullMode = cullMode;
        cpu->depthTest = depthTest;
        cpu->depthWrite = depthWrite;
        cpu->colorMaskEnabled = colorMaskEnabled;
        cpu->colorMaskBits = mask;
        cpu->colorMask[0] = maskValues[0];
        cpu->colorMask[1] = maskValues[1];
        cpu->colorMask[2] = maskValues[2];
        cpu->colorMask[3] = maskValues[3];
        cpu->polygonOffsetEnabled = polygonOffsetEnabled;
        cpu->polygonOffsetFactor = polygonOffsetFactor;
        cpu->polygonOffsetUnits = polygonOffsetUnits;
        cpu->clampDiffuse = clampDiffuse;
        cpu->clampEmissive = clampEmissive;
        cpu->queue = queue;
        cpu->sortKey = sortKey;
        cpu->sortValue = sortValue;
        cpu->material = material;

        SDL_assert (num_instances > 0);
        SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
        bmodel_call_remap[num_bmodel_calls].src = index;
        bmodel_call_remap[num_bmodel_calls].inst = first_instance * MAX_BMODEL_INSTANCES + (num_instances - 1);

        ++num_bmodel_calls;
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
        }

        for (i = 0, totalinst = 0; i < count; i++)
        {
                entity_t *ent = ents[i];
                if (ent->model->texofs[texend] - ent->model->texofs[texbegin] > 0)
                        R_InitBModelInstance (&bmodel_instances[totalinst++], ent);
        }

        if (!totalinst)
                return;

        state = GLS_CULL_BACK | GLS_ATTRIBS(4);
        if (!translucent)
                state |= GLS_BLEND_OPAQUE;
        else
                state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;

        R_ResetBModelCalls (program);
        GL_SetState (state);

        if (pass <= BP_ALPHATEST || pass == BP_SHOWTRIS)
        {
                qboolean use_delux =
                        (r_deluxemaps.value > 0.f) && !r_fullbright_cheatsafe && !r_lightmap_cheatsafe &&
                        deluxemap_texture && cl.worldmodel && cl.worldmodel->deluxdata;

                GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
                GL_Bind (GL_TEXTURE3, use_delux ? deluxemap_texture : NULL);
        }
        else if (pass == BP_SKYCUBEMAP)
        {
                GL_Bind (GL_TEXTURE2, skybox->cubemap);
                GL_Bind (GL_TEXTURE3, NULL);
        }
        else
        {
                GL_Bind (GL_TEXTURE3, NULL);
        }

        GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * totalinst, &buf, &ofs);
        GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * totalinst);

        for (i = 0, baseinst = 0; i < count; /**/)
        {
                int numinst;
                entity_t *e = ents[i++];
                qmodel_t *model = e->model;
                qboolean isworld = (e == &cl_entities[0]);
                qboolean isstatic = PTR_IN_RANGE (e, cl_static_entities, cl_static_entities + MAX_STATIC_ENTITIES);
                qboolean zfix = !isworld && !isstatic;
                int frame = isworld ? 0 : e->frame;
                int numtex = model->texofs[texend] - model->texofs[texbegin];

                if (!numtex)
                        continue;

                for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
                        numinst += (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin]) > 0;

                for (j = model->texofs[texbegin]; j < model->texofs[texend]; j++)
                {
                        texture_t *t = model->textures[model->usedtextures[j]];
                        R_AddBModelCall (pass, translucent, model->firstcmd + j, baseinst, numinst, pass != BP_SHOWTRIS ? R_TextureAnimation (t, frame) : 0, zfix, e);
                }

                baseinst += numinst;
        }

        R_FlushBModelCalls ();
}


/*
=============
R_EntHasWater
=============
*/
static qboolean R_EntHasWater (entity_t *ent, qboolean translucent)
{
	int i;
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
	state = GLS_CULL_BACK | GLS_ATTRIBS(4);
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
        {
                qboolean use_delux =
                        (r_deluxemaps.value > 0.f) && !r_fullbright_cheatsafe && !r_lightmap_cheatsafe &&
                        deluxemap_texture && cl.worldmodel && cl.worldmodel->deluxdata;

                GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);
                GL_Bind (GL_TEXTURE3, use_delux ? deluxemap_texture : NULL);
        }

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * totalinst, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * count);

	// generate drawcalls
	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		int frame = isworld ? 0 : e->frame;

		if (!R_EntHasWater (e, translucent))
			continue;

		for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
			numinst += R_EntHasWater (ents[i], translucent);

		for (j = model->texofs[TEXTYPE_FIRSTLIQUID]; j < model->texofs[TEXTYPE_LASTLIQUID+1]; j++)
		{
			texture_t *t = model->textures[model->usedtextures[j]];
			if ((GL_WaterAlphaForEntityTextureType (e, t->type) < 1.f) != translucent)
				continue;
			R_AddBModelCall (BP_SOLID, translucent, model->firstcmd + j, baseinst, numinst, R_TextureAnimation (t, frame), !isworld, e);
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
