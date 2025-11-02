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

// r_alias.c -- alias model rendering (gehärtet & leicht optimiert)

#include "quakedef.h"

extern cvar_t gl_overbright_models, gl_fullbrights, r_lerpmodels, r_lerpmove; //johnfitz
extern cvar_t scr_fov, cl_gun_fovscale, cl_gun_x, cl_gun_y, cl_gun_z;
extern cvar_t r_oit;

// bis zu 16 farb-translatete Skins
gltexture_t* playertextures[MAX_SCOREBOARD]; //johnfitz -- array of pointers

const float r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

extern vec3_t lightcolor; //johnfitz -- replaces "float shadelight" for lit support

static float entalpha; //johnfitz

// johnfitz -- struct für Lerp-Infos an die Draw-Funktionen
typedef struct {
	short pose1;
	short pose2;
	float blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;

#define MAX_ALIAS_INSTANCES 256

typedef struct aliasinstance_s {
	float    worldmatrix[12];
	float    prev_worldmatrix[12];
	vec3_t   lightcolor;
	float    alpha;
	int32_t  pose1;
	int32_t  pose2;
	float    blend;
	int32_t  flags;
} aliasinstance_t;

#define ALIAS_INSTANCE_FLAG_NONE                  0
#define ALIAS_INSTANCE_FLAG_NO_MOTION_BLUR        (1 << 0)
#define ALIAS_INSTANCE_FLAG_VIEWMODEL             (1 << 1)
#define ALIAS_INSTANCE_FLAG_PLAYER                (1 << 2)
#define ALIAS_INSTANCE_FLAG_FULLBRIGHT_HACK       (1 << 3)
#define ALIAS_INSTANCE_FLAG_ITEM                  (1 << 4)
#define ALIAS_INSTANCE_FLAG_FORCE_FULLBRIGHT      (1 << 5)

struct ibuf_s {
	int         count;
	entity_t* ent;

	struct {
		float matviewproj[16];
		float prev_matviewproj[16];
		vec3_t eyepos;
		float  _pad;
		vec4_t fog;
		float  dither;
		float  _padding[3];
	} global;

	aliasinstance_t inst[MAX_ALIAS_INSTANCES];
} ibuf;

/* === Hilfsfunktionen ==================================================== */

static inline float qsafe_div (float num, float den, float fallback)
{
	// schützt vor Division durch 0 / NaN
	if (den == 0.0f || !isfinite (den))
		return fallback;
	return num / den;
}

static inline qboolean qsafe_ent_valid (const entity_t* e)
{
	return e && e->model;
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
Härtung:
- Null-/Bounds-Checks
- Schutz vor numposes==0
- Schutz vor Division durch 0 in Lerp
=================
*/
void R_SetupAliasFrame (entity_t* e, aliashdr_t* paliashdr, lerpdata_t* lerpdata)
{
	if (!qsafe_ent_valid (e) || !paliashdr || !lerpdata)
		return;

	int frame = e->frame;
	if (frame < 0 || frame >= paliashdr->numframes) {
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model ? e->model->name : "(null)");
		frame = 0;
	}

	int posenum = paliashdr->frames[frame].firstpose;
	int numposes = paliashdr->frames[frame].numposes;

	// Schutz: numposes muss >=1 sein
	if (numposes <= 0) {
		Con_DPrintf ("R_AliasSetupFrame: frame %d has invalid numposes=%d for '%s'\n",
			frame, numposes, e->model ? e->model->name : "(null)");
		numposes = 1;
	}

	if (numposes > 1)
	{
		e->lerptime = paliashdr->frames[frame].interval;
		if (e->lerptime <= 0.0f || !isfinite (e->lerptime))
			e->lerptime = 0.1f; // Fallback

		const double t = cl.time;
		const int    pi = (int)(t / e->lerptime);
		int offs = pi % numposes;
		if (offs < 0) offs += numposes; // robust bei negativen Zeiten
		posenum += offs;
	}
	else
	{
		// Konstante Pose, aber nicht 0, um Division durch 0 zu vermeiden
		e->lerptime = 0.1f;
	}

	// Lerp-State
	if (e->lerpflags & LERP_RESETANIM) // kill any lerp in progress
	{
		e->lerpstart = 0;
		e->previouspose = posenum;
		e->currentpose = posenum;
		e->lerpflags -= LERP_RESETANIM;
	}
	else if (e->currentpose != posenum) // pose changed, start new lerp
	{
		if (e->lerpflags & LERP_RESETANIM2) // defer lerping one more time
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

	// Werte setzen
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		const float s = (cls.demoplayback && cls.demospeed < 0.f) ? -1.f : 1.f;
		float tnum, tden;

		if (e->lerpflags & LERP_FINISH && numposes == 1) {
			tnum = (float)(cl.time - e->lerpstart);
			tden = (float)(e->lerpfinish - e->lerpstart);
		}
		else {
			tnum = (float)(cl.time - e->lerpstart) * s;
			tden = e->lerptime;
		}

		lerpdata->blend = CLAMP (0.0f, qsafe_div (tnum, tden, 1.0f), 1.0f);
		if (lerpdata->blend == 1.0f)
			e->previouspose = e->currentpose;

		lerpdata->pose1 = e->previouspose;
		lerpdata->pose2 = e->currentpose;
	}
	else // nicht lerpen
	{
		lerpdata->blend = 1.0f;
		lerpdata->pose1 = posenum;
		lerpdata->pose2 = posenum;
	}
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
Härtung:
- robustere Zeit- und Blend-Berechnung
=================
*/
void R_SetupEntityTransform (entity_t* e, lerpdata_t* lerpdata)
{
	if (!qsafe_ent_valid (e) || !lerpdata)
		return;

	float blend;
	vec3_t d;
	int i;

	// if LERP_RESETMOVE, kill any lerps in progress
	if (e->lerpflags & LERP_RESETMOVE)
	{
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
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->currentangles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
	}

	// Werte setzen
	if (r_lerpmove.value && e != &cl.viewent && (e->lerpflags & LERP_MOVESTEP))
	{
		const float s = (cls.demoplayback && cls.demospeed < 0.f) ? -1.f : 1.f;

		float tnum, tden;
		if (e->lerpflags & LERP_FINISH) {
			tnum = (float)(cl.time - e->movelerpstart);
			tden = (float)(e->lerpfinish - e->movelerpstart);
		}
		else {
			tnum = (float)(cl.time - e->movelerpstart) * s;
			tden = 0.1f;
		}

		blend = CLAMP (0.0f, qsafe_div (tnum, tden, 1.0f), 1.0f);

		// translation
		VectorSubtract (e->currentorigin, e->previousorigin, d);
		lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
		lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
		lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

		// rotation
		VectorSubtract (e->currentangles, e->previousangles, d);
		for (i = 0; i < 3; i++)
		{
			if (d[i] > 180.0f) d[i] -= 360.0f;
			if (d[i] < -180.0f) d[i] += 360.0f;
		}
		lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
		lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
		lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
	}
	else // nicht lerpen
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
Härtung:
- Fallback wenn beide Light-Traces schwarz sind
=================
*/
void R_SetupAliasLighting (entity_t* e)
{
	if (!qsafe_ent_valid (e))
		return;

	// initialer Trace
	if (!R_LightPoint (e->origin, 0.f, &e->lightcache)) {
		// Origin leicht über Boden erneut versuchen
		if (!R_LightPoint (e->origin, e->model->maxs[2] * 0.5f, &e->lightcache)) {
			// letzter Fallback: kleines Grundlicht, damit nichts komplett „versumpft“
			lightcolor[0] = lightcolor[1] = lightcolor[2] = 64.0f / 200.0f;
		}
	}

	// Normierung: Quake-Software-Renderer-Annäherung
	VectorScale (lightcolor, 1.0f / 200.0f, lightcolor);
}


/*
=================
R_FlushAliasInstances
Härtung:
- Null-Checks, Bounds, Early-Return
- Schutz vor leeren/inkonsistenten Batches
=================
*/
void R_FlushAliasInstances (qboolean showtris)
{
	extern cvar_t r_softemu_mdl_warp;
	if (!ibuf.count)
		return;

	if (!ibuf.ent || !qsafe_ent_valid (ibuf.ent)) {
		ibuf.count = 0;
		return;
	}

	qmodel_t* model = ibuf.ent->model;
	aliashdr_t* mainhdr = (aliashdr_t*)Mod_Extradata (model);

	if (!mainhdr) {
		ibuf.count = 0;
		return;
	}

	qboolean alphatest = (model->flags & MF_HOLEY) ? 1 : 0;
	qboolean translucent = !ENTALPHA_OPAQUE (ibuf.ent->alpha);
	qboolean oit = translucent && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT;
	int      anim = (int)(cl.time * 10) & 3;
	int      mode;

	GLuint   buf;
	GLbyte* ofs = NULL;
	size_t   ibuf_size;
	GLuint   buffers[2];
	GLintptr offsets[2];
	GLsizeiptr sizes[2];
	gltexture_t* textures[3];

	GL_BeginGroup (model->name);

	const qboolean md5 = (mainhdr->poseverttype == PV_IQM);

	// Softemu-Modus
	switch (softemu)
	{
	case SOFTEMU_BANDED:
		mode = (r_softemu_mdl_warp.value != 0.f) ? ALIASSHADER_NOPERSP : ALIASSHADER_STANDARD;
		break;
	case SOFTEMU_COARSE:
		mode = (r_softemu_mdl_warp.value > 0.f) ? ALIASSHADER_NOPERSP : ALIASSHADER_DITHER;
		break;
	default:
		mode = (r_softemu_mdl_warp.value > 0.f) ? ALIASSHADER_NOPERSP : ALIASSHADER_STANDARD;
		break;
	}

	GL_UseProgram (glprogs.alias[oit][mode][alphatest][md5]);

	unsigned state = GLS_CULL_BACK | (md5 ? GLS_ATTRIBS (5) : GLS_ATTRIBS (1));
	if (!translucent)
		state |= GLS_BLEND_OPAQUE;
	else
		state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;
	GL_SetState (state);

	// globale Daten für diesen Batch
	memcpy (ibuf.global.matviewproj, r_matviewproj, sizeof (r_matviewproj));
	memcpy (ibuf.global.prev_matviewproj, r_framedata.prev_viewproj, sizeof (r_framedata.prev_viewproj));
	memcpy (ibuf.global.eyepos, r_refdef.vieworg, sizeof (r_refdef.vieworg));
	memcpy (ibuf.global.fog, r_framedata.fogdata, 3 * sizeof (float));
	// Fog-Dichte: Vorzeichenbit als Overbright-Flag missbrauchen (bestehendes Verhalten)
	ibuf.global.fog[3] = gl_overbright_models.value ? -fabs (r_framedata.fogdata[3]) : fabs (r_framedata.fogdata[3]);
	ibuf.global.dither = r_framedata.screendither;

	ibuf_size = sizeof (ibuf.global) + sizeof (ibuf.inst[0]) * (size_t)ibuf.count;
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &ibuf.global, (GLsizeiptr)ibuf_size, &buf, &ofs);

	buffers[0] = buf;
	offsets[0] = (GLintptr)ofs;
	sizes[0] = (GLsizeiptr)ibuf_size;

	GL_BindBuffer (GL_ARRAY_BUFFER, model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);

	for (aliashdr_t* hdr = mainhdr; hdr; hdr = hdr->nextsurface ? (aliashdr_t*)((byte*)hdr + hdr->nextsurface) : NULL)
	{
		if (md5)
		{
			GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), (void*)(intptr_t)(hdr->vbovertofs + offsetof (iqmvert_t, xyz)));
			GL_VertexAttribPointerFunc (1, 4, GL_BYTE, GL_TRUE, sizeof (iqmvert_t), (void*)(intptr_t)(hdr->vbovertofs + offsetof (iqmvert_t, norm)));
			GL_VertexAttribPointerFunc (2, 2, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), (void*)(intptr_t)(hdr->vbovertofs + offsetof (iqmvert_t, st)));
			GL_VertexAttribPointerFunc (3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (iqmvert_t), (void*)(intptr_t)(hdr->vbovertofs + offsetof (iqmvert_t, weight)));
			GL_VertexAttribIPointerFunc (4, 4, GL_UNSIGNED_BYTE, sizeof (iqmvert_t), (void*)(intptr_t)(hdr->vbovertofs + offsetof (iqmvert_t, idx)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vboposeofs;
			sizes[1] = (GLsizeiptr)(sizeof (bonepose_t) * (size_t)hdr->numbones * (size_t)hdr->numboneposes);
		}
		else
		{
			GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof (meshst_t), (void*)(intptr_t)hdr->vbostofs);

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vbovertofs;
			sizes[1] = (GLsizeiptr)(sizeof (meshxyz_t) * (size_t)hdr->numverts_vbo * (size_t)hdr->numposes);
		}

		GL_BindBuffersRange (GL_SHADER_STORAGE_BUFFER, 1, 2, buffers, offsets, sizes);

		// Texturen setzen
		int skinnum = ibuf.ent->skinnum;
		if (skinnum < 0 || skinnum >= hdr->numskins)
		{
			Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, model->name);
			skinnum = 0; // WinQuake-Kompatibilität
		}

		textures[0] = hdr->gltextures[skinnum][anim];
		textures[1] = hdr->fbtextures[skinnum][anim];
		textures[2] = hdr->emissivetextures[skinnum][anim];

		if (hdr == mainhdr && ibuf.ent->colormap != vid.colormap && !gl_nocolors.value) {
			if (CL_IsPlayerEnt (ibuf.ent)) /* && !strcmp (ibuf.ent->model->name, "progs/player.mdl") */
				textures[0] = playertextures[ibuf.ent - cl_entities - 1];
		}

		if (!gl_fullbrights.value) {
			textures[1] = blacktexture;
			textures[2] = blacktexture;
		}

		if (r_lightmap_cheatsafe) {
			textures[0] = greytexture;
			textures[1] = blacktexture;
			textures[2] = blacktexture;
		}

		if (!textures[1]) textures[1] = blacktexture;
		if (!textures[2]) textures[2] = blacktexture;

		if (showtris) {
			textures[0] = blacktexture;
			textures[1] = whitetexture;
			textures[2] = blacktexture;
		}

		GL_BindTextures (0, 3, textures);

		GL_DrawElementsInstancedFunc (GL_TRIANGLES, hdr->numindexes, GL_UNSIGNED_SHORT, (void*)(intptr_t)hdr->eboofs, ibuf.count);

		rs_aliaspasses += hdr->numtris * ibuf.count;
	}

	ibuf.count = 0;

	GL_EndGroup ();
}

