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
#include "../common/lightgrid.h"

extern cvar_t gl_overbright_models, gl_fullbrights, r_lerpmodels, r_lerpmove, r_model_halflambert; //johnfitz
extern cvar_t r_rim, r_rim_strength, r_rim_power, r_rim_staticScale, r_rim_dynScale, r_rim_ambScale;
extern cvar_t r_rim_gateK, r_rim_gateBias, r_rim_colorScale, r_rim_clampDirect, r_rim_clampAmb, r_rim_viewmodel, r_rim_debug;
extern cvar_t scr_fov, cl_gun_fovscale, cl_gun_x, cl_gun_y, cl_gun_z;
extern cvar_t r_oit;
extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_force;
extern cvar_t r_lightgrid_debug;
extern cvar_t r_shadow_bias_mdl;
extern cvar_t r_shadow_normalbias_mdl;
extern cvar_t r_shadows;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadow_pcf;
extern cvar_t r_shadow_pcf_taps;
extern cvar_t r_shadow_twosided_mdl;

//up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz -- changed to an array of pointers

const float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

extern vec3_t	lightcolor; //johnfitz -- replaces "float shadelight" for lit support

static float	entalpha; //johnfitz

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
	vec3_t		lightcolor;
	float		alpha;
	vec3_t		dlightcolor;
	float		_pad0;
	vec3_t		ambientcolor;
	float		_pad1;
	vec4_t		envmap_params;
	int32_t		pose1;
	int32_t		pose2;
	float		blend;
	int32_t		flags;
} aliasinstance_t;

#define ALIAS_INSTANCE_FLAG_NONE           0
#define ALIAS_INSTANCE_FLAG_NO_MOTION_BLUR (1 << 0)
#define ALIAS_INSTANCE_FLAG_VIEWMODEL      (1 << 1)
#define ALIAS_INSTANCE_FLAG_LIGHTNING      (1 << 2)

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
		float	_pad1;
		vec4_t	rim_params0; // x=enable y=scale z=power w=staticScale
		vec4_t	rim_params1; // x=dynScale y=ambientScale z=directK w=gateBias
		vec4_t	rim_params2; // x=colorScale y=clampDirect z=ambientLimit w=debug
		float	shadow_viewproj[16];
		vec4_t	shadow_params;
		vec4_t	shadow_debug;
		vec4_t	shadow_sun_dir;
	} global;
	aliasinstance_t inst[MAX_ALIAS_INSTANCES];
} ibuf;

COMPILE_TIME_ASSERT (alias_global_size_matches_std430, sizeof (ibuf.global) % 16 == 0);

static qboolean r_lightgrid_debug_sample_reported = false;
static const qmodel_t *r_lightgrid_debug_last_world = NULL;

static float R_AliasLuma (const vec3_t rgb)
{
	return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
}

static float R_AliasEnvIndoorHint (const entity_t *e)
{
	float luma = 0.f;
	float ao = 1.f;

	if (e && e->lightcache.lightgrid_has_sample)
	{
		luma = CLAMP (0.f, R_AliasLuma (e->lightcache.lightgrid_color), 1.f);
		ao = CLAMP (0.f, e->lightcache.lightgrid_ao, 1.f);
	}

	return CLAMP (0.f, (1.f - luma) * (1.f - 0.5f * ao), 1.f);
}

static void R_DebugLightgridSample (const entity_t *e, const vec3_t ambient_add)
{
        if (!r_lightgrid_debug.value)
        {
                r_lightgrid_debug_sample_reported = false;
                return;
        }

        if (cl.worldmodel != r_lightgrid_debug_last_world)
        {
                r_lightgrid_debug_sample_reported = false;
                r_lightgrid_debug_last_world = cl.worldmodel;
        }

        if (r_lightgrid_debug_sample_reported)
                return;

        r_lightgrid_debug_sample_reported = true;

        Con_Printf ("r_lightgrid_debug: %s probe rgb=(%.2f %.2f %.2f) ao=%.2f ambient_add=(%.1f %.1f %.1f)\n",
                e->model ? e->model->name : "<no model>",
                e->lightcache.lightgrid_color[0], e->lightcache.lightgrid_color[1], e->lightcache.lightgrid_color[2],
                e->lightcache.lightgrid_ao,
                ambient_add[0], ambient_add[1], ambient_add[2]);
}

