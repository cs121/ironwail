/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

//r_alias.c -- alias model rendering

#include "quakedef.h"
#include "glquake.h"
#include "r_framegraph.h"
#include "r_entitylight.h"
#include "lightgrid.h"
#include "r_realtimelight.h"
#include "r_skyvis.h"

extern cvar_t gl_overbright_models, gl_fullbrights, r_lerpmodels, r_lerpmove, r_model_halflambert; //johnfitz
extern cvar_t r_alias_q3lighting;
extern cvar_t r_tonemap;
extern cvar_t scr_fov, cl_gun_fovscale, cl_gun_x, cl_gun_y, cl_gun_z;
extern cvar_t r_oit;
extern float r_autoexposure_debug_exposure;

//up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz -- changed to an array of pointers

const float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

extern vec3_t	lightcolor; //johnfitz -- replaces "float shadelight" for lit support

static float	entalpha; //johnfitz

static qboolean R_AliasIsFinite (float v)
{
#if defined(_MSC_VER)
	return _finite (v) != 0;
#else
	return isfinite (v);
#endif
}

//johnfitz -- struct for passing lerp information to drawing functions
typedef struct {
	short pose1;
	short pose2;
	float blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;
//johnfitz

#define MAX_ALIAS_INSTANCES 256

typedef struct aliasinstance_s {
	float		worldmatrix[12];
	float		prev_worldmatrix[12];
	float		normalmatrix[12];
	vec3_t		lightcolor;
	float		alpha;
	vec3_t		dlightcolor;
	float		_pad0;
	vec3_t		dlightdir;
	float		_pad1;
	vec3_t		staticlightdir;
	float		_pad2;
	float		skyvisibility;
	float		_pad3[3];
	/* std430: vec3 has 16-byte base alignment in structs.
	 * After skyvisibility + _pad3, keep extra 12 bytes so Pose1 starts at byte 236
	 * (matching shader InstanceData layout), then pad struct stride to 256. */
	float		_pad_pose[3];
	int32_t		pose1;
	int32_t		pose2;
	float		blend;
	int32_t		flags;
	float		_pad_tail;
} aliasinstance_t;

#define ALIAS_INSTANCE_FLAG_NONE           0
#define ALIAS_INSTANCE_FLAG_NO_MOTION_BLUR (1 << 0)
#define ALIAS_INSTANCE_FLAG_VIEWMODEL      (1 << 1)
#define ALIAS_INSTANCE_FLAG_LIGHTNING      (1 << 2)
#define ALIAS_INSTANCE_FLAG_ROTATE         (1 << 3)

struct ibuf_s {
	int			count;
	entity_t	*ent;

	struct {
		float	matviewproj[16];
		float	prev_matviewproj[16];
		vec3_t	eyepos;
		float	_pad;
		vec4_t	fog;
		float	dither;
		float	overbright;
		float	half_lambert;
		float	dlight_debug_models;
		float	dlight_directional_mix;
		float	pp_dlight_model_enable;
		float	pp_dlight_model_debug;
		float	ambient_sky_params[4]; // x: enabled, y: scale, z: debug, w: unused
		float	ambient_sky_tint[4];   // rgb: tint, w: cap
		float	post_exposure;
		float	tonemap_mode;
		float	_pad1[1];
		/* std430 rounds the struct size up to 16-byte alignment before instances[].
		 * Keep explicit tail padding so CPU/GPU offsets match exactly. */
		float	_pad_tail[2];
	} global;
	aliasinstance_t inst[MAX_ALIAS_INSTANCES];
} ibuf;

COMPILE_TIME_ASSERT (alias_global_size_matches_std430, sizeof (ibuf.global) == 240);
COMPILE_TIME_ASSERT (alias_instance_size_matches_std430, sizeof (aliasinstance_t) == 256);
COMPILE_TIME_ASSERT (alias_instance_pose1_std430_offset, offsetof (aliasinstance_t, pose1) == 236);
COMPILE_TIME_ASSERT (alias_instance_pose2_std430_offset, offsetof (aliasinstance_t, pose2) == 240);
COMPILE_TIME_ASSERT (alias_instance_blend_std430_offset, offsetof (aliasinstance_t, blend) == 244);
COMPILE_TIME_ASSERT (alias_instance_flags_std430_offset, offsetof (aliasinstance_t, flags) == 248);

static qboolean r_alias_shadow_batch_dlight = false;

static void R_BuildAliasNormalMatrix (const float model_matrix[16], float out_normalmatrix[12])
{
	const float m00 = model_matrix[0],  m01 = model_matrix[4],  m02 = model_matrix[8];
	const float m10 = model_matrix[1],  m11 = model_matrix[5],  m12 = model_matrix[9];
	const float m20 = model_matrix[2],  m21 = model_matrix[6],  m22 = model_matrix[10];
	const float c00 =  (m11 * m22 - m12 * m21);
	const float c01 = -(m10 * m22 - m12 * m20);
	const float c02 =  (m10 * m21 - m11 * m20);
	const float c10 = -(m01 * m22 - m02 * m21);
	const float c11 =  (m00 * m22 - m02 * m20);
	const float c12 = -(m00 * m21 - m01 * m20);
	const float c20 =  (m01 * m12 - m02 * m11);
	const float c21 = -(m00 * m12 - m02 * m10);
	const float c22 =  (m00 * m11 - m01 * m10);
	const float det = m00 * c00 + m01 * c01 + m02 * c02;

	if (fabsf (det) > 1e-8f && R_AliasIsFinite (det))
	{
		const float invdet = 1.f / det;

		out_normalmatrix[0] = c00 * invdet;
		out_normalmatrix[1] = c10 * invdet;
		out_normalmatrix[2] = c20 * invdet;
		out_normalmatrix[3] = 0.f;
		out_normalmatrix[4] = c01 * invdet;
		out_normalmatrix[5] = c11 * invdet;
		out_normalmatrix[6] = c21 * invdet;
		out_normalmatrix[7] = 0.f;
		out_normalmatrix[8] = c02 * invdet;
		out_normalmatrix[9] = c12 * invdet;
		out_normalmatrix[10] = c22 * invdet;
		out_normalmatrix[11] = 0.f;
		return;
	}

	{
		vec3_t axis_x = { m00, m10, m20 };
		vec3_t axis_y = { m01, m11, m21 };
		vec3_t axis_z = { m02, m12, m22 };

		if (VectorNormalize (axis_x) < 1e-6f)
			VectorSet (axis_x, 1.f, 0.f, 0.f);
		if (VectorNormalize (axis_y) < 1e-6f)
			VectorSet (axis_y, 0.f, 1.f, 0.f);
		if (VectorNormalize (axis_z) < 1e-6f)
			VectorSet (axis_z, 0.f, 0.f, 1.f);

		out_normalmatrix[0] = axis_x[0];
		out_normalmatrix[1] = axis_x[1];
		out_normalmatrix[2] = axis_x[2];
		out_normalmatrix[3] = 0.f;
		out_normalmatrix[4] = axis_y[0];
		out_normalmatrix[5] = axis_y[1];
		out_normalmatrix[6] = axis_y[2];
		out_normalmatrix[7] = 0.f;
		out_normalmatrix[8] = axis_z[0];
		out_normalmatrix[9] = axis_z[1];
		out_normalmatrix[10] = axis_z[2];
		out_normalmatrix[11] = 0.f;
	}
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame (entity_t *e, aliashdr_t *paliashdr, lerpdata_t *lerpdata)
{
	static int debug_frame = -1;
	int posenum, numposes;
	int frame = e->frame;

	if ((frame >= paliashdr->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model->name);
		frame = 0;
	}

	posenum = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		e->lerptime = paliashdr->frames[frame].interval;
		posenum += (int)(cl.time / e->lerptime) % numposes;
	}
	else
		e->lerptime = 0.1;

	if (e->lerpflags & LERP_RESETANIM) //kill any lerp in progress
	{
		e->lerpstart = 0;
		e->previouspose = posenum;
		e->currentpose = posenum;
		e->lerpflags -= LERP_RESETANIM;
	}
	else if (e->currentpose != posenum) // pose changed, start new lerp
	{
		if (e->lerpflags & LERP_RESETANIM2) //defer lerping one more time
		{
			e->lerpstart = 0;
			e->previouspose = posenum;
			e->currentpose = posenum;
			e->lerpflags -= LERP_RESETANIM2;
		}
		else
		{
			e->lerpstart = cl.time;
			e->previouspose = e->currentpose;
			e->currentpose = posenum;
		}
	}

	//set up values
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		float s = (cls.demoplayback && cls.demospeed < 0.f) ? -1.f : 1.f;
		if (e->lerpflags & LERP_FINISH && numposes == 1)
			lerpdata->blend = CLAMP (0.0f, (float)(cl.time - e->lerpstart) / (e->lerpfinish - e->lerpstart), 1.0f);
		else
			lerpdata->blend = CLAMP (0.0f, (float)(cl.time - e->lerpstart) / e->lerptime * s, 1.0f);
		if (lerpdata->blend == 1.0f)
			e->previouspose = e->currentpose;
		lerpdata->pose1 = e->previouspose;
		lerpdata->pose2 = e->currentpose;
	}
	else //don't lerp
	{
		lerpdata->blend = 1;
		lerpdata->pose1 = posenum;
		lerpdata->pose2 = posenum;
	}

	if (r_debug_itemlight.value > 1.f && r_framecount != debug_frame)
	{
		debug_frame = r_framecount;
		Con_Printf ("alias_anim: host_frametime=%.6f cl.time=%.6f cl.oldtime=%.6f realtime=%.6f model=%s frame=%d lerpstart=%.6f lerptime=%.6f lerpfinish=%.6f lerpfrac=%.3f origin=(%.1f %.1f %.1f) pose=(%d->%d)\n",
			host_frametime,
			cl.time,
			cl.oldtime,
			realtime,
			e->model ? e->model->name : "<null>",
			e->frame,
			e->lerpstart,
			e->lerptime,
			e->lerpfinish,
			lerpdata->blend,
			e->origin[0],
			e->origin[1],
			e->origin[2],
			lerpdata->pose1,
			lerpdata->pose2);
	}
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
=================
*/
void R_SetupEntityTransform (entity_t *e, lerpdata_t *lerpdata)
{
	float           blend;
	vec3_t          d;
	unsigned int    i;

	// if LERP_RESETMOVE, kill any lerps in progress
	if (e->lerpflags & LERP_RESETMOVE)
	{
		e->lightcache.static_color_smooth_reset = true;
		e->movelerpstart = 0;
		VectorCopy (e->origin, e->previousorigin);
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->angles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
		e->lerpflags -= LERP_RESETMOVE;
	}
	else if (!VectorCompare (e->origin, e->currentorigin) || !VectorCompare (e->angles, e->currentangles)) // origin/angles changed, start new lerp
	{
		e->movelerpstart = cl.time;
		VectorCopy (e->currentorigin, e->previousorigin);
		VectorCopy (e->origin,  e->currentorigin);
		VectorCopy (e->currentangles, e->previousangles);
		VectorCopy (e->angles,  e->currentangles);
	}

	//set up values
	if (r_lerpmove.value && e != &cl.viewent && e->lerpflags & LERP_MOVESTEP)
	{
		float s = (cls.demoplayback && cls.demospeed < 0.f) ? -1.f : 1.f;
		if (e->lerpflags & LERP_FINISH)
			blend = CLAMP (0.0f, (float)(cl.time - e->movelerpstart) / (e->lerpfinish - e->movelerpstart), 1.0f);
		else
			blend = CLAMP (0.0f, (float)(cl.time - e->movelerpstart) / 0.1f * s, 1.0f);

		//translation
		VectorSubtract (e->currentorigin, e->previousorigin, d);
		lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
		lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
		lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

		//rotation
		VectorSubtract (e->currentangles, e->previousangles, d);
		for (i = 0; i < 3; i++)
		{
			if (d[i] > 180)  d[i] -= 360;
			if (d[i] < -180) d[i] += 360;
		}
		lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
		lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
		lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
	}
	else //don't lerp
	{
		VectorCopy (e->origin, lerpdata->origin);
		VectorCopy (e->angles, lerpdata->angles);
	}

	// chasecam
	if (chase_active.value && e == &cl_entities[cl.viewentity])
		lerpdata->angles[PITCH] *= 0.3f;
}

/*
=================
R_SetupAliasLighting -- johnfitz -- broken out from R_DrawAliasModel and rewritten
=================
*/
void R_SetupAliasLighting (entity_t     *e)
{
        vec3_t          dlightcolor = {0.f, 0.f, 0.f};
        vec3_t          dlightdir = {0.f, 0.f, 0.f};
        vec3_t          staticlightdir;
        vec3_t          ambientcolor;
        vec3_t          static_color;
        entity_lightinfo_t lightinfo;
        entity_lightinfo_t *lightinfo_ptr = r_debug_itemlight.value > 0.f ? &lightinfo : NULL;

        R_DefaultStaticLightDir (staticlightdir);
        R_EntityStaticLight (e, static_color, lightinfo_ptr);
        VectorCopy (static_color, lightcolor);
        VectorCopy (static_color, ambientcolor);

        if (e != &cl.viewent && cl.worldmodel && cl.worldmodel->lightdirdata)
        {
                vec3_t sample_rgb;
                vec3_t sample_dir;
                float dir_blend = CLAMP (0.f, r_alias_q3lighting.value, 1.f);

                if ((e->model->flags & EF_ROTATE) != 0)
                        dir_blend *= 0.25f;

                if (dir_blend > 0.f &&
                        R_SampleLightmapAndDeluxemapAtPoint (e->origin, sample_rgb, sample_dir) &&
                        VectorLength (sample_dir) > 1e-6f)
                {
                        // Blend towards deluxemap direction only when opted in.
                        // Rotating bonus items keep a much softer influence so the
                        // player-facing side does not get pushed into shadow.
                        vec3_t blended_dir;
                        VectorLerp (staticlightdir, sample_dir, dir_blend, blended_dir);
                        if (VectorNormalize (blended_dir) > 1e-6f)
                                VectorCopy (blended_dir, staticlightdir);
                }
        }

        R_AccumulateEntityModelDLights (e->origin, dlightcolor, dlightdir);
        VectorAdd (lightcolor, dlightcolor, lightcolor);
        R_FinalizeAliasLighting (e, lightcolor, ambientcolor, dlightcolor, dlightdir, staticlightdir, lightinfo_ptr);
}

/*
=================
R_FlushAliasInstances
=================
*/
void R_FlushAliasInstances (qboolean showtris)
{
	extern cvar_t r_softemu_mdl_warp;
	qmodel_t	*model;
	aliashdr_t	*mainhdr, *hdr;
	qboolean	alphatest, translucent, oit, md5;
	qboolean	viewmodel;
	int			skinnum, anim, mode;
	unsigned	state;
	GLuint		buf;
	GLbyte		*ofs;
	size_t		ibuf_size;
	qboolean	use_pp_models;
	GLuint		buffers[2];
	GLintptr	offsets[2];
	GLsizeiptr	sizes[2];
	gltexture_t	*textures[3];

	if (!ibuf.count)
		return;

	R_GLStateDump (ibuf.ent == &cl.viewent ? "before-viewmodel" : "before-alias");

	model = ibuf.ent->model;
	mainhdr = (aliashdr_t *)Mod_Extradata (model);
	anim = (int)(cl.time*10) & 3;
	viewmodel = (ibuf.ent == &cl.viewent);

	GL_BeginGroup (model->name);

	md5 = mainhdr->poseverttype == PV_IQM;

	alphatest = model->flags & MF_HOLEY ? 1 : 0;
	translucent = !ENTALPHA_OPAQUE (ibuf.ent->alpha);
	/* Late viewmodel pass has no stable translucent ordering guarantees. */
	if (viewmodel)
		translucent = false;

	/* Viewmodel and standard alpha entities should never rely on OIT blend targets.
	 * OIT is only valid during the dedicated scene translucency pass. */
	oit = translucent && !viewmodel && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT;
	switch (softemu)
	{
	case SOFTEMU_BANDED:
		mode = r_softemu_mdl_warp.value != 0.f ? ALIASSHADER_NOPERSP : ALIASSHADER_STANDARD;
		break;
	case SOFTEMU_COARSE:
		mode = r_softemu_mdl_warp.value > 0.f ? ALIASSHADER_NOPERSP : ALIASSHADER_DITHER;
		break;
	default:
		mode = r_softemu_mdl_warp.value > 0.f ? ALIASSHADER_NOPERSP : ALIASSHADER_STANDARD;
		break;
	}
	if (mode == ALIASSHADER_NOPERSP && gl_vendor && !strcmp (gl_vendor, "Intel"))
		mode = ALIASSHADER_STANDARD;
	GL_UseProgram (glprogs.alias[oit][mode][alphatest][md5]);
	R_Shadow_ApplyAliasReceiverUniforms (glprogs.alias[oit][mode][alphatest][md5]);

	if (md5)
		state = GLS_CULL_BACK | GLS_ATTRIBS(6);
	else
		state = GLS_CULL_BACK | GLS_ATTRIBS(2);

	if (!translucent)
	{
		state |= GLS_BLEND_OPAQUE;
	}
	else
	{
		state |= (oit ? GLS_BLEND_ALPHA_OIT : GLS_BLEND_ALPHA) | GLS_NO_ZWRITE;
	}
	{
		RenderBackendPipelineDesc pipeline_desc;
		RenderBackendDynamicState dynamic_state;
		memset (&pipeline_desc, 0, sizeof (pipeline_desc));
		memset (&dynamic_state, 0, sizeof (dynamic_state));
		pipeline_desc.state_bits = state;
		dynamic_state.blend_state = state;
		dynamic_state.depth_state = state;
		dynamic_state.raster_state = state;
		R_Backend_BindPipeline (&pipeline_desc);
		R_Backend_SetDynamicState (&dynamic_state);
	}

	memcpy (ibuf.global.matviewproj, r_matviewproj, sizeof (r_matviewproj));
memcpy (ibuf.global.prev_matviewproj, r_framedata.prev_viewproj, sizeof (r_framedata.prev_viewproj));
	memcpy (ibuf.global.eyepos, r_refdef.vieworg, sizeof (r_refdef.vieworg));
	memcpy (ibuf.global.fog, r_framedata.fogdata, 3 * sizeof (float));
	use_pp_models = (r_dynamic.value > 0.f);
	{
// use fog density sign bit as overbright flag
ibuf.global.fog[3] =
gl_overbright_models.value ?
-fabs (r_framedata.fogdata[3]) :
 fabs (r_framedata.fogdata[3])
;
ibuf.global.overbright = gl_overbright_models.value > 0.f ? r_framedata.dither[2] : 1.f;
ibuf.global.dither = r_framedata.dither[0];
ibuf.global.half_lambert = CLAMP (0.f, r_model_halflambert.value, 1.f);
ibuf.global.dlight_debug_models = 0.f;
ibuf.global.dlight_directional_mix = 1.f;
ibuf.global.pp_dlight_model_enable = use_pp_models ? 1.f : 0.f;
ibuf.global.pp_dlight_model_debug = 0.f;
	}
ibuf.global.ambient_sky_params[0] = (r_skyvis.value > 0.f && R_SkyVis_Active ()) ? 1.f : 0.f;
ibuf.global.ambient_sky_params[1] = R_SkyVis_GetResolvedScale ();
ibuf.global.ambient_sky_params[2] = (r_skyvis_debug.value >= 2.f) ? 1.f : 0.f;
ibuf.global.ambient_sky_params[3] = 0.f;
ibuf.global.ambient_sky_tint[0] = r_framedata.skyvis_tint[0];
ibuf.global.ambient_sky_tint[1] = r_framedata.skyvis_tint[1];
ibuf.global.ambient_sky_tint[2] = r_framedata.skyvis_tint[2];
ibuf.global.ambient_sky_tint[3] = r_framedata.skyvis_tint[3];
ibuf.global.post_exposure = r_autoexposure_debug_exposure;
ibuf.global.tonemap_mode = r_tonemap.value;
ibuf.global._pad1[0] = CLAMP (r_alias_q3lighting.value, 0.f, 1.f);
ibuf.global._pad_tail[0] = 0.f;
ibuf.global._pad_tail[1] = 0.f;

	ibuf_size = sizeof(ibuf.global) + sizeof(ibuf.inst[0]) * ibuf.count;
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &ibuf.global, ibuf_size, &buf, &ofs);

	buffers[0] = buf;
	offsets[0] = (GLintptr) ofs;
	sizes[0] = ibuf_size;

	GL_BindBuffer (GL_ARRAY_BUFFER, model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);
	{
		/*
		 * Keep TEXTURE6 unbound for alias draws to avoid
		 * when both units point at the same texture object.
		 *
		 * Important: do not switch/bind TEXTURE6 until after
		 * FRAME_BEGIN logging block and framebuffer attachment queries.
		 */
		GL_BindNative (GL_TEXTURE6, GL_TEXTURE_2D, 0);
	}

	for (hdr = mainhdr; hdr; hdr = hdr->nextsurface ? (aliashdr_t *) ((byte *)hdr + hdr->nextsurface) : NULL)
	{
		if (md5)
		{
			GL_VertexAttribPointerFunc  (0, 3, GL_FLOAT,			GL_FALSE, sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, xyz)));
			GL_VertexAttribPointerFunc  (1, 4, GL_BYTE,				GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, norm)));
			GL_VertexAttribPointerFunc  (2, 2, GL_FLOAT,			GL_FALSE, sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, st)));
			GL_VertexAttribPointerFunc  (3, 4, GL_UNSIGNED_BYTE,	GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, weight)));
			GL_VertexAttribIPointerFunc (4, 4, GL_UNSIGNED_BYTE,	          sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, idx)));
			GL_VertexAttribPointerFunc  (5, 4, GL_BYTE,             GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, tangent)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vboposeofs;
			sizes[1] = sizeof (bonepose_t) * hdr->numbones * hdr->numboneposes;
		}
		else
		{
			GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof (meshst_t), (void *) hdr->vbostofs);
			GL_VertexAttribPointerFunc (1, 4, GL_BYTE, GL_TRUE, sizeof (meshst_t), (void *) (hdr->vbostofs + offsetof (meshst_t, tangent)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vbovertofs;
			sizes[1] = sizeof (meshxyz_t) * hdr->numverts_vbo * hdr->numposes;
		}

		GL_BindBuffersRange (GL_SHADER_STORAGE_BUFFER, 1, 2, buffers, offsets, sizes);

		//
		// set up textures
		//
		skinnum = ibuf.ent->skinnum;
		if ((skinnum >= hdr->numskins) || (skinnum < 0))
		{
			Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, model->name);
			// ericw -- display skin 0 for winquake compatibility
			skinnum = 0;
		}

		textures[0] = hdr->gltextures[skinnum][anim];
		textures[1] = hdr->fbtextures[skinnum][anim];
		textures[2] = hdr->emissivetextures[skinnum][anim];
		if (hdr == mainhdr && ibuf.ent->colormap != vid.colormap && !gl_nocolors.value)
			if (CL_IsPlayerEnt (ibuf.ent)) /* && !strcmp (ibuf.ent->model->name, "progs/player.mdl") */
				textures[0] = playertextures[ibuf.ent - cl_entities - 1];

		if (!gl_fullbrights.value)
		{
			textures[1] = blacktexture;
			textures[2] = blacktexture;
		}

		if (r_lightmap_cheatsafe)
		{
			textures[0] = greytexture;
			textures[1] = blacktexture;
			textures[2] = blacktexture;
		}

		if (!textures[1])
			textures[1] = blacktexture;

		if (!textures[2])
			textures[2] = blacktexture;

		if (showtris)
		{
			textures[0] = blacktexture;
			textures[1] = whitetexture;
			textures[2] = blacktexture;
		}

		GL_BindTextures (0, 3, textures);
		GL_Bind (GL_TEXTURE3, blacktexture);

		{
			RenderBackendDrawPacket draw_packet;
			memset (&draw_packet, 0, sizeof (draw_packet));
			draw_packet.primitive = R_BACKEND_PRIMITIVE_TRIANGLES;
			draw_packet.index_type = R_BACKEND_INDEX_TYPE_UINT16;
			draw_packet.count = hdr->numindexes;
			draw_packet.instance_count = ibuf.count;
			draw_packet.index_offset_bytes = (intptr_t)hdr->eboofs;
			draw_packet.flags = R_BACKEND_DRAW_PACKET_INDEXED | R_BACKEND_DRAW_PACKET_INSTANCED;
			R_Backend_DrawPacket (&draw_packet);
		}

		rs_aliaspasses += hdr->numtris * ibuf.count;
	}

	ibuf.count = 0;

	GL_EndGroup();
}

