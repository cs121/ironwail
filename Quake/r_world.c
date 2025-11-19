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
#include "image.h"

#include <math.h>

extern cvar_t gl_fullbrights, r_oldskyleaf, r_showtris; //johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_oit;

extern gltexture_t *lightmap_texture;
extern gltexture_t *deluxemap_texture;

#define MAX_EFFECT_STAGES 4

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
		if (r_viewleaf->firstmarksurface[i]->flags & SURF_DRAWTURB)
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
	GLfloat		alpha;
	GLuint64	texture;
	GLuint64	fullbright;
	GLuint64	emissive;
	GLuint64	envmap;
	GLuint		padding0;
	GLuint		padding1;
	float		tcmod_matrix[4];
	float		tcmod_translate[4];
	float		tcmod_params0[4];
	float		tcmod_params1[4];
	float		tcgen_params[4];
	float		tcgen_s[4];
	float		tcgen_t[4];
	float		emissive_matrix[4];
	float		emissive_translate[4];
	float		emissive_color[4];
	float		fog_color[4];
	float		alpha_params0[4];
	float		alpha_params1[4];
	float		alpha_params2[4];
	float		env_params0[4];
	float		env_params1[4];
	float		env_params2[4];
	float		effect_info[4];
	float		effect_matrix[MAX_EFFECT_STAGES][4];
	float		effect_translate[MAX_EFFECT_STAGES][4];
	float		effect_color[MAX_EFFECT_STAGES][4];
	float		effect_alpha_params0[MAX_EFFECT_STAGES][4];
	float		effect_alpha_params1[MAX_EFFECT_STAGES][4];
	float		effect_alpha_params2[MAX_EFFECT_STAGES][4];
	float		effect_tcmod_params0[MAX_EFFECT_STAGES][4];
	float		effect_tcmod_params1[MAX_EFFECT_STAGES][4];
	float		effect_flags[MAX_EFFECT_STAGES][4];
	GLuint64	effect_handles[MAX_EFFECT_STAGES];
} bmodel_bindless_gpu_call_t;

typedef struct bmodel_bound_gpu_call_s {
	GLuint		flags;
	GLfloat		alpha;
	GLint		baseinstance;
	GLint		padding;
	float		tcmod_matrix[4];
	float		tcmod_translate[4];
	float		tcmod_params0[4];
	float		tcmod_params1[4];
	float		tcgen_params[4];
	float		tcgen_s[4];
	float		tcgen_t[4];
	float		emissive_matrix[4];
	float		emissive_translate[4];
	float		emissive_color[4];
	float		fog_color[4];
	float		alpha_params0[4];
	float		alpha_params1[4];
	float		alpha_params2[4];
	float		env_params0[4];
	float		env_params1[4];
	float		env_params2[4];
	float		effect_info[4];
	float		effect_matrix[MAX_EFFECT_STAGES][4];
	float		effect_translate[MAX_EFFECT_STAGES][4];
	float		effect_color[MAX_EFFECT_STAGES][4];
	float		effect_alpha_params0[MAX_EFFECT_STAGES][4];
	float		effect_alpha_params1[MAX_EFFECT_STAGES][4];
	float		effect_alpha_params2[MAX_EFFECT_STAGES][4];
	float		effect_tcmod_params0[MAX_EFFECT_STAGES][4];
	float		effect_tcmod_params1[MAX_EFFECT_STAGES][4];
	float		effect_flags[MAX_EFFECT_STAGES][4];
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
		gltexture_t					*textures[MAX_BMODEL_DRAWS][4 + MAX_EFFECT_STAGES];
	} bound;
} bmodel_calls;
static bmodel_gpu_call_remap_t		bmodel_call_remap[MAX_BMODEL_DRAWS];
static iwCull_t				bmodel_call_cull[MAX_BMODEL_DRAWS];
static GLbyte                           bmodel_call_depth_test[MAX_BMODEL_DRAWS];
static GLbyte                           bmodel_call_depth_write[MAX_BMODEL_DRAWS];
static GLbyte                           bmodel_call_depth_func_override[MAX_BMODEL_DRAWS];
static GLint                            bmodel_call_depth_func[MAX_BMODEL_DRAWS];
static GLboolean                        bmodel_call_blend_enable[MAX_BMODEL_DRAWS];
static GLenum                           bmodel_call_blend_src[MAX_BMODEL_DRAWS];
static GLenum                           bmodel_call_blend_dst[MAX_BMODEL_DRAWS];
static GLubyte                           bmodel_call_color_mask[MAX_BMODEL_DRAWS];
static int							num_bmodel_calls;
static GLuint						bmodel_batch_program;

static GLboolean                        bmodel_initial_blend_enabled;
static GLint                            bmodel_initial_blend_src_rgb;
static GLint                            bmodel_initial_blend_dst_rgb;
static GLint                            bmodel_initial_blend_src_alpha;
static GLint                            bmodel_initial_blend_dst_alpha;
static GLubyte                          bmodel_initial_color_mask_bits;

static GLenum IW_BlendFactorToGL(iwBlendFactor_t factor)
{
        switch (factor)
        {
        case IW_SRC_ZERO: return GL_ZERO;
        case IW_SRC_ONE: return GL_ONE;
        case IW_SRC_SRC_COLOR: return GL_SRC_COLOR;
        case IW_SRC_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
        case IW_SRC_DST_COLOR: return GL_DST_COLOR;
        case IW_SRC_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
        case IW_SRC_SRC_ALPHA: return GL_SRC_ALPHA;
        case IW_SRC_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case IW_SRC_DST_ALPHA: return GL_DST_ALPHA;
        case IW_SRC_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
        default: return GL_ONE;
        }
}

static GLenum IW_DepthFuncToGL(iwDepthFunc_t func)
{
        switch (func)
        {
        case IW_DEPTHFUNC_NEVER: return GL_NEVER;
        case IW_DEPTHFUNC_LESS: return GL_LESS;
        case IW_DEPTHFUNC_EQUAL: return GL_EQUAL;
        case IW_DEPTHFUNC_LEQUAL: return GL_LEQUAL;
        case IW_DEPTHFUNC_GREATER: return GL_GREATER;
        case IW_DEPTHFUNC_NOTEQUAL: return GL_NOTEQUAL;
        case IW_DEPTHFUNC_GEQUAL: return GL_GEQUAL;
        case IW_DEPTHFUNC_ALWAYS: return GL_ALWAYS;
        case IW_DEPTHFUNC_DEFAULT: return GL_LEQUAL;
        default: return GL_LEQUAL;
        }
}

static GLubyte IW_ColorMaskBits(iwColorMask_t mask)
{
        GLubyte bits = 0;
        if (mask & IW_COLORMASK_R) bits |= 1;
        if (mask & IW_COLORMASK_G) bits |= 2;
        if (mask & IW_COLORMASK_B) bits |= 4;
        if (mask & IW_COLORMASK_A) bits |= 8;
        return bits;
}

typedef struct
{
	GLboolean	enabled;
	GLint		mode;
} r_cull_state_t;

typedef struct
{
        GLboolean       testEnabled;
        GLboolean       writeMask;
        GLint           func;
} r_depth_state_t;