/*
=================
R_Alias_CanAddToBatch
=================
*/
static inline qboolean R_Alias_CanAddToBatch (const entity_t* e)
{
	if (!qsafe_ent_valid (e))
		return false;

	// leerer Batch
	if (!ibuf.count)
		return true;

	// voller Batch
	if (ibuf.count >= (int)countof (ibuf.inst))
		return false;

	// verschiedene Modelle/Skins
	if (ibuf.ent->model != e->model || ibuf.ent->skinnum != e->skinnum)
		return false;

	// Spieler haben Custom Colors
	if (!gl_nocolors.value && CL_IsPlayerEnt (ibuf.ent))
		return false;

	return true;
}

/*
=================
R_DrawAliasModel_Real
Härtung:
- Null-/Culling-Checks
- NaN-sichere FOV-Skalierung
=================
*/
static void R_DrawAliasModel_Real (entity_t* e, qboolean showtris)
{
	if (!qsafe_ent_valid (e))
		return;

	aliashdr_t* paliashdr = (aliashdr_t*)Mod_Extradata (e->model);
	if (!paliashdr)
		return;

	lerpdata_t lerpdata;
	float      fovscale = 1.0f;
	float      model_matrix[16];

	// Pose/Lerp-Daten
	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (lerpdata.pose1 == lerpdata.pose2)
		lerpdata.blend = 0.0f;

	// Viewmodel-Anpassungen (Position, FOV-Distortion-Korrektur)
	if (e == &cl.viewent)
	{
		if (r_refdef.basefov > 90.f && cl_gun_fovscale.value)
		{
			float x = tanf (r_refdef.basefov * (0.5f * (float)M_PI / 180.f));
			if (!isfinite (x)) x = 1.0f;
			fovscale = 1.0f + (x - 1.0f) * cl_gun_fovscale.value;
			if (!isfinite (fovscale)) fovscale = 1.0f;
		}

		VectorMA (lerpdata.origin, cl_gun_x.value * paliashdr->scale[0] * fovscale, vright, lerpdata.origin);
		VectorMA (lerpdata.origin, cl_gun_y.value * paliashdr->scale[1] * fovscale, vup, lerpdata.origin);
		VectorMA (lerpdata.origin, cl_gun_z.value * paliashdr->scale[2], vpn, lerpdata.origin);
	}

	// Culling
	if (R_CullModelForEntity (e))
		return;

	// Transform
	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles, e->scale);
	ApplyTranslation (model_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
	ApplyScale (model_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);

	// Alpha
	if (r_lightmap_cheatsafe) // kein Alpha in drawflat/lightmap
		entalpha = 1.0f;
	else
		entalpha = ENTALPHA_DECODE (e->alpha);

	if (entalpha == 0.0f)
		return;

	// Lighting
	rs_aliaspolys += paliashdr->numtris;
	R_SetupAliasLighting (e);

	// Muzzle-Flash-Handling (Software-Renderer-Kompatibilität)
	if (e->effects & EF_MUZZLEFLASH)
	{
		if (e == &cl.viewent || paliashdr->poseverttype == PV_IQM)
			lightcolor[0] = lightcolor[1] = lightcolor[2] = 256.0f / 200.0f;
	}

	// Draw
	if (r_fullbright_cheatsafe || showtris)
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;

	if (showtris)
		entalpha = 1.0f;

	if (!R_Alias_CanAddToBatch (e))
		R_FlushAliasInstances (showtris);

	if (!ibuf.count)
		ibuf.ent = e;

	aliasinstance_t* instance = &ibuf.inst[ibuf.count++];
	instance->flags = ALIAS_INSTANCE_FLAG_NONE;
	if (e == &cl.viewent)
		instance->flags |= ALIAS_INSTANCE_FLAG_VIEWMODEL;
	if (e > cl_entities && e <= cl_entities + cl.maxclients)
		instance->flags |= ALIAS_INSTANCE_FLAG_PLAYER;
	if ((e->model->flags & MOD_FBRIGHTHACK) && gl_fullbrights.value && !gl_overbright_models.value)
		instance->flags |= ALIAS_INSTANCE_FLAG_FULLBRIGHT_HACK;
	if (e->model->flags & EF_ROTATE)
		instance->flags |= ALIAS_INSTANCE_FLAG_ITEM;
	if (paliashdr->poseverttype == PV_IQM)
		instance->flags |= ALIAS_INSTANCE_FLAG_FORCE_FULLBRIGHT;

	// Motion-Blur-Prev-Matrix
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
	}

	VectorCopy (lerpdata.origin, e->motion_blur_prev_origin);
	VectorCopy (lerpdata.angles, e->motion_blur_prev_angles);
	e->motion_blur_prev_frame = r_framecount;
	e->motion_blur_prev_valid = true;

	instance->lightcolor[0] = lightcolor[0];
	instance->lightcolor[1] = lightcolor[1];
	instance->lightcolor[2] = lightcolor[2];
	instance->alpha = entalpha;
	if (e == &cl.viewent)
		instance->flags |= ALIAS_INSTANCE_FLAG_NO_MOTION_BLUR;
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
R_DrawAliasModels
=================
*/
void R_DrawAliasModels (entity_t** ents, int count)
{
	if (!ents || count <= 0) return;

	for (int i = 0; i < count; i++)
		R_DrawAliasModel_Real (ents[i], false);

	R_FlushAliasInstances (false);
}

/*
=================
R_DrawAliasModels_ShowTris
=================
*/
void R_DrawAliasModels_ShowTris (entity_t** ents, int count)
{
	if (!ents || count <= 0) return;

	for (int i = 0; i < count; i++)
		R_DrawAliasModel_Real (ents[i], true);

	R_FlushAliasInstances (true);
}