static void R_FlushAliasInstances_Shadow (qboolean dlight)
{
	qmodel_t *model;
	aliashdr_t *mainhdr, *hdr;
	qboolean md5;
	unsigned state;
	GLuint program;
	GLuint instance_buf;
	GLbyte *instance_ofs;
	GLuint buffers[2];
	GLintptr offsets[2];
	GLsizeiptr sizes[2];

	if (!ibuf.count)
		return;

	model = ibuf.ent->model;
	mainhdr = (aliashdr_t *)Mod_Extradata (model);
	md5 = mainhdr->poseverttype == PV_IQM;

	program = glprogs.alias_shadow[md5 ? 1 : 0];
	GL_UseProgram (program);
	R_Shadow_ApplyAliasCasterUniforms (program);

	if (md5)
		state = GLS_CULL_BACK | GLS_BLEND_OPAQUE | GLS_ATTRIBS (6);
	else
		state = GLS_CULL_BACK | GLS_BLEND_OPAQUE | GLS_ATTRIBS (2);
	{
		RenderBackendPipelineDesc pipeline_desc;
		RenderBackendDynamicState dynamic_state;
		memset (&pipeline_desc, 0, sizeof (pipeline_desc));
		memset (&dynamic_state, 0, sizeof (dynamic_state));
		pipeline_desc.state_bits = state;
		dynamic_state.blend_state = state;
		dynamic_state.depth_state = state;
		dynamic_state.raster_state = state;
		R_Backend_BindPipeline (&pipeline_desc);
		R_Backend_SetDynamicState (&dynamic_state);
	}

	GL_Upload (GL_SHADER_STORAGE_BUFFER, ibuf.inst, sizeof (ibuf.inst[0]) * ibuf.count, &instance_buf, &instance_ofs);

	GL_BindBuffer (GL_ARRAY_BUFFER, model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);

	buffers[0] = instance_buf;
	offsets[0] = (GLintptr)instance_ofs;
	sizes[0] = sizeof (ibuf.inst[0]) * ibuf.count;

	for (hdr = mainhdr; hdr; hdr = hdr->nextsurface ? (aliashdr_t *)((byte *)hdr + hdr->nextsurface) : NULL)
	{
		if (md5)
		{
			GL_VertexAttribPointerFunc  (0, 3, GL_FLOAT,         GL_FALSE, sizeof (iqmvert_t), (void *)(hdr->vbovertofs + offsetof (iqmvert_t, xyz)));
			GL_VertexAttribPointerFunc  (1, 4, GL_BYTE,          GL_TRUE,  sizeof (iqmvert_t), (void *)(hdr->vbovertofs + offsetof (iqmvert_t, norm)));
			GL_VertexAttribPointerFunc  (2, 2, GL_FLOAT,         GL_FALSE, sizeof (iqmvert_t), (void *)(hdr->vbovertofs + offsetof (iqmvert_t, st)));
			GL_VertexAttribPointerFunc  (3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  sizeof (iqmvert_t), (void *)(hdr->vbovertofs + offsetof (iqmvert_t, weight)));
			GL_VertexAttribIPointerFunc (4, 4, GL_UNSIGNED_BYTE,          sizeof (iqmvert_t), (void *)(hdr->vbovertofs + offsetof (iqmvert_t, idx)));
			GL_VertexAttribPointerFunc  (5, 4, GL_BYTE,          GL_TRUE,  sizeof (iqmvert_t), (void *)(hdr->vbovertofs + offsetof (iqmvert_t, tangent)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vboposeofs;
			sizes[1] = sizeof (bonepose_t) * hdr->numbones * hdr->numboneposes;
		}
		else
		{
			GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof (meshst_t), (void *)hdr->vbostofs);
			GL_VertexAttribPointerFunc (1, 4, GL_BYTE, GL_TRUE, sizeof (meshst_t), (void *)(hdr->vbostofs + offsetof (meshst_t, tangent)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vbovertofs;
			sizes[1] = sizeof (meshxyz_t) * hdr->numverts_vbo * hdr->numposes;
		}

		GL_BindBuffersRange (GL_SHADER_STORAGE_BUFFER, 1, 2, buffers, offsets, sizes);
		R_Backend_DrawIndexedInstanced (R_BACKEND_PRIMITIVE_TRIANGLES, R_BACKEND_INDEX_TYPE_UINT16, hdr->numindexes, (intptr_t)hdr->eboofs, ibuf.count);
	}

	ibuf.count = 0;
	(void)dlight;
}

/*
=================
R_Alias_CanAddToBatch
=================
*/
static qboolean R_Alias_CanAddToBatch (const entity_t *e)
{
	// empty batch
	if (!ibuf.count)
		return true;

	// full batch
	if (ibuf.count == countof (ibuf.inst))
		return false;

	// different models/skins
	if (ibuf.ent->model != e->model || ibuf.ent->skinnum != e->skinnum)
		return false;

	// players have custom colors
	if (!gl_nocolors.value && CL_IsPlayerEnt (ibuf.ent))
		return false;

	return true;
}

static void R_DrawAliasModel_Shadow_Real (entity_t *e)
{
	aliashdr_t *paliashdr;
	lerpdata_t lerpdata;
	float model_matrix[16];
	aliasinstance_t *instance;

	if (!e || !e->model || e->model->type != mod_alias)
		return;
	if (e == &cl.viewent)
		return;
	if (e->model->flags & MF_HOLEY) /* alpha-test casters are intentionally excluded in v1 */
		return;
	if (e->model->flags & MOD_NOSHADOW)
		return;
	if (!ENTALPHA_OPAQUE (e->alpha))
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (lerpdata.pose1 == lerpdata.pose2)
		lerpdata.blend = 0.f;
	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles, e->scale);
	ApplyTranslation (model_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
	ApplyScale (model_matrix, paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);

	if (!R_Alias_CanAddToBatch (e))
		R_FlushAliasInstances_Shadow (r_alias_shadow_batch_dlight);

	if (!ibuf.count)
		ibuf.ent = e;

	instance = &ibuf.inst[ibuf.count++];
	memset (instance, 0, sizeof (*instance));
	MatrixTranspose4x3 (model_matrix, instance->worldmatrix);
	MatrixTranspose4x3 (model_matrix, instance->prev_worldmatrix);
	R_BuildAliasNormalMatrix (model_matrix, instance->normalmatrix);
	instance->alpha = 1.f;
	instance->pose1 = lerpdata.pose1;
	instance->pose2 = lerpdata.pose2;
	instance->blend = lerpdata.blend;
	if (paliashdr->poseverttype == PV_QUAKE1)
	{
		instance->pose1 *= paliashdr->numverts_vbo;
		instance->pose2 *= paliashdr->numverts_vbo;
	}
	else
	{
		instance->pose1 *= paliashdr->numbones;
		instance->pose2 *= paliashdr->numbones;
	}
}

/*
=================
R_DrawAliasModel_Real
=================
*/
static void R_DrawAliasModel_Real (entity_t *e, qboolean showtris)
{
	aliashdr_t	*paliashdr;
	lerpdata_t	lerpdata;
	float		fovscale = 1.0f;
	float		model_matrix[16];
	aliasinstance_t	*instance;
	float		skyvisibility = 0.f;

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);

	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (lerpdata.pose1 == lerpdata.pose2)
		lerpdata.blend = 0.f;

	//
	// viewmodel adjustments (position, fov distortion correction)
	//
	if (e == &cl.viewent)
	{
		if (r_refdef.basefov > 90.f && cl_gun_fovscale.value)
		{
			fovscale = tan (r_refdef.basefov * (0.5f * M_PI / 180.f));
			fovscale = 1.f + (fovscale - 1.f) * cl_gun_fovscale.value;
		}

		VectorMA (lerpdata.origin, cl_gun_x.value * paliashdr->scale[0] * fovscale,	vright,	lerpdata.origin);
		VectorMA (lerpdata.origin, cl_gun_y.value * paliashdr->scale[1] * fovscale,	vup,	lerpdata.origin);
		VectorMA (lerpdata.origin, cl_gun_z.value * paliashdr->scale[2],			vpn,	lerpdata.origin);
	}

	//
	// cull it
	//
	if (R_CullModelForEntity(e))
		return;

	//
	// transform it
	//
	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles, e->scale);
	ApplyTranslation (model_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
	ApplyScale (model_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);

	//
	// set up for alpha blending
	//
        if (r_lightmap_cheatsafe) //no alpha in drawflat or lightmap mode
                entalpha = 1;
        else
                entalpha = ENTALPHA_DECODE(e->alpha);

	/* Late viewmodel pass has no stable translucent ordering guarantees.
	 * Force full alpha to prevent intermittent transparent weapon polygons. */
	if (e == &cl.viewent)
		entalpha = 1.f;

        if (entalpha == 0)
                return;

        //
        // set up lighting
        //
	rs_aliaspolys += paliashdr->numtris;

	if (r_fullbright_cheatsafe || showtris)
	{
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;
		VectorCopy (lightcolor, e->lightcache.ambientcolor);
		VectorClear (e->lightcache.dlightcolor);
		VectorClear (e->lightcache.dlightdir);
		R_DefaultStaticLightDir (e->lightcache.staticlightdir);
		e->lightcache.lightgrid_has_sample = false;
		e->lightcache.lightgrid_ao = 0.f;
		VectorClear (e->lightcache.lightgrid_color);
	}
	else
	{
		R_SetupAliasLighting (e);
		if (r_skyvis.value > 0.f && R_SkyVis_Active ())
			skyvisibility = CLAMP (0.f, R_SkyVis_Sample (lerpdata.origin), 1.f);
	}

	if (showtris)
		entalpha = 1.f;

	if (!R_Alias_CanAddToBatch (e))
		R_FlushAliasInstances (showtris);

	if (!ibuf.count)
		ibuf.ent = e;

	instance = &ibuf.inst[ibuf.count++];
	instance->flags = ALIAS_INSTANCE_FLAG_NONE;

	{
		float prev_model_matrix[16];
		vec3_t prev_origin;
		vec3_t prev_angles;
		qboolean have_prev = false;

		if (e != &cl.viewent && e->motion_blur_prev_valid && e->motion_blur_prev_frame == r_framecount - 1)
		{
			VectorCopy (e->motion_blur_prev_origin, prev_origin);
			VectorCopy (e->motion_blur_prev_angles, prev_angles);
			have_prev = true;
		}

		if (!have_prev)
		{
			VectorCopy (lerpdata.origin, prev_origin);
			VectorCopy (lerpdata.angles, prev_angles);
		}

		if (e == &cl.viewent)
		{
			memcpy (prev_model_matrix, model_matrix, sizeof (model_matrix));
		}
		else
		{
			R_EntityMatrix (prev_model_matrix, prev_origin, prev_angles, e->scale);
			ApplyTranslation (prev_model_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
			ApplyScale (prev_model_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);
		}

		MatrixTranspose4x3 (model_matrix, instance->worldmatrix);
		MatrixTranspose4x3 (prev_model_matrix, instance->prev_worldmatrix);
		R_BuildAliasNormalMatrix (model_matrix, instance->normalmatrix);
	}

	VectorCopy (lerpdata.origin, e->motion_blur_prev_origin);
	VectorCopy (lerpdata.angles, e->motion_blur_prev_angles);
	e->motion_blur_prev_frame = r_framecount;
	e->motion_blur_prev_valid = true;

        VectorCopy (lightcolor, instance->lightcolor);
        VectorCopy (e->lightcache.dlightcolor, instance->dlightcolor);
        VectorCopy (e->lightcache.dlightdir, instance->dlightdir);
        VectorCopy (e->lightcache.staticlightdir, instance->staticlightdir);
	instance->skyvisibility = skyvisibility;
	instance->_pad0 = 0.f;
	instance->_pad1 = 0.f;
	instance->_pad2 = 0.f;
	instance->_pad3[0] = 0.f;
	instance->_pad3[1] = 0.f;
	instance->_pad3[2] = 0.f;
	instance->_pad_pose[0] = 0.f;
	instance->_pad_pose[1] = 0.f;
	instance->_pad_pose[2] = 0.f;
	instance->alpha = entalpha;
	if (e == &cl.viewent)
		instance->flags |= ALIAS_INSTANCE_FLAG_NO_MOTION_BLUR | ALIAS_INSTANCE_FLAG_VIEWMODEL;
if (e->model->flags & EF_ROTATE)
		instance->flags |= ALIAS_INSTANCE_FLAG_ROTATE;
if (!Q_strncmp (e->model->name, "progs/bolt", 10))
		instance->flags |= ALIAS_INSTANCE_FLAG_LIGHTNING;
        instance->pose1 = lerpdata.pose1;
        instance->pose2 = lerpdata.pose2;
        instance->blend = lerpdata.blend;

	if (paliashdr->poseverttype == PV_QUAKE1)
	{
		instance->pose1 *= paliashdr->numverts_vbo;
		instance->pose2 *= paliashdr->numverts_vbo;
	}
	else
	{
		instance->pose1 *= paliashdr->numbones;
		instance->pose2 *= paliashdr->numbones;
	}
	instance->_pad_tail = 0.f;
}

/*
=================
R_DrawAliasModels
=================
*/
void R_DrawAliasModels (entity_t **ents, int count)
{
	int i;
	qboolean use_shared_model_lights = (r_dynamic.value > 0.f);
	unsigned int saved_numlights = r_framedata.numlights;
	gpulightbuffer_t saved_lightbuffer = {0};
	dlight_t *saved_sources[DLIGHT_GPU_MAX] = {0};

	/* Shared-light architecture: consumes the same frame-collected list as
	 * world dlights, but filtered for forward alias/model shading. */

	if (use_shared_model_lights)
	{
		int pp_count;
		memcpy (&saved_lightbuffer, &r_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (saved_sources, r_dlight_sources, sizeof (saved_sources));

		pp_count = R_PPdlights_BuildModelGpuLights (&r_lightbuffer, r_dlight_sources, DLIGHT_GPU_MAX);
		r_framedata.numlights = (unsigned int)pp_count;
		R_UploadFrameData ();
	}

	for (i = 0; i < count; i++)
		R_DrawAliasModel_Real (ents[i], false);
	R_FlushAliasInstances (false);

	if (use_shared_model_lights)
	{
		r_framedata.numlights = saved_numlights;
		memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
		R_UploadFrameData ();
	}
}

void R_DrawAliasModels_Shadow (entity_t **ents, int count, qboolean dlight)
{
	int i;
	r_alias_shadow_batch_dlight = dlight;
	for (i = 0; i < count; i++)
		R_DrawAliasModel_Shadow_Real (ents[i]);
	R_FlushAliasInstances_Shadow (dlight);
}

/*
=================
R_DrawAliasModels_ShowTris
=================
*/
void R_DrawAliasModels_ShowTris (entity_t **ents, int count)
{
	int i;
	qboolean use_shared_model_lights = (r_dynamic.value > 0.f);
	unsigned int saved_numlights = r_framedata.numlights;
	gpulightbuffer_t saved_lightbuffer = {0};
	dlight_t *saved_sources[DLIGHT_GPU_MAX] = {0};

	if (use_shared_model_lights)
	{
		int pp_count;
		memcpy (&saved_lightbuffer, &r_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (saved_sources, r_dlight_sources, sizeof (saved_sources));

		pp_count = R_PPdlights_BuildModelGpuLights (&r_lightbuffer, r_dlight_sources, DLIGHT_GPU_MAX);
		r_framedata.numlights = (unsigned int)pp_count;
		R_UploadFrameData ();
	}

	for (i = 0; i < count; i++)
		R_DrawAliasModel_Real (ents[i], true);
	R_FlushAliasInstances (true);

	if (use_shared_model_lights)
	{
		r_framedata.numlights = saved_numlights;
		memcpy (&r_lightbuffer, &saved_lightbuffer, sizeof (saved_lightbuffer));
		memcpy (r_dlight_sources, saved_sources, sizeof (saved_sources));
		R_UploadFrameData ();
	}
}