static void R_PushDepthState(r_depth_state_t *state);
static qboolean R_ApplyDepthState(GLbyte depthTestOverride, GLbyte depthWriteOverride, GLint depthFuncOverride, r_depth_state_t *state);
static void R_PopDepthState(const r_depth_state_t *state);

static void R_PushCullState(r_cull_state_t *state)
{
	if (!state)
		return;

	state->enabled = glIsEnabled(GL_CULL_FACE);
	if (state->enabled)
	{
		GLint mode = GL_BACK;
		glGetIntegerv(GL_CULL_FACE_MODE, &mode);
		state->mode = mode;
	}
	else
	{
		state->mode = GL_BACK;
	}
}

static void R_SetCullState(iwCull_t cull)
{
	switch (cull)
	{
	case IW_CULL_FRONT:
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);
		break;

	case IW_CULL_NONE:
		glDisable(GL_CULL_FACE);
		break;

	case IW_CULL_BACK:
	default:
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		break;
	}

	IW_Debugf("Cull=%d", (int)cull);
}

static void R_PopCullState(const r_cull_state_t *state)
{
	if (!state)
		return;

	if (state->enabled)
	{
		glEnable(GL_CULL_FACE);
		glCullFace(state->mode);
	}
	else
	{
		glDisable(GL_CULL_FACE);
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

        bmodel_initial_blend_enabled = glIsEnabled(GL_BLEND);
        bmodel_initial_blend_src_rgb = GL_ONE;
        bmodel_initial_blend_dst_rgb = GL_ZERO;
        bmodel_initial_blend_src_alpha = GL_ONE;
        bmodel_initial_blend_dst_alpha = GL_ZERO;
        glGetIntegerv(GL_BLEND_SRC_RGB, &bmodel_initial_blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &bmodel_initial_blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &bmodel_initial_blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &bmodel_initial_blend_dst_alpha);

        GLboolean maskValues[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleanv(GL_COLOR_WRITEMASK, maskValues);
        bmodel_initial_color_mask_bits = (maskValues[0] ? 1 : 0) |
                                         (maskValues[1] ? 2 : 0) |
                                         (maskValues[2] ? 4 : 0) |
                                         (maskValues[3] ? 8 : 0);

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

        GLboolean initialBlendEnabled = bmodel_initial_blend_enabled;
        GLint initialBlendSrcRGB = bmodel_initial_blend_src_rgb;
        GLint initialBlendDstRGB = bmodel_initial_blend_dst_rgb;
        GLint initialBlendSrcAlpha = bmodel_initial_blend_src_alpha;
        GLint initialBlendDstAlpha = bmodel_initial_blend_dst_alpha;
        GLboolean currentBlendEnabled = initialBlendEnabled;
        GLint currentBlendSrcRGB = initialBlendSrcRGB;
        GLint currentBlendDstRGB = initialBlendDstRGB;
        GLint currentBlendSrcAlpha = initialBlendSrcAlpha;
        GLint currentBlendDstAlpha = initialBlendDstAlpha;
        GLubyte initialMaskBits = bmodel_initial_color_mask_bits;
        GLubyte currentColorMask = initialMaskBits;

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

	if (gl_bindless_able)
	{
		GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_calls.bindless.params, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls, &buf, &ofs);
		GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bindless.params[0]) * num_bmodel_calls);
		const GLsizei stride = sizeof (bmodel_draw_indirect_t);
		for (int i = 0; i < num_bmodel_calls; )
		{
                iwCull_t cull = bmodel_call_cull[i];
                        GLbyte depthTest = bmodel_call_depth_test[i];
                        GLbyte depthWrite = bmodel_call_depth_write[i];
                GLint depthFunc = bmodel_call_depth_func_override[i] ? (GLint)bmodel_call_depth_func[i] : -1;
                GLboolean blendEnable = bmodel_call_blend_enable[i];
                GLenum blendSrc = bmodel_call_blend_src[i];
                GLenum blendDst = bmodel_call_blend_dst[i];
                GLubyte colorMask = bmodel_call_color_mask[i];
                int count = 1;
                        while (i + count < num_bmodel_calls && bmodel_call_cull[i + count] == cull &&
                               bmodel_call_depth_test[i + count] == depthTest &&
                               bmodel_call_depth_write[i + count] == depthWrite &&
                               (bmodel_call_depth_func_override[i + count] ? (GLint)bmodel_call_depth_func[i + count] : -1) == depthFunc &&
                               bmodel_call_blend_enable[i + count] == blendEnable &&
                               bmodel_call_blend_src[i + count] == blendSrc &&
                               bmodel_call_blend_dst[i + count] == blendDst &&
                               bmodel_call_color_mask[i + count] == colorMask)
                                ++count;

			r_cull_state_t guard;
			R_PushCullState(&guard);
			R_SetCullState(cull);
                        r_depth_state_t depthGuard;
                        qboolean depthChanged = R_ApplyDepthState(depthTest, depthWrite, depthFunc, &depthGuard);

                        if (currentColorMask != colorMask)
                        {
                                GL_ColorMaskiFunc(0,
                                             (colorMask & 1) ? GL_TRUE : GL_FALSE,
                                             (colorMask & 2) ? GL_TRUE : GL_FALSE,
                                             (colorMask & 4) ? GL_TRUE : GL_FALSE,
                                             (colorMask & 8) ? GL_TRUE : GL_FALSE);
                                currentColorMask = colorMask;
                        }

                        if (currentBlendEnabled != blendEnable)
                        {
                                if (blendEnable)
                                        glEnable(GL_BLEND);
                                else
                                        glDisable(GL_BLEND);
                                currentBlendEnabled = blendEnable;
                        }

                        if (currentBlendSrcRGB != (GLint)blendSrc || currentBlendDstRGB != (GLint)blendDst ||
                            currentBlendSrcAlpha != (GLint)blendSrc || currentBlendDstAlpha != (GLint)blendDst)
                        {
                                glBlendFunc(blendSrc, blendDst);
                                currentBlendSrcRGB = (GLint)blendSrc;
                                currentBlendDstRGB = (GLint)blendDst;
                                currentBlendSrcAlpha = (GLint)blendSrc;
                                currentBlendDstAlpha = (GLint)blendDst;
                        }

			const size_t offset = dstcmdofs + (size_t)i * (size_t)stride;
			const void *indirect = (const void *)(uintptr_t)offset;
			GL_MultiDrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, indirect, count, stride);

                        if (depthChanged)
                                R_PopDepthState(&depthGuard);
			R_PopCullState(&guard);
			i += count;
		}
	}
	else
	{
		int i;

		GL_Upload (GL_SHADER_STORAGE_BUFFER, &bmodel_calls.bound.params, sizeof (bmodel_calls.bound.params[0]) * num_bmodel_calls, &buf, &ofs);
		GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof (bmodel_calls.bound.params[0]) * num_bmodel_calls);

        for (i = 0; i < num_bmodel_calls; i++)
        {
                GLboolean blendEnable = bmodel_call_blend_enable[i];
                GLenum blendSrc = bmodel_call_blend_src[i];
                GLenum blendDst = bmodel_call_blend_dst[i];
                GLubyte colorMask = bmodel_call_color_mask[i];
                GLint depthFunc = bmodel_call_depth_func_override[i] ? (GLint)bmodel_call_depth_func[i] : -1;
                r_cull_state_t guard;
                R_PushCullState(&guard);
                R_SetCullState(bmodel_call_cull[i]);
                r_depth_state_t depthGuard;
                qboolean depthChanged = R_ApplyDepthState(bmodel_call_depth_test[i], bmodel_call_depth_write[i],
                        depthFunc, &depthGuard);

                if (currentColorMask != colorMask)
                {
                        GL_ColorMaskiFunc(0,
                                     (colorMask & 1) ? GL_TRUE : GL_FALSE,
                                     (colorMask & 2) ? GL_TRUE : GL_FALSE,
                                     (colorMask & 4) ? GL_TRUE : GL_FALSE,
                                     (colorMask & 8) ? GL_TRUE : GL_FALSE);
                        currentColorMask = colorMask;
                }

                if (currentBlendEnabled != blendEnable)
                {
                        if (blendEnable)
                                glEnable(GL_BLEND);
                        else
                                glDisable(GL_BLEND);
                        currentBlendEnabled = blendEnable;
                }

                if (currentBlendSrcRGB != (GLint)blendSrc || currentBlendDstRGB != (GLint)blendDst ||
                    currentBlendSrcAlpha != (GLint)blendSrc || currentBlendDstAlpha != (GLint)blendDst)
                {
                        glBlendFunc(blendSrc, blendDst);
                        currentBlendSrcRGB = (GLint)blendSrc;
                        currentBlendDstRGB = (GLint)blendDst;
                        currentBlendSrcAlpha = (GLint)blendSrc;
                        currentBlendDstAlpha = (GLint)blendDst;
                }

                GL_Uniform1iFunc (0, i);
                GL_BindTextures (0, 2, bmodel_calls.bound.textures[i]);
                GL_Bind (GL_TEXTURE4, bmodel_calls.bound.textures[i][2]);
                GL_Bind (GL_TEXTURE5, bmodel_calls.bound.textures[i][3]);
                GL_BindTextures (6, MAX_EFFECT_STAGES, &bmodel_calls.bound.textures[i][4]);
			const size_t offset = dstcmdofs + (size_t)i * sizeof (bmodel_draw_indirect_t);
			GL_DrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, (const void *)(uintptr_t)offset);

			if (depthChanged)
				R_PopDepthState(&depthGuard);
			R_PopCullState(&guard);
		}

        }

        if (currentColorMask != initialMaskBits)
        {
                GL_ColorMaskiFunc(0,
                             (initialMaskBits & 1) ? GL_TRUE : GL_FALSE,
                             (initialMaskBits & 2) ? GL_TRUE : GL_FALSE,
                             (initialMaskBits & 4) ? GL_TRUE : GL_FALSE,
                             (initialMaskBits & 8) ? GL_TRUE : GL_FALSE);
                currentColorMask = initialMaskBits;
        }

        if (currentBlendSrcRGB != initialBlendSrcRGB || currentBlendDstRGB != initialBlendDstRGB ||
            currentBlendSrcAlpha != initialBlendSrcAlpha || currentBlendDstAlpha != initialBlendDstAlpha)
        {
                GL_BlendFuncSeparateFunc(initialBlendSrcRGB, initialBlendDstRGB,
                                     initialBlendSrcAlpha, initialBlendDstAlpha);
                currentBlendSrcRGB = initialBlendSrcRGB;
                currentBlendDstRGB = initialBlendDstRGB;
                currentBlendSrcAlpha = initialBlendSrcAlpha;
                currentBlendDstAlpha = initialBlendDstAlpha;
        }

        if (currentBlendEnabled != initialBlendEnabled)
        {
                if (initialBlendEnabled)
                        glEnable(GL_BLEND);
                else
                        glDisable(GL_BLEND);
                currentBlendEnabled = initialBlendEnabled;
        }

        num_bmodel_calls = 0;
}