static void R_ApplyLightgridLighting (const entity_t *e, vec3_t ambientcolor, vec3_t dlightcolor)
{
        vec3_t          gridcolor;

        if (!R_LightgridEnabled () || !e->lightcache.lightgrid_has_sample)
                return;

        VectorScale (e->lightcache.lightgrid_color, e->lightcache.lightgrid_ao * 255.f, gridcolor);
        if (gridcolor[0] == 0.f && gridcolor[1] == 0.f && gridcolor[2] == 0.f)
                return;

        {
                vec3_t ambient_add;
                for (int i = 0; i < 3; i++)
                {
                        ambientcolor[i] -= gridcolor[i];
                        if (ambientcolor[i] < 0.f)
                                ambientcolor[i] = 0.f;

                        ambientcolor[i] += gridcolor[i];

                        ambient_add[i] = gridcolor[i];
                }

                R_DebugLightgridSample (e, ambient_add);
        }
}

static const char *R_ModelTypeName (qmodel_t *model)
{
        if (!model)
                return "none";

        switch (model->type)
        {
        case mod_alias:
        {
                aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata (model);
                if (hdr && hdr->poseverttype == PV_IQM)
                        return "alias/iqm";
                return "alias";
        }
        case mod_sprite:
                return "sprite";
        case mod_brush:
                return "brush";
        default:
                return "unknown";
        }
}

static qboolean R_DebugItemLightEnabled (const entity_t *e)
{
        if (r_debug_itemlight.value <= 0.f)
                return false;

        if (!e || !e->model)
                return false;

        if (e == &cl.viewent)
                return false;

        return (e->model->flags & EF_ROTATE) != 0;
}