static void R_PushDepthState(r_depth_state_t *state)
{
        if (!state)
                return;

        state->testEnabled = glIsEnabled(GL_DEPTH_TEST);
        GLboolean mask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
        state->writeMask = mask;
        GLint func = GL_LEQUAL;
        glGetIntegerv(GL_DEPTH_FUNC, &func);
        state->func = func;
}

static qboolean R_ApplyDepthState(GLbyte depthTestOverride, GLbyte depthWriteOverride, GLint depthFuncOverride, r_depth_state_t *state)
{
        if (depthTestOverride < 0 && depthWriteOverride < 0 && depthFuncOverride < 0)
                return false;

        R_PushDepthState(state);

        if (depthTestOverride >= 0)
        {
                if (depthTestOverride)
                        glEnable(GL_DEPTH_TEST);
                else
                        glDisable(GL_DEPTH_TEST);
        }

        if (depthWriteOverride >= 0)
                glDepthMask(depthWriteOverride ? GL_TRUE : GL_FALSE);

        if (depthFuncOverride >= 0)
                glDepthFunc((GLenum)depthFuncOverride);

        const char *depthTestStr = ((depthTestOverride >= 0 ? depthTestOverride : state->testEnabled) ? "on" : "off");
        const char *depthWriteStr = ((depthWriteOverride >= 0 ? depthWriteOverride : state->writeMask) ? "on" : "off");
        GLenum func = (depthFuncOverride >= 0) ? (GLenum)depthFuncOverride : (GLenum)state->func;
        const char *depthFuncStr;
        switch (func)
        {
        case GL_NEVER: depthFuncStr = "never"; break;
        case GL_LESS: depthFuncStr = "less"; break;
        case GL_EQUAL: depthFuncStr = "equal"; break;
        case GL_LEQUAL: depthFuncStr = "lequal"; break;
        case GL_GREATER: depthFuncStr = "greater"; break;
        case GL_NOTEQUAL: depthFuncStr = "notequal"; break;
        case GL_GEQUAL: depthFuncStr = "gequal"; break;
        case GL_ALWAYS:
        default:
                depthFuncStr = "always";
                break;
        }
        IW_Debugf("depthtest=%s, depthwrite=%s, depthfunc=%s", depthTestStr, depthWriteStr, depthFuncStr);

        return true;
}

static void R_PopDepthState(const r_depth_state_t *state)
{
        if (!state)
                return;

        if (state->testEnabled)
                glEnable(GL_DEPTH_TEST);
        else
                glDisable(GL_DEPTH_TEST);

        glDepthMask(state->writeMask ? GL_TRUE : GL_FALSE);
        glDepthFunc((GLenum)state->func);
}

#define CALLFLAG_EMISSIVE        (1u << 3)
#define CALLFLAG_ALPHA_TEST      (1u << 4)
#define CALLFLAG_TC_STRETCH      (1u << 5)
#define CALLFLAG_TC_TURB         (1u << 6)
#define CALLFLAG_TC_ENVMAP       (1u << 7)
#define CALLFLAG_CUSTOM_FOG      (1u << 8)
#define CALLFLAG_EFFECTS         (1u << 9)