static void R_LogItemLight (const entity_t *e, const entity_lightinfo_t *info)
{
        if (!e || !e->model || !info)
                return;

        const char *cell = info->lightgrid_cell_valid ? va ("%d,%d,%d",
                info->lightgrid_cell[0], info->lightgrid_cell[1], info->lightgrid_cell[2]) : "n/a";
        const qboolean fullbright_hack = (!gl_overbright_models.value
                && (e->model->flags & MOD_FBRIGHTHACK) && gl_fullbrights.value);

        Con_Printf ("r_debug_itemlight: model=%s type=%s origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)\n",
                e->model->name, R_ModelTypeName (e->model),
                e->origin[0], e->origin[1], e->origin[2],
                e->angles[0], e->angles[1], e->angles[2]);
        Con_Printf ("  effects=0x%X model_flags=0x%X fullbright_cvar=%.0f fullbright_hack=%s\n",
                e->effects, e->model->flags, gl_fullbrights.value,
                fullbright_hack ? "yes" : "no");
        Con_Printf ("  static_rgb=(%.3f %.3f %.3f) intensity=%.3f minlight=%s\n",
                info->static_color[0], info->static_color[1], info->static_color[2],
                info->intensity, info->used_minlight ? "yes" : "no");
        Con_Printf ("  lightgrid=%s valid=%s cell=%s rgb=(%.3f %.3f %.3f) ao=%.2f\n",
                info->used_lightgrid ? "yes" : "no",
                info->lightgrid_valid ? "yes" : "no",
                cell,
                info->lightgrid_color[0], info->lightgrid_color[1], info->lightgrid_color[2],
                info->lightgrid_ao);
        Con_Printf ("  lightpoint=%s rgb=(%.3f %.3f %.3f)\n",
                info->used_lightpoint ? "yes" : "no",
                info->lightpoint_color[0], info->lightpoint_color[1], info->lightpoint_color[2]);
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame (entity_t *e, aliashdr_t *paliashdr, lerpdata_t *lerpdata)
{
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
        vec3_t          dist;
        float           add;
        unsigned int    i;
        vec3_t          dlightcolor = {0.f, 0.f, 0.f};
        vec3_t          ambientcolor;
        vec3_t          static_color;
        entity_lightinfo_t lightinfo;
        entity_lightinfo_t *lightinfo_ptr = r_debug_itemlight.value > 0.f ? &lightinfo : NULL;

        R_EntityStaticLight (e, static_color, lightinfo_ptr);
        VectorCopy (static_color, lightcolor);
        VectorCopy (static_color, ambientcolor);

        if (lightinfo_ptr && R_DebugItemLightEnabled (e))
                R_LogItemLight (e, lightinfo_ptr);

        if (lightinfo_ptr ? lightinfo_ptr->used_lightgrid : e->lightcache.lightgrid_has_sample)
        {
                R_AddDynamicLights_Lightgrid (e->origin, lightcolor);
                VectorSubtract (lightcolor, ambientcolor, dlightcolor);
        }
        else
        {
                //add dlights
                for (i=0; i<r_framedata.numlights; i++)
                {
                        gpulight_t *l = &r_lightbuffer.lights[i];
                        VectorSubtract (e->origin, l->pos, dist);
                        add = DotProduct (dist, dist);
                        if (l->radius * l->radius > add)
                        {
                                const float intensity = l->radius - sqrtf (add);
                                VectorMA (lightcolor, intensity, l->color, lightcolor);
                                VectorMA (dlightcolor, intensity, l->color, dlightcolor);
                        }
                }
        }

        R_ApplyLightgridLighting (e, ambientcolor, dlightcolor);

        // viewmodel lighting is typically darker because world lights aren't placed for a free camera
	if (e == &cl.viewent)
	{
		for (i = 0; i < 3; i++)
		{
			const float L = lightcolor[i];
			const float new_L = fmaxf (L * 1.5f, L + 40.0f);
			const float scale = L > 0.0f ? new_L / L : 0.0f;
			ambientcolor[i] *= scale;
			dlightcolor[i] *= scale;
			lightcolor[i] = new_L;
		}
	}

	// minimum light value on gun (24)
	if (e == &cl.viewent)
	{
		add = 72.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			add *= 1.0f / 3.0f;
			lightcolor[0] += add;
			lightcolor[1] += add;
			lightcolor[2] += add;
			ambientcolor[0] += add;
			ambientcolor[1] += add;
			ambientcolor[2] += add;
		}
	}

	// minimum light value on players (8)
	if (e > cl_entities && e <= cl_entities + cl.maxclients)
	{
		add = 24.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			add *= 1.0f / 3.0f;
			lightcolor[0] += add;
			lightcolor[1] += add;
			lightcolor[2] += add;
			ambientcolor[0] += add;
			ambientcolor[1] += add;
			ambientcolor[2] += add;
		}
	}

	//hack up the brightness when fullbrights but no overbrights (256)
	if (!gl_overbright_models.value && (e->model->flags & MOD_FBRIGHTHACK) && gl_fullbrights.value)
	{
		lightcolor[0] = 256.0f;
		lightcolor[1] = 256.0f;
		lightcolor[2] = 256.0f;
		VectorCopy (lightcolor, ambientcolor);
		VectorClear (dlightcolor);
	}

	{
		vec3_t pre_total;
		vec3_t linear_total;
		VectorAdd (ambientcolor, dlightcolor, pre_total);
		for (i = 0; i < 3; i++)
		{
			float L = pre_total[i] * (1.0f / 256.0f);
			L = fminf(L, 1.0f);
			linear_total[i] = L;
		}

		for (i = 0; i < 3; i++)
		{
			const float total = pre_total[i];
			const float ambient_ratio = total > 0.0f ? ambientcolor[i] / total : 0.0f;
			const float dlight_ratio = total > 0.0f ? dlightcolor[i] / total : 0.0f;
			ambientcolor[i] = linear_total[i] * ambient_ratio;
			dlightcolor[i] = linear_total[i] * dlight_ratio;
			lightcolor[i] = linear_total[i];
		}
	}

	VectorCopy (ambientcolor, e->lightcache.ambientcolor);
	VectorCopy (dlightcolor, e->lightcache.dlightcolor);
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
	int			skinnum, anim, mode;
	unsigned	state;
	GLuint		buf;
	GLbyte		*ofs;
	size_t		ibuf_size;
	GLuint		buffers[2];
	GLintptr	offsets[2];
	GLsizeiptr	sizes[2];
	gltexture_t	*textures[3];

	if (!ibuf.count)
		return;

	model = ibuf.ent->model;
	mainhdr = (aliashdr_t *)Mod_Extradata (model);
	anim = (int)(cl.time*10) & 3;

	GL_BeginGroup (model->name);

	md5 = mainhdr->poseverttype == PV_IQM;

	alphatest = model->flags & MF_HOLEY ? 1 : 0;
	translucent = !ENTALPHA_OPAQUE (ibuf.ent->alpha);
	oit = translucent && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT;
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
	GL_UseProgram (glprogs.alias[oit][mode][alphatest][md5]);

	if (md5)
		state = GLS_CULL_BACK | GLS_ATTRIBS(5);
	else
		state = GLS_CULL_BACK | GLS_ATTRIBS(1);

	if (!translucent)
		state |= GLS_BLEND_OPAQUE;
	else
		state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;
	GL_SetState (state);