typedef struct
{
        const char      *suffix[6];
        int             remap[6];
        qboolean        insert_underscore;
} iw_cubemap_pattern_t;

static gltexture_t *R_IWShader_LoadCubeMapTexture(const texture_t *base, const char *path)
{
        static const iw_cubemap_pattern_t patterns[] = {
                {{"px", "nx", "py", "ny", "pz", "nz"}, {0, 1, 2, 3, 4, 5}, true},
                {{"posx", "negx", "posy", "negy", "posz", "negz"}, {0, 1, 2, 3, 4, 5}, true},
                {{"right", "left", "front", "back", "up", "down"}, {0, 1, 2, 3, 4, 5}, true},
                {{"rt", "bk", "lf", "ft", "up", "dn"}, {3, 1, 4, 5, 0, 2}, true},
                {{"_px", "_nx", "_py", "_ny", "_pz", "_nz"}, {0, 1, 2, 3, 4, 5}, false}
        };

        if (!path || !*path)
                return NULL;

        qmodel_t *owner = (base && base->gltexture) ? base->gltexture->owner : NULL;
        gltexture_t *existing = TexMgr_FindTexture(owner, path);
        if (!existing)
                existing = TexMgr_FindTexture(NULL, path);
        if (existing)
                return existing;

        char base_name[MAX_QPATH];
        q_strlcpy(base_name, path, sizeof(base_name));
        char *slash = strrchr(base_name, '/');
        char *dot = strrchr(base_name, '.');
        if (dot && (!slash || dot > slash))
                *dot = '\0';

        for (size_t pattern_index = 0; pattern_index < Q_COUNTOF(patterns); ++pattern_index)
        {
                const iw_cubemap_pattern_t *pattern = &patterns[pattern_index];
                int attempt_mark = Hunk_LowMark();
                byte *faces[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
                int face_width = 0;
                int face_height = 0;
                qboolean success = true;

                for (int i = 0; i < 6; ++i)
                {
                        char face_path[MAX_QPATH];
                        const char *suffix = pattern->suffix[i];
                        if (!suffix)
                        {
                                success = false;
                                break;
                        }

                        if (pattern->insert_underscore)
                        {
                                size_t len = strlen(base_name);
                                const char *connector = "_";
                                if (len > 0)
                                {
                                        char last = base_name[len - 1];
                                        if (last == '_' || last == '-' || last == '/')
                                                connector = "";
                                }
                                q_snprintf(face_path, sizeof(face_path), "%s%s%s", base_name, connector, suffix);
                        }
                        else
                        {
                                q_snprintf(face_path, sizeof(face_path), "%s%s", base_name, suffix);
                        }

                        enum srcformat fmt;
                        int width = 0;
                        int height = 0;
                        byte *data = Image_LoadImage(face_path, &width, &height, &fmt);
                        if (!data || fmt != SRC_RGBA || width <= 0 || height <= 0 || width != height)
                        {
                                success = false;
                                break;
                        }
                        if (i == 0)
                        {
                                face_width = width;
                                face_height = height;
                        }
                        else if (width != face_width || height != face_height)
                        {
                                success = false;
                                break;
                        }
                        faces[i] = data;
                }

                if (success)
                {
                        byte **ordered = (byte **)Hunk_AllocNoFill(sizeof(byte *) * 6);
                        for (int i = 0; i < 6; ++i)
                        {
                                int src = pattern->remap[i];
                                if (src < 0 || src >= 6 || !faces[src])
                                {
                                        success = false;
                                        break;
                                }
                                ordered[i] = faces[src];
                        }

                        if (success)
                        {
                                unsigned flags = TEXPREF_CUBEMAP | TEXPREF_NOPICMIP | TEXPREF_MIPMAP | TEXPREF_ALPHA;
                                if (gl_bindless_able)
                                        flags |= TEXPREF_BINDLESS;
                                gltexture_t *cubemap = TexMgr_LoadImageEx(owner, path, face_width, face_height, 1,
                                                                           SRC_RGBA, (byte *)ordered, "", 0, flags);
                                Hunk_FreeToLowMark(attempt_mark);
                                return cubemap;
                        }
                }

                Hunk_FreeToLowMark(attempt_mark);
        }

        return NULL;
}

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
                fps *= IW_GetAnimRate();
                if (fps <= 0.f)
                        fps = 1.f;
                int frame = (int)floor(time * fps);
                frame %= stage->numAnimFrames;
                if (frame < 0)
                        frame += stage->numAnimFrames;
                if (frame >= 0 && frame < stage->numAnimFrames)
                        path = stage->animPaths[frame];
        }

        if (stage->mapType == IW_MAP_CUBEMAP)
                return R_IWShader_LoadCubeMapTexture(base, path);

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

static qboolean R_IWShader_FogFromParms(const iwFogParms_t *parms, float fog_color[4])
{
        if (!parms || !parms->hasColor)
                return false;

        float density = 0.f;
        if (parms->hasDensity)
        {
                density = parms->density;
        }
        else if (parms->hasDepthForOpaque && parms->depthForOpaque > 0.f)
        {
                density = 1.f / parms->depthForOpaque;
        }
        else
        {
                return false;
        }

        density = fabsf(density);
        fog_color[0] = q_clamp(parms->color[0], 0.f, 1.f);
        fog_color[1] = q_clamp(parms->color[1], 0.f, 1.f);
        fog_color[2] = q_clamp(parms->color[2], 0.f, 1.f);
        fog_color[3] = density * density;
        return fog_color[3] > 0.f;
}

static qboolean R_IWShader_GetFogColor(const iwMaterial_t *material, float fog_color[4], int depth)
{
        if (!material || depth > 4)
                return false;

        if (material->hasFogParms && R_IWShader_FogFromParms(&material->fogParms, fog_color))
                return true;

        for (int i = 0; i < material->numStages; ++i)
        {
                const iwStage_t *stage = &material->stages[i];
                if (stage->hasFogParms && R_IWShader_FogFromParms(&stage->fogParms, fog_color))
                        return true;
        }

        if (material->hasFog && material->fogShader[0])
        {
                const iwMaterial_t *fog_material = IW_FindMaterial(material->fogShader);
                if (fog_material && fog_material != material)
                        return R_IWShader_GetFogColor(fog_material, fog_color, depth + 1);
        }

        return false;
}