memcpy (ibuf.global.matviewproj, r_matviewproj, sizeof (r_matviewproj));
memcpy (ibuf.global.prev_matviewproj, r_framedata.prev_viewproj, sizeof (r_framedata.prev_viewproj));
memcpy (ibuf.global.eyepos, r_refdef.vieworg, sizeof (r_refdef.vieworg));
memcpy (ibuf.global.fog, r_framedata.fogdata, 3 * sizeof (float));
// use fog density sign bit as overbright flag
ibuf.global.fog[3] =
gl_overbright_models.value ?
-fabs (r_framedata.fogdata[3]) :
 fabs (r_framedata.fogdata[3])
;
ibuf.global.overbright = gl_overbright_models.value > 0.f ? r_framedata.dither[2] : 1.f;
ibuf.global.dither = r_framedata.dither[0];
ibuf.global.half_lambert = CLAMP (0.f, r_model_halflambert.value, 1.f);
ibuf.global._pad1 = q_max (0.f, r_rim_viewmodel.value);
ibuf.global.rim_params0[0] = r_rim.value > 0.f ? 1.f : 0.f;
ibuf.global.rim_params0[1] = CLAMP (0.f, r_rim_strength.value, 1.f);
ibuf.global.rim_params0[2] = CLAMP (1.f, r_rim_power.value, 16.f);
ibuf.global.rim_params0[3] = q_max (0.f, r_rim_staticScale.value);
ibuf.global.rim_params1[0] = q_max (0.f, r_rim_dynScale.value);
ibuf.global.rim_params1[1] = q_max (0.f, r_rim_ambScale.value);
ibuf.global.rim_params1[2] = q_max (0.f, r_rim_gateK.value);
ibuf.global.rim_params1[3] = r_rim_gateBias.value;
ibuf.global.rim_params2[0] = q_max (0.f, r_rim_colorScale.value);
ibuf.global.rim_params2[1] = q_max (0.f, r_rim_clampDirect.value);
ibuf.global.rim_params2[2] = q_max (0.f, r_rim_clampAmb.value);
ibuf.global.rim_params2[3] = CLAMP (0.f, r_rim_debug.value, 3.f);
	memcpy (ibuf.global.shadow_viewproj, r_framedata.shadow_viewproj, sizeof (r_framedata.shadow_viewproj));
	ibuf.global.shadow_params[0] = r_shadow_bias_mdl.value;
	ibuf.global.shadow_params[1] = r_shadow_normalbias_mdl.value;
	ibuf.global.shadow_params[2] = r_shadow_pcf.value > 0.f ? 1.f : 0.f;
	ibuf.global.shadow_params[3] = r_shadow_pcf_taps.value;
	memcpy (ibuf.global.shadow_debug, r_framedata.shadow_debug, sizeof (r_framedata.shadow_debug));
	memcpy (ibuf.global.shadow_sun_dir, r_framedata.shadow_sun_dir, sizeof (r_framedata.shadow_sun_dir));

	ibuf_size = sizeof(ibuf.global) + sizeof(ibuf.inst[0]) * ibuf.count;
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &ibuf.global, ibuf_size, &buf, &ofs);

	buffers[0] = buf;
	offsets[0] = (GLintptr) ofs;
	sizes[0] = ibuf_size;

	GL_BindBuffer (GL_ARRAY_BUFFER, model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);
	GL_BindNative (GL_TEXTURE6, GL_TEXTURE_CUBE_MAP, (skybox && skybox->cubemap) ? skybox->cubemap->texnum : 0);
	R_Shadow_BindShadowMap (GL_TEXTURE5);
	R_Shadow_Log_ReceiverPassSnapshot ("ALIAS", glprogs.alias[oit][mode][alphatest][md5], GL_TEXTURE5, R_Shadow_GetShadowMapTextureId (), r_shadows.value > 0.f && r_shadow_sun.value > 0.f, r_shadow_bias_mdl.value, r_shadow_normalbias_mdl.value, r_shadow_pcf.value > 0.f ? 1.f : 0.f, r_shadow_pcf_taps.value, r_framedata.shadow_viewproj);

	for (hdr = mainhdr; hdr; hdr = hdr->nextsurface ? (aliashdr_t *) ((byte *)hdr + hdr->nextsurface) : NULL)
	{
		if (md5)
		{
			GL_VertexAttribPointerFunc  (0, 3, GL_FLOAT,			GL_FALSE, sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, xyz)));
			GL_VertexAttribPointerFunc  (1, 4, GL_BYTE,				GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, norm)));
			GL_VertexAttribPointerFunc  (2, 2, GL_FLOAT,			GL_FALSE, sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, st)));
			GL_VertexAttribPointerFunc  (3, 4, GL_UNSIGNED_BYTE,	GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, weight)));
			GL_VertexAttribIPointerFunc (4, 4, GL_UNSIGNED_BYTE,	          sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, idx)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vboposeofs;
			sizes[1] = sizeof (bonepose_t) * hdr->numbones * hdr->numboneposes;
		}
		else
		{
			GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof (meshst_t), (void *) hdr->vbostofs);

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

		GL_DrawElementsInstancedFunc (GL_TRIANGLES, hdr->numindexes, GL_UNSIGNED_SHORT, (void *)hdr->eboofs, ibuf.count);

		rs_aliaspasses += hdr->numtris * ibuf.count;
	}

	ibuf.count = 0;

	GL_EndGroup();
}