/*
=============
R_AddBModelCall
=============
*/
static void R_AddBModelCall (int index, int first_instance, int num_instances, texture_t *t, qboolean zfix)
{
        GLuint          flags;
        float           alpha;
        gltexture_t     *tx, *fb, *em, *env;
        const iwMaterial_t *material = NULL;
        iwTexMatrix_t   tex_matrix;
        iwTexMatrix_t   emissive_matrix;
        float           emissive_color[3];
        float           fog_color[4] = { 0.f, 0.f, 0.f, 0.f };
        qboolean        has_material_fog = false;
        const iwStage_t *emissive_stage = NULL;
        const iwStage_t *env_stage = NULL;
        iwCull_t        cull = IW_CULL_BACK;
        float           base_tcmod_params0[4] = { 1.f, 0.f, 0.f, 0.f };
        float           base_tcmod_params1[4] = { 0.f, 0.f, -1.f, (float)IW_WAVE_SIN };
        float           tcgen_params[4] = { (float)IW_TCGEN_BASE, (float)IW_TC_ALIGN_OBJECT, 0.f, 0.f };
        float           tcgen_s[4] = { 1.f, 0.f, 0.f, 0.f };
        float           tcgen_t[4] = { 0.f, 1.f, 0.f, 0.f };
        unsigned int    tc_feature_flags = 0u;
        GLbyte           depthTestOverride = -1;
        GLbyte           depthWriteOverride = -1;
        GLboolean       stageBlendEnabled = GL_FALSE;
        GLenum          stageBlendSrc = GL_ONE;
        GLenum          stageBlendDst = GL_ZERO;
        GLint           stageDepthFunc = -1;
        GLubyte         stageColorMaskBits = IW_ColorMaskBits(IW_COLORMASK_RGBA);
        float           alpha_params0[4] = { 0.f, 1.f, 0.f, 0.f };
        float           alpha_params1[4] = { 0.f, 0.f, -1.f, (float)IW_WAVE_SIN };
        float           alpha_params2[4] = { (float)IW_ALPHA_FUNC_DISABLED, 0.f, 0.f, 0.f };
        qboolean        stageAlphaTest = false;
        iwAlphaFunc_t   stageAlphaFuncMode = IW_ALPHA_FUNC_DISABLED;
        float           stageAlphaFuncRef = 0.f;
        float           env_params0[4] = { 1.f, 1.f, 1.f, 0.f };
        float           env_params1[4] = { 1.f, 0.f, 0.f, 1.f };
        float           env_params2[4] = { (float)IW_WAVE_SIN, (float)IW_RGB_IDENTITY, (float)IW_BLEND_NONE, 0.f };
        float           effect_info[4] = { 0.f, 0.f, 0.f, 0.f };
        float           effect_matrix[MAX_EFFECT_STAGES][4];
        float           effect_translate[MAX_EFFECT_STAGES][4];
        float           effect_color[MAX_EFFECT_STAGES][4];
        float           effect_alpha_params0[MAX_EFFECT_STAGES][4];
        float           effect_alpha_params1[MAX_EFFECT_STAGES][4];
        float           effect_alpha_params2[MAX_EFFECT_STAGES][4];
        float           effect_tcmod_params0[MAX_EFFECT_STAGES][4];
        float           effect_tcmod_params1[MAX_EFFECT_STAGES][4];
        float           effect_flags_array[MAX_EFFECT_STAGES][4];
        gltexture_t     *effect_textures[MAX_EFFECT_STAGES] = { NULL };
        GLuint64        effect_handles[MAX_EFFECT_STAGES] = { 0 };
        int             effect_stage_count = 0;

        memset(effect_matrix, 0, sizeof(effect_matrix));
        memset(effect_translate, 0, sizeof(effect_translate));
        memset(effect_color, 0, sizeof(effect_color));
        memset(effect_alpha_params0, 0, sizeof(effect_alpha_params0));
        memset(effect_alpha_params1, 0, sizeof(effect_alpha_params1));
        memset(effect_alpha_params2, 0, sizeof(effect_alpha_params2));
        memset(effect_tcmod_params0, 0, sizeof(effect_tcmod_params0));
        memset(effect_tcmod_params1, 0, sizeof(effect_tcmod_params1));
        memset(effect_flags_array, 0, sizeof(effect_flags_array));

        IW_TexMatrixIdentity (&tex_matrix);
        IW_TexMatrixIdentity (&emissive_matrix);
        R_IWShader_EmissiveColor (NULL, emissive_color);
        env = NULL;

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
                cull = material->cull;
                has_material_fog = R_IWShader_GetFogColor(material, fog_color, 0);
                IW_MaterialTexMatrix (material, r_framedata.time, &tex_matrix);

                if (material->numStages > 0)
                {
                        const iwStage_t *base_stage = &material->stages[0];

                        for (int s = 0; s < material->numStages; ++s)
                        {
                                const iwStage_t *stage = &material->stages[s];
                                if (!stage)
                                        continue;
                                if (!emissive_stage && stage->emissive)
                                        emissive_stage = stage;

                                if (!env_stage)
                                {
                                        qboolean wantsEnv = (stage->mapType == IW_MAP_CUBEMAP);
                                        if (!wantsEnv && stage->tcGen[0] && q_strcasestr(stage->tcGen, "environment"))
                                                wantsEnv = true;
                                        if (!wantsEnv)
                                        {
                                                for (int tc = 0; tc < stage->numTCMods; ++tc)
                                                {
                                                        if (stage->tcmods[tc].op == IW_TC_ENVMAP)
                                                        {
                                                                wantsEnv = true;
                                                                break;
                                                        }
                                                }
                                        }
                                        if (wantsEnv)
                                                env_stage = stage;
                                }
                        }

                        if (material->numStages > 1)
                        {
                                for (int s = 1; s < material->numStages && effect_stage_count < MAX_EFFECT_STAGES; ++s)
                                {
                                        const iwStage_t *stage = &material->stages[s];
                                        if (!stage || stage == emissive_stage || stage == env_stage)
                                                continue;
                                        if (stage->mapType == IW_MAP_CUBEMAP)
                                                continue;
                                        if (stage->blendMode != IW_BLEND_ADD && stage->blendMode != IW_BLEND_ADD_ALPHA && stage->blendMode != IW_BLEND_PREMUL)
                                                continue;
                                        if (stage->rgbgen != IW_RGB_CONST && stage->rgbgen != IW_RGB_IDENTITY)
                                                continue;

                                        gltexture_t *stage_tex = R_IWShader_FindStageTexture(t, stage, r_framedata.time);
                                        if (!stage_tex)
                                                stage_tex = blacktexture;

                                        float local_matrix[4] = { 1.f, 0.f, 0.f, 1.f };
                                        float local_translate[4] = { 0.f, 0.f, 0.f, 0.f };
                                        float local_color[4] = { 1.f, 1.f, 1.f, 1.f };
                                        float local_alpha0[4] = { 0.f, 1.f, stage->alphaWave.base, stage->alphaWave.amplitude };
                                        float local_alpha1[4] = { stage->alphaWave.phase, stage->alphaWave.frequency, stage->maskExplicit ? (float)stage->mask : -1.f, (float)stage->alphaWave.func };
                                        float local_alpha2[4] = { (float)stage->alphaFuncMode, q_clamp(stage->alphaFuncRef, 0.f, 1.f), 0.f, stage->alphaToCoverage ? 1.f : 0.f };
                                        float local_tc0[4] = { 1.f, 0.f, 0.f, 0.f };
                                        float local_tc1[4] = { 0.f, 0.f, stage->maskExplicit ? (float)stage->mask : -1.f, (float)IW_WAVE_SIN };
                                        iwTexMatrix_t stage_matrix;
                                        IW_TexMatrixIdentity(&stage_matrix);
                                        if (IW_StageTexMatrix(stage, r_framedata.time, &stage_matrix))
                                        {
                                                local_matrix[0] = stage_matrix.matrix[0];
                                                local_matrix[1] = stage_matrix.matrix[1];
                                                local_matrix[2] = stage_matrix.matrix[2];
                                                local_matrix[3] = stage_matrix.matrix[3];
                                                local_translate[0] = stage_matrix.translate[0];
                                                local_translate[1] = stage_matrix.translate[1];
                                        }
                                        unsigned int stageFlags = 0u;
                                        for (int tc = 0; tc < stage->numTCMods; ++tc)
                                        {
                                                const iwTCMod_t *mod = &stage->tcmods[tc];
                                                switch (mod->op)
                                                {
                                                case IW_TC_STRETCH:
                                                        stageFlags |= CALLFLAG_TC_STRETCH;
                                                        local_tc0[0] = mod->wave.base;
                                                        local_tc0[1] = mod->wave.amplitude;
                                                        local_tc0[2] = mod->wave.phase;
                                                        local_tc0[3] = mod->wave.frequency;
                                                        local_tc1[3] = (float)mod->wave.func;
                                                        break;

                                                case IW_TC_TURB:
                                                        stageFlags |= CALLFLAG_TC_TURB;
                                                        local_tc1[0] = mod->a;
                                                        local_tc1[1] = mod->b;
                                                        break;

                                                case IW_TC_ENVMAP:
                                                        stageFlags |= CALLFLAG_TC_ENVMAP;
                                                        break;

                                                default:
                                                        break;
                                                }
                                        }
                                        if ((stageFlags & CALLFLAG_TC_ENVMAP) != 0u)
                                                continue;
                                        switch (stage->alphagen)
                                        {
                                        case IW_A_CONST:
                                                local_alpha0[0] = 1.f;
                                                local_alpha0[1] = q_clamp(stage->aConst, 0.f, 1.f);
                                                break;
                                        case IW_A_ENTITY:
                                                local_alpha0[0] = 2.f;
                                                break;
                                        case IW_A_WAVE:
                                                local_alpha0[0] = 3.f;
                                                break;
                                        case IW_A_MASK:
                                                local_alpha0[0] = 4.f;
                                                break;
                                        default:
                                                local_alpha0[0] = 0.f;
                                                break;
                                        }
                                        if (stage->rgbgen == IW_RGB_CONST)
                                        {
                                                local_color[0] = stage->rgbConst[0];
                                                local_color[1] = stage->rgbConst[1];
                                                local_color[2] = stage->rgbConst[2];
                                        }

                                        int idx = effect_stage_count;
                                        effect_textures[idx] = stage_tex ? stage_tex : blacktexture;
                                        effect_handles[idx] = (stage_tex && stage_tex->bindless_handle) ? stage_tex->bindless_handle : (blacktexture ? blacktexture->bindless_handle : 0);
                                        memcpy(effect_matrix[idx], local_matrix, sizeof(local_matrix));
                                        memcpy(effect_translate[idx], local_translate, sizeof(local_translate));
                                        memcpy(effect_color[idx], local_color, sizeof(local_color));
                                        memcpy(effect_alpha_params0[idx], local_alpha0, sizeof(local_alpha0));
                                        memcpy(effect_alpha_params1[idx], local_alpha1, sizeof(local_alpha1));
                                        memcpy(effect_alpha_params2[idx], local_alpha2, sizeof(local_alpha2));
                                        memcpy(effect_tcmod_params0[idx], local_tc0, sizeof(local_tc0));
                                        memcpy(effect_tcmod_params1[idx], local_tc1, sizeof(local_tc1));
                                        effect_flags_array[idx][0] = (float)stageFlags;
                                        effect_flags_array[idx][1] = (float)stage->blendMode;
                                        ++effect_stage_count;
                                }
                        }

                        effect_info[0] = (float)effect_stage_count;

                        for (int i = 0; i < MAX_EFFECT_STAGES; ++i)
                        {
                                if (!effect_textures[i])
                                        effect_textures[i] = blacktexture;
                                if (effect_handles[i] == 0 && blacktexture)
                                        effect_handles[i] = blacktexture->bindless_handle;
                        }

                        tcgen_params[0] = (float)base_stage->tcGenMode;
                        tcgen_params[1] = (float)base_stage->tcAlign;
                        tcgen_s[0] = base_stage->tcGenVectors[0][0];
                        tcgen_s[1] = base_stage->tcGenVectors[0][1];
                        tcgen_s[2] = base_stage->tcGenVectors[0][2];
                        tcgen_t[0] = base_stage->tcGenVectors[1][0];
                        tcgen_t[1] = base_stage->tcGenVectors[1][1];
                        tcgen_t[2] = base_stage->tcGenVectors[1][2];
                        if (base_stage->depthTestExplicit)
                                depthTestOverride = base_stage->depthTest ? 1 : 0;
                        if (base_stage->depthWrite != IW_DEPTHWRITE_AUTO)
                                depthWriteOverride = base_stage->depthWrite ? 1 : 0;
                        if (base_stage->depthFuncExplicit && base_stage->depthFuncMode != IW_DEPTHFUNC_DEFAULT)
                                stageDepthFunc = IW_DepthFuncToGL(base_stage->depthFuncMode);

                        switch (base_stage->blendMode)
                        {
                        case IW_BLEND_ALPHA:
                                stageBlendEnabled = GL_TRUE;
                                stageBlendSrc = GL_SRC_ALPHA;
                                stageBlendDst = GL_ONE_MINUS_SRC_ALPHA;
                                break;

                        case IW_BLEND_ADD:
                                stageBlendEnabled = GL_TRUE;
                                stageBlendSrc = GL_ONE;
                                stageBlendDst = GL_ONE;
                                break;

                        case IW_BLEND_MUL:
                                stageBlendEnabled = GL_TRUE;
                                stageBlendSrc = GL_DST_COLOR;
                                stageBlendDst = GL_ZERO;
                                break;

                        case IW_BLEND_PREMUL:
                                stageBlendEnabled = GL_TRUE;
                                stageBlendSrc = GL_ONE;
                                stageBlendDst = GL_ONE_MINUS_SRC_ALPHA;
                                break;

                        case IW_BLEND_ADD_ALPHA:
                                stageBlendEnabled = GL_TRUE;
                                stageBlendSrc = GL_SRC_ALPHA;
                                stageBlendDst = GL_ONE;
                                break;

                        case IW_BLEND_CUSTOM:
                                stageBlendEnabled = GL_TRUE;
                                stageBlendSrc = IW_BlendFactorToGL(base_stage->src);
                                stageBlendDst = IW_BlendFactorToGL(base_stage->dst);
                                break;

                        default:
                                stageBlendEnabled = GL_FALSE;
                                stageBlendSrc = GL_ONE;
                                stageBlendDst = GL_ZERO;
                                break;
                        }

                        stageColorMaskBits = IW_ColorMaskBits(base_stage->colorMask);

                        alpha_params0[0] = 0.f;
                        alpha_params0[1] = 1.f;
                        alpha_params0[2] = base_stage->alphaWave.base;
                        alpha_params0[3] = base_stage->alphaWave.amplitude;
                        alpha_params1[0] = base_stage->alphaWave.phase;
                        alpha_params1[1] = base_stage->alphaWave.frequency;
                        alpha_params1[2] = (float)base_stage->mask;
                        alpha_params1[3] = (float)base_stage->alphaWave.func;

                        switch (base_stage->alphagen)
                        {
                        case IW_A_CONST:
                                alpha_params0[0] = 1.f;
                                alpha_params0[1] = q_clamp(base_stage->aConst, 0.f, 1.f);
                                break;

                        case IW_A_ENTITY:
                                alpha_params0[0] = 2.f;
                                break;

                        case IW_A_WAVE:
                                alpha_params0[0] = 3.f;
                                break;

                        case IW_A_MASK:
                                alpha_params0[0] = 4.f;
                                break;

                        default:
                                alpha_params0[0] = 0.f;
                                break;
                        }

                        stageAlphaFuncMode = base_stage->alphaFuncMode;
                        stageAlphaFuncRef = q_clamp(base_stage->alphaFuncRef, 0.f, 1.f);
                        if (stageAlphaFuncMode != IW_ALPHA_FUNC_DISABLED && stageAlphaFuncMode != IW_ALPHA_FUNC_ALWAYS)
                                stageAlphaTest = true;
                        else
                                stageAlphaTest = (stageAlphaFuncMode == IW_ALPHA_FUNC_NEVER);

                        alpha_params2[0] = (float)stageAlphaFuncMode;
                        alpha_params2[1] = stageAlphaFuncRef;
                        alpha_params2[2] = stageAlphaTest ? 1.f : 0.f;
                        alpha_params2[3] = base_stage->alphaToCoverage ? 1.f : 0.f;

                        if (base_stage->maskExplicit)
                                base_tcmod_params1[2] = (float)base_stage->mask;

                        for (int tc = 0; tc < base_stage->numTCMods; ++tc)
                        {
                                const iwTCMod_t *mod = &base_stage->tcmods[tc];
                                switch (mod->op)
                                {
                                case IW_TC_STRETCH:
                                        tc_feature_flags |= CALLFLAG_TC_STRETCH;
                                        base_tcmod_params0[0] = mod->wave.base;
                                        base_tcmod_params0[1] = mod->wave.amplitude;
                                        base_tcmod_params0[2] = mod->wave.phase;
                                        base_tcmod_params0[3] = mod->wave.frequency;
                                        base_tcmod_params1[3] = (float)mod->wave.func;
                                        break;

                                case IW_TC_TURB:
                                        tc_feature_flags |= CALLFLAG_TC_TURB;
                                        base_tcmod_params1[0] = mod->a;
                                        base_tcmod_params1[1] = mod->b;
                                        break;

                                case IW_TC_ENVMAP:
                                        tc_feature_flags |= CALLFLAG_TC_ENVMAP;
                                        break;

                                default:
                                        break;
                                }
                        }

                        if (env_stage)
                        {
                                gltexture_t *candidate = R_IWShader_FindStageTexture(t, env_stage, r_framedata.time);
                                if (candidate && candidate->target == GL_TEXTURE_CUBE_MAP)
                                {
                                        env = candidate;
                                        env_params0[0] = env_stage->rgbConst[0];
                                        env_params0[1] = env_stage->rgbConst[1];
                                        env_params0[2] = env_stage->rgbConst[2];
                                        env_params0[3] = q_max(env_stage->specularScale, 0.f);
                                        env_params1[0] = env_stage->rgbWave.base;
                                        env_params1[1] = env_stage->rgbWave.amplitude;
                                        env_params1[2] = env_stage->rgbWave.phase;
                                        env_params1[3] = env_stage->rgbWave.frequency;
                                        env_params2[0] = (float)env_stage->rgbWave.func;
                                        env_params2[1] = (float)env_stage->rgbgen;
                                        env_params2[2] = (float)env_stage->blendMode;
                                        tc_feature_flags |= CALLFLAG_TC_ENVMAP;
                                }
                                else
                                {
                                        env_stage = NULL;
                                }
                        }

                        if (!env_stage)
                                tc_feature_flags &= ~CALLFLAG_TC_ENVMAP;
                }
        }
        else
        {
                for (int i = 0; i < MAX_EFFECT_STAGES; ++i)
                {
                        if (!effect_textures[i])
                                effect_textures[i] = blacktexture;
                        if (effect_handles[i] == 0 && blacktexture)
                                effect_handles[i] = blacktexture->bindless_handle;
                }
        }
        if (!stageAlphaTest && t && t->type == TEXTYPE_CUTOUT)
        {
                stageAlphaTest = true;
                stageAlphaFuncMode = IW_ALPHA_FUNC_GEQUAL;
                stageAlphaFuncRef = 2.f / 3.f;
        }

        stageAlphaFuncRef = q_clamp(stageAlphaFuncRef, 0.f, 1.f);
        alpha_params2[0] = (float)stageAlphaFuncMode;
        alpha_params2[1] = stageAlphaFuncRef;
        alpha_params2[2] = stageAlphaTest ? 1.f : 0.f;

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
        if (effect_stage_count > 0)
                flags |= CALLFLAG_EFFECTS;
        if (em != NULL)
                flags |= CALLFLAG_EMISSIVE;
        if (stageAlphaTest)
                flags |= CALLFLAG_ALPHA_TEST;
        flags |= tc_feature_flags;
        if (has_material_fog)
                flags |= CALLFLAG_CUSTOM_FOG;
        alpha = t ? GL_WaterAlphaForTextureType (t->type) : 1.f;

        if (gl_bindless_able)
        {
                bmodel_bindless_gpu_call_t *call = &bmodel_calls.bindless.params[num_bmodel_calls];
                call->flags = flags;
                call->alpha = alpha;
                call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
                call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
                call->emissive = em ? em->bindless_handle : blacktexture->bindless_handle;
                call->envmap = env ? env->bindless_handle : 0;
                memcpy (call->tcmod_matrix, tex_matrix.matrix, sizeof (tex_matrix.matrix));
                call->tcmod_translate[0] = tex_matrix.translate[0];
                call->tcmod_translate[1] = tex_matrix.translate[1];
                call->tcmod_translate[2] = 0.f;
                call->tcmod_translate[3] = 0.f;
                memcpy (call->tcmod_params0, base_tcmod_params0, sizeof (base_tcmod_params0));
                memcpy (call->tcmod_params1, base_tcmod_params1, sizeof (base_tcmod_params1));
                memcpy (call->tcgen_params, tcgen_params, sizeof (tcgen_params));
                memcpy (call->tcgen_s, tcgen_s, sizeof (tcgen_s));
                memcpy (call->tcgen_t, tcgen_t, sizeof (tcgen_t));
                memcpy (call->emissive_matrix, emissive_matrix.matrix, sizeof (emissive_matrix.matrix));
                call->emissive_translate[0] = emissive_matrix.translate[0];
                call->emissive_translate[1] = emissive_matrix.translate[1];
                call->emissive_translate[2] = 0.f;
                call->emissive_translate[3] = 0.f;
                call->emissive_color[0] = emissive_color[0];
                call->emissive_color[1] = emissive_color[1];
                call->emissive_color[2] = emissive_color[2];
                call->emissive_color[3] = (flags & CALLFLAG_EMISSIVE) ? 1.f : 0.f;
                memcpy (call->fog_color, fog_color, sizeof (fog_color));
                memcpy (call->alpha_params0, alpha_params0, sizeof (alpha_params0));
                memcpy (call->alpha_params1, alpha_params1, sizeof (alpha_params1));
                memcpy (call->alpha_params2, alpha_params2, sizeof (alpha_params2));
                memcpy (call->env_params0, env_params0, sizeof (env_params0));
                memcpy (call->env_params1, env_params1, sizeof (env_params1));
                memcpy (call->env_params2, env_params2, sizeof (env_params2));
                memcpy (call->effect_info, effect_info, sizeof (effect_info));
                memcpy (call->effect_matrix, effect_matrix, sizeof (effect_matrix));
                memcpy (call->effect_translate, effect_translate, sizeof (effect_translate));
                memcpy (call->effect_color, effect_color, sizeof (effect_color));
                memcpy (call->effect_alpha_params0, effect_alpha_params0, sizeof (effect_alpha_params0));
                memcpy (call->effect_alpha_params1, effect_alpha_params1, sizeof (effect_alpha_params1));
                memcpy (call->effect_alpha_params2, effect_alpha_params2, sizeof (effect_alpha_params2));
                memcpy (call->effect_tcmod_params0, effect_tcmod_params0, sizeof (effect_tcmod_params0));
                memcpy (call->effect_tcmod_params1, effect_tcmod_params1, sizeof (effect_tcmod_params1));
                memcpy (call->effect_flags, effect_flags_array, sizeof (effect_flags_array));
                for (int i = 0; i < MAX_EFFECT_STAGES; ++i)
                        call->effect_handles[i] = effect_handles[i];
        }
        else
        {
                bmodel_bound_gpu_call_t *call = &bmodel_calls.bound.params[num_bmodel_calls];
                gltexture_t **textures = bmodel_calls.bound.textures[num_bmodel_calls];
                call->flags = flags;
                call->alpha = alpha;
                call->baseinstance = first_instance;
                call->padding = 0;
                memcpy (call->tcmod_matrix, tex_matrix.matrix, sizeof (tex_matrix.matrix));
                call->tcmod_translate[0] = tex_matrix.translate[0];
                call->tcmod_translate[1] = tex_matrix.translate[1];
                call->tcmod_translate[2] = 0.f;
                call->tcmod_translate[3] = 0.f;
                memcpy (call->tcmod_params0, base_tcmod_params0, sizeof (base_tcmod_params0));
                memcpy (call->tcmod_params1, base_tcmod_params1, sizeof (base_tcmod_params1));
                memcpy (call->tcgen_params, tcgen_params, sizeof (tcgen_params));
                memcpy (call->tcgen_s, tcgen_s, sizeof (tcgen_s));
                memcpy (call->tcgen_t, tcgen_t, sizeof (tcgen_t));
                memcpy (call->emissive_matrix, emissive_matrix.matrix, sizeof (emissive_matrix.matrix));
                call->emissive_translate[0] = emissive_matrix.translate[0];
                call->emissive_translate[1] = emissive_matrix.translate[1];
                call->emissive_translate[2] = 0.f;
                call->emissive_translate[3] = 0.f;
                call->emissive_color[0] = emissive_color[0];
                call->emissive_color[1] = emissive_color[1];
                call->emissive_color[2] = emissive_color[2];
                call->emissive_color[3] = (flags & CALLFLAG_EMISSIVE) ? 1.f : 0.f;
                memcpy (call->fog_color, fog_color, sizeof (fog_color));
                memcpy (call->alpha_params0, alpha_params0, sizeof (alpha_params0));
                memcpy (call->alpha_params1, alpha_params1, sizeof (alpha_params1));
                memcpy (call->alpha_params2, alpha_params2, sizeof (alpha_params2));
                memcpy (call->env_params0, env_params0, sizeof (env_params0));
                memcpy (call->env_params1, env_params1, sizeof (env_params1));
                memcpy (call->env_params2, env_params2, sizeof (env_params2));
                memcpy (call->effect_info, effect_info, sizeof (effect_info));
                memcpy (call->effect_matrix, effect_matrix, sizeof (effect_matrix));
                memcpy (call->effect_translate, effect_translate, sizeof (effect_translate));
                memcpy (call->effect_color, effect_color, sizeof (effect_color));
                memcpy (call->effect_alpha_params0, effect_alpha_params0, sizeof (effect_alpha_params0));
                memcpy (call->effect_alpha_params1, effect_alpha_params1, sizeof (effect_alpha_params1));
                memcpy (call->effect_alpha_params2, effect_alpha_params2, sizeof (effect_alpha_params2));
                memcpy (call->effect_tcmod_params0, effect_tcmod_params0, sizeof (effect_tcmod_params0));
                memcpy (call->effect_tcmod_params1, effect_tcmod_params1, sizeof (effect_tcmod_params1));
                memcpy (call->effect_flags, effect_flags_array, sizeof (effect_flags_array));
                textures[0] = tx ? tx : greytexture;
                textures[1] = fb ? fb : blacktexture;
                textures[2] = em ? em : blacktexture;
                textures[3] = env ? env : blacktexture;
                for (int i = 0; i < MAX_EFFECT_STAGES; ++i)
                        textures[4 + i] = effect_textures[i] ? effect_textures[i] : blacktexture;
        }

        SDL_assert (num_instances > 0);
        SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
        bmodel_call_depth_test[num_bmodel_calls] = depthTestOverride;
        bmodel_call_depth_write[num_bmodel_calls] = depthWriteOverride;
        bmodel_call_depth_func_override[num_bmodel_calls] = (stageDepthFunc >= 0) ? 1 : 0;
        bmodel_call_depth_func[num_bmodel_calls] = (stageDepthFunc >= 0) ? stageDepthFunc : GL_LEQUAL;
        bmodel_call_blend_enable[num_bmodel_calls] = stageBlendEnabled;
        bmodel_call_blend_src[num_bmodel_calls] = stageBlendSrc;
        bmodel_call_blend_dst[num_bmodel_calls] = stageBlendDst;
        bmodel_call_color_mask[num_bmodel_calls] = stageColorMaskBits;
        bmodel_call_cull[num_bmodel_calls] = cull;
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

typedef enum {
        BP_SOLID,
        BP_ALPHATEST,
        BP_SKYLAYERS,
        BP_SKYCUBEMAP,
        BP_SKYSTENCIL,
        BP_SHOWTRIS,
} brushpass_t;

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
                        R_AddBModelCall (model->firstcmd + j, baseinst, numinst, pass != BP_SHOWTRIS ? R_TextureAnimation (t, frame) : 0, zfix);
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
			R_AddBModelCall (model->firstcmd + j, baseinst, numinst, R_TextureAnimation (t, frame), !isworld);
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