/*
=================
R_FlushAliasInstances_Shadow
=================
*/
static void R_FlushAliasInstances_Shadow (void)
{
	qmodel_t	*model;
	aliashdr_t	*mainhdr, *hdr;
	qboolean	md5;
	unsigned	state;
	GLuint		buf;
	GLbyte		*ofs;
	size_t		ibuf_size;
	GLuint		buffers[2];
	GLintptr	offsets[2];
	GLsizeiptr	sizes[2];

	if (!ibuf.count)
		return;

	model = ibuf.ent->model;
	mainhdr = (aliashdr_t *)Mod_Extradata (model);
	md5 = mainhdr->poseverttype == PV_IQM;

	GL_BeginGroup (model->name);

	GL_UseProgram (glprogs.shadow_depth_alias[md5]);

	if (md5)
		state = GLS_ATTRIBS(5);
	else
		state = GLS_ATTRIBS(1);

	if (r_shadow_twosided_mdl.value > 0.f)
		state |= GLS_CULL_NONE;
	else
		state |= GLS_CULL_FRONT;

	state |= GLS_BLEND_OPAQUE;
	GL_SetState (state);

	memcpy (ibuf.global.matviewproj, r_matviewproj, sizeof (r_matviewproj));
	memcpy (ibuf.global.prev_matviewproj, r_framedata.prev_viewproj, sizeof (r_framedata.prev_viewproj));
	memcpy (ibuf.global.eyepos, r_refdef.vieworg, sizeof (r_refdef.vieworg));
	memcpy (ibuf.global.shadow_viewproj, r_framedata.shadow_viewproj, sizeof (r_framedata.shadow_viewproj));
	ibuf.global.shadow_params[0] = r_shadow_bias_mdl.value;
	ibuf.global.shadow_params[1] = r_shadow_normalbias_mdl.value;
	ibuf.global.shadow_params[2] = r_shadow_pcf.value > 0.f ? 1.f : 0.f;
	ibuf.global.shadow_params[3] = r_shadow_pcf_taps.value;
	memcpy (ibuf.global.shadow_debug, r_framedata.shadow_debug, sizeof (r_framedata.shadow_debug));
	memcpy (ibuf.global.shadow_sun_dir, r_framedata.shadow_sun_dir, sizeof (r_framedata.shadow_sun_dir));

	ibuf_size = sizeof(ibuf.global) + sizeof(ibuf.inst[0]) * ibuf.count;
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &ibuf.global, ibuf_size, &buf, &ofs);

	buffers[0] = buf;
	offsets[0] = (GLintptr) ofs;
	sizes[0] = ibuf_size;

	GL_BindBuffer (GL_ARRAY_BUFFER, model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);

	for (hdr = mainhdr; hdr; hdr = hdr->nextsurface ? (aliashdr_t *) ((byte *)hdr + hdr->nextsurface) : NULL)
	{
		if (md5)
		{
			GL_VertexAttribPointerFunc  (0, 3, GL_FLOAT,			GL_FALSE, sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, xyz)));
			GL_VertexAttribPointerFunc  (1, 4, GL_BYTE,				GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, norm)));
			GL_VertexAttribPointerFunc  (2, 2, GL_FLOAT,			GL_FALSE, sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, st)));
			GL_VertexAttribPointerFunc  (3, 4, GL_UNSIGNED_BYTE,	GL_TRUE,  sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, weight)));
			GL_VertexAttribIPointerFunc (4, 4, GL_UNSIGNED_BYTE,	          sizeof (iqmvert_t), (void *) (hdr->vbovertofs + offsetof (iqmvert_t, idx)));

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vboposeofs;
			sizes[1] = sizeof (bonepose_t) * hdr->numbones * hdr->numboneposes;
		}
		else
		{
			GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof (meshst_t), (void *) hdr->vbostofs);

			buffers[1] = model->meshvbo;
			offsets[1] = hdr->vbovertofs;
			sizes[1] = sizeof (meshxyz_t) * hdr->numverts_vbo * hdr->numposes;
		}

		GL_BindBuffersRange (GL_SHADER_STORAGE_BUFFER, 1, 2, buffers, offsets, sizes);

		GL_DrawElementsInstancedFunc (GL_TRIANGLES, hdr->numindexes, GL_UNSIGNED_SHORT, (void *)hdr->eboofs, ibuf.count);

		rs_aliaspasses += hdr->numtris * ibuf.count;
	}

	ibuf.count = 0;

	GL_EndGroup();
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

        if (entalpha == 0)
                return;

        //
        // set up lighting
        //
	rs_aliaspolys += paliashdr->numtris;
	R_SetupAliasLighting (e);

	//
	// draw it
	//

        if (r_fullbright_cheatsafe || showtris)
        {
                lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;
                VectorCopy (lightcolor, e->lightcache.ambientcolor);
                VectorClear (e->lightcache.dlightcolor);
                e->lightcache.lightgrid_has_sample = false;
                e->lightcache.lightgrid_ao = 0.f;
                VectorClear (e->lightcache.lightgrid_color);
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
	}

	VectorCopy (lerpdata.origin, e->motion_blur_prev_origin);
	VectorCopy (lerpdata.angles, e->motion_blur_prev_angles);
	e->motion_blur_prev_frame = r_framecount;
	e->motion_blur_prev_valid = true;

	VectorCopy (lightcolor, instance->lightcolor);
	VectorCopy (e->lightcache.dlightcolor, instance->dlightcolor);
	VectorCopy (e->lightcache.ambientcolor, instance->ambientcolor);
	instance->alpha = entalpha;
	if (e == &cl.viewent)
		instance->flags |= ALIAS_INSTANCE_FLAG_NO_MOTION_BLUR | ALIAS_INSTANCE_FLAG_VIEWMODEL;
	if (!Q_strncmp (e->model->name, "progs/bolt", 10))
		instance->flags |= ALIAS_INSTANCE_FLAG_LIGHTNING;
	{
		const qboolean env_hint = (instance->flags & ALIAS_INSTANCE_FLAG_LIGHTNING)
			|| (e->model->flags & MOD_FBRIGHTHACK);
		const float indoor_hint = R_AliasEnvIndoorHint (e);
		const float envmap_base = (instance->flags & ALIAS_INSTANCE_FLAG_LIGHTNING) ? 0.95f : 0.55f;
		const float indoor_dampen = CLAMP (0.f, r_envlight_indoor_dampen.value, 1.f);

		instance->envmap_params[0] = (r_envlight.value > 0.f && r_envlight_envmap.value > 0.f && env_hint) ? 1.f : 0.f;
		instance->envmap_params[1] = envmap_base * CLAMP (0.f, r_envlight_envmap.value, 1.f);
		instance->envmap_params[2] = indoor_hint;
		instance->envmap_params[3] = (instance->flags & ALIAS_INSTANCE_FLAG_LIGHTNING)
			? 0.14f
			: 0.08f + indoor_hint * indoor_dampen * 0.18f;
	}
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
R_DrawAliasModel_Shadow_Real
=================
*/
static void R_DrawAliasModel_Shadow_Real (entity_t *e)
{
	aliashdr_t	*paliashdr;
	lerpdata_t	lerpdata;
	float		model_matrix[16];
	aliasinstance_t	*instance;
	float		entalpha;

	if (!e || !e->model)
		return;

	if (e == &cl.viewent)
		return;

	if (e->model->flags & MOD_NOSHADOW)
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);

	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (lerpdata.pose1 == lerpdata.pose2)
		lerpdata.blend = 0.f;

	if (R_CullModelForEntity (e))
		return;

	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles, e->scale);
	ApplyTranslation (model_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
	ApplyScale (model_matrix, paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);

	entalpha = ENTALPHA_DECODE (e->alpha);
	if (entalpha == 0.f)
		return;

	if (!R_Alias_CanAddToBatch (e))
		R_FlushAliasInstances_Shadow ();

	if (!ibuf.count)
		ibuf.ent = e;

	instance = &ibuf.inst[ibuf.count++];
	instance->flags = ALIAS_INSTANCE_FLAG_NONE;

	MatrixTranspose4x3 (model_matrix, instance->worldmatrix);
	MatrixTranspose4x3 (model_matrix, instance->prev_worldmatrix);

	VectorClear (instance->lightcolor);
	VectorClear (instance->dlightcolor);
	VectorClear (instance->ambientcolor);
	instance->alpha = entalpha;
	Vector4Set (instance->envmap_params, 0.f, 0.f, 0.f, 0.f);
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
void R_DrawAliasModels (entity_t **ents, int count)
{
        int i;
        for (i = 0; i < count; i++)
                R_DrawAliasModel_Real (ents[i], false);
        R_FlushAliasInstances (false);
}

/*
=================
R_DrawAliasModels_Shadow
=================
*/
void R_DrawAliasModels_Shadow (entity_t **ents, int count)
{
	int i;
	for (i = 0; i < count; i++)
		R_DrawAliasModel_Shadow_Real (ents[i]);
	R_FlushAliasInstances_Shadow ();
}

/*
=================
R_DrawAliasModels_ShowTris
=================
*/
void R_DrawAliasModels_ShowTris (entity_t **ents, int count)
{
        int i;
        for (i = 0; i < count; i++)
                R_DrawAliasModel_Real (ents[i], true);
        R_FlushAliasInstances (true);
}
