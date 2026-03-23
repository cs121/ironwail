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
// r_misc.c

#include "quakedef.h"
#include "gl_lightgrid.h"
#include "mat_material.h"
#include "r_dlight_pool.h"
#include "r_postfx.h"
#include "r_quality.h"
#include "r_ssao.h"
#include "r_godrays.h"
#include "r_fogvol.h"
#include "r_realtimelight.h"

//johnfitz -- new cvars
extern cvar_t r_clearcolor;
extern cvar_t r_flatlightstyles;
extern cvar_t r_lerplightstyles;
extern cvar_t gl_fullbrights;
extern cvar_t gl_farclip;
extern cvar_t gl_overbright_models;
extern cvar_t r_viewmodel_light_boost;
extern cvar_t r_viewmodel_minlight;
extern cvar_t r_model_halflambert;
extern cvar_t r_facenormals_enable;
extern cvar_t r_overbrightbits;
extern cvar_t r_waterwarp;
extern cvar_t r_oldskyleaf;
extern cvar_t r_drawworld;
extern cvar_t r_showtris;
extern cvar_t r_showbboxes;
extern cvar_t r_showbboxes_think;
extern cvar_t r_showbboxes_health;
extern cvar_t r_showbboxes_links;
extern cvar_t r_showbboxes_targets;
extern cvar_t r_showfields;
extern cvar_t r_showfields_align;
extern cvar_t r_lerpmodels;
extern cvar_t r_lerpmove;
extern cvar_t r_nolerp_list;
extern cvar_t r_noshadow_list;
extern cvar_t r_lightmap_linear;
extern cvar_t r_lightmap_mipmaps;
extern cvar_t r_lightmap16f;
extern cvar_t r_lightingdir;
extern cvar_t r_dlight_entities;
extern cvar_t r_quality;
extern cvar_t r_model_light_multisample;
extern cvar_t r_model_light_smooth;
extern cvar_t r_dlight_models_directional;
extern cvar_t r_model_lightgrid_assist;
extern cvar_t r_model_lightgrid_assist_threshold;
extern cvar_t r_bmodel_relight;
extern cvar_t r_model_light_stats;
extern cvar_t r_model_light_samples_max;
//johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_alphasort;
extern cvar_t r_oit;
extern cvar_t r_dither;
extern cvar_t r_dof;
extern cvar_t r_dof_autofocus;
extern cvar_t r_dof_focus;
extern cvar_t r_dof_range;
extern cvar_t r_dof_strength;
extern cvar_t r_motionblur;
extern cvar_t r_motionblur_shutter;
extern cvar_t r_motionblur_maxradiuspixels;
extern cvar_t r_motionblur_maxsamples;
extern cvar_t r_motionblur_minvelocity;
extern cvar_t r_motionblur_depththreshold;
extern cvar_t r_tonemap;
extern cvar_t r_tonemap_exposure;
extern cvar_t r_autoexposure;
extern cvar_t r_autoexposure_async;
extern cvar_t r_ae_min_scene_luma;
extern cvar_t r_ae_min_exposure;
extern cvar_t r_ae_max_exposure;
extern cvar_t r_exposure_bias;
extern cvar_t r_exposure_min;
extern cvar_t r_exposure_max;
extern cvar_t r_exposure_speed_up;
extern cvar_t r_exposure_speed_down;
extern cvar_t r_exposure_lock;
extern cvar_t r_exposure_debug;
cvar_t r_tonemap_black_lift = {"r_tonemap_black_lift", "0", CVAR_ARCHIVE};
cvar_t r_tonemap_black_lift_strength = {"r_tonemap_black_lift_strength", "0.0", CVAR_ARCHIVE};
extern cvar_t r_srgb_textures;
extern cvar_t r_srgb_framebuffer;
extern cvar_t r_debug_colorspace;
extern cvar_t r_lighting_debug_view;
extern cvar_t r_color_midtone;
extern cvar_t r_color_contrast;
extern cvar_t r_color_saturation;
extern cvar_t r_lightmap_colorspace;
extern cvar_t r_lightmap_colorspace_debug;
extern cvar_t r_bloom;
extern void TexMgr_SRGBTextures_f (cvar_t *var);
extern void TexMgr_LightmapColorspace_f (cvar_t *var);
extern void TexMgr_LightmapLinearCompat_f (cvar_t *var);
extern cvar_t r_bloom_threshold;
extern cvar_t r_vignette;
extern cvar_t r_vignette_radius_inner;
extern cvar_t r_vignette_radius_outer;
extern cvar_t r_vignette_falloff;
extern cvar_t r_vignette_color_r;
extern cvar_t r_vignette_color_g;
extern cvar_t r_vignette_color_b;
extern cvar_t r_vignette_blend_mode;
extern cvar_t r_vignette_noise;
extern cvar_t r_teleportfx;
extern cvar_t r_teleportfx_time;
extern cvar_t r_sun_light;
extern cvar_t r_shadow;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadow_dlight;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_sun_size;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_sun_distance;
extern cvar_t r_shadow_sun_bias;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_receiver_bias;
extern cvar_t r_shadow_sun_pcf;
extern cvar_t r_shadow_dlight_pcf;
extern cvar_t r_shadow_sun_snap;
extern cvar_t r_shadow_sun_cascades;
extern cvar_t r_shadow_sun_split1;
extern cvar_t r_shadow_sun_split2;
extern cvar_t r_shadow_sun_split3;
extern cvar_t r_shadow_sun_split_mode;
extern cvar_t r_shadow_sun_split_lambda;
extern cvar_t r_shadow_mark_mode;
extern cvar_t r_shadow_profile;
extern cvar_t r_shadow_cull_vis;
extern cvar_t r_shadow_cull_backface;
extern cvar_t r_shadow_cull_frustum;
extern cvar_t r_shadow_cull_sphere;
extern cvar_t r_shadow_debug;
extern cvar_t r_shadow_log;
extern cvar_t r_godrays_decay;
extern cvar_t r_godrays_density;

#if defined(USE_SIMD)
extern cvar_t r_simd;
#endif
qboolean use_simd;

extern gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz

static qboolean r_gl_nocolors_enabled;

extern char r_showbboxes_filter_strings[MAXCMDLINE];
extern qboolean r_showbboxes_filter_byindex;

/*
====================
R_ShowbboxesFilter_f
====================
*/
static void R_ShowbboxesFilter_f (void)
{
	if (Cmd_Argc () >= 2)
	{
		int i, len, ofs;
		r_showbboxes_filter_byindex = false;
		for (i = 1, ofs = 0; i < Cmd_Argc (); i++)
		{
			const char *arg = Cmd_Argv (i);
			if (!*arg)
				continue;
			len = strlen (arg) + 1;
			if (ofs + len + 1 > (int) countof (r_showbboxes_filter_strings))
			{
				Con_Warning ("overflow at \"%s\"\n", arg);
				break;
			}
			r_showbboxes_filter_byindex |= (arg[0] == '#');
			memcpy (&r_showbboxes_filter_strings[ofs], arg, len);
			ofs += len;
		}
		r_showbboxes_filter_strings[ofs++] = '\0';
	}
	else
	{
		const char *p = r_showbboxes_filter_strings;
		Con_SafePrintf ("\"r_showbboxes_filter\" is");
		if (!*p)
			Con_SafePrintf (" \"\"");
		else do
		{
			Con_SafePrintf (" \"%s\"", p);
			p += strlen (p) + 1;
		} while (*p);
		Con_SafePrintf ("\n");
	}
}

/*
====================
R_ShowbboxesFilter_Completion_f -- tab completion for r_showbboxes_filter
====================
*/
static void R_ShowbboxesFilter_Completion_f (const char *partial)
{
	extern edict_t *sv_player;
	extern edict_t **bbox_linked;
	qcvm_t	*oldvm;
	edict_t	*ed;
	int		i;

	if (!sv.active)
		return;

	PR_PushQCVM (&sv.qcvm, &oldvm);

	if (*partial == '#')
	{
		for (i = 0; i < (int) VEC_SIZE (bbox_linked); i++)
		{
			ed = bbox_linked[i];
			Con_AddToTabList (va ("#%d", NUM_FOR_EDICT (ed)), partial, PR_GetString (ed->v.classname));
		}
	}
	else
	{
		for (i = 1, ed = NEXT_EDICT (qcvm->edicts); i < qcvm->num_edicts; i++, ed = NEXT_EDICT (ed))
		{
			const char *name;
			if (ed == sv_player || ed->free || !ed->v.classname)
				continue;
			name = PR_GetString (ed->v.classname);
			if (*name)
				Con_AddToTabList (name, partial, "#");
		}
	}

	PR_PopQCVM (oldvm);
}

/*
====================
R_ShowbboxesFilterClear_f
====================
*/
static void R_ShowbboxesFilterClear_f (void)
{
	r_showbboxes_filter_strings[0] = '\0';
	r_showbboxes_filter_byindex = false;
}

/*
====================
GL_Fullbrights_f -- johnfitz
====================
*/
static void GL_Fullbrights_f (cvar_t *var)
{
	TexMgr_ReloadNobrightImages ();
}

/*
====================
GL_NoColors_f
====================
*/
static void GL_NoColors_f (cvar_t *var)
{
	const qboolean gl_nocolors_enabled = (var->value != 0.0f);

	if (r_gl_nocolors_enabled && !gl_nocolors_enabled)
	{
		int i;
		for (i = 0; i < cl.maxclients; i++)
		{
			entity_t *e = &cl_entities[1 + i];

			if (!e->model)
				continue;

			if (playertextures[i])
				R_TranslatePlayerSkin (i);
			else
				R_TranslateNewPlayerSkin (i);
		}
	}

	r_gl_nocolors_enabled = gl_nocolors_enabled;
}

/*
====================
R_SetClearColor_f -- johnfitz
====================
*/
static void R_SetClearColor_f (cvar_t *var)
{
	byte	*rgb;
	int		s;

	s = (int)r_clearcolor.value & 0xFF;
	rgb = (byte*)(d_8to24table + s);
	glClearColor (rgb[0]/255.0,rgb[1]/255.0,rgb[2]/255.0,0);
}

/*
===============
R_Model_ExtraFlags_List_f -- johnfitz -- called when r_nolerp_list cvar changes
===============
*/
static void R_Model_ExtraFlags_List_f (cvar_t *var)
{
	int i;
	for (i=0; i < MAX_MODELS; i++)
		Mod_SetExtraFlags (cl.model_precache[i]);
}

#if defined(USE_SIMD)
/*
====================
R_SIMD_f
====================
*/
static void R_SIMD_f (cvar_t *var)
{
#if defined(USE_SSE2)
	use_simd = SDL_HasSSE() && SDL_HasSSE2() && (var->value != 0.0f);
#else
	#error not implemented
#endif
}
#endif

/*
====================
R_SetWateralpha_f -- ericw
====================
*/
static void R_SetWateralpha_f (cvar_t *var)
{
	float alpha = CLAMP(0.f, var->value, 1.f);
	if (alpha != var->value)
		Cvar_SetValueQuick (var, alpha);

	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent&SURF_DRAWWATER) && alpha < 1)
				Con_Warning("Map does not appear to be water-vised\n");
	map_wateralpha = alpha;
	map_fallbackalpha = alpha;
}

/*
====================
R_SetTelealpha_f -- ericw
====================
*/
static void R_SetTelealpha_f (cvar_t *var)
{
	float alpha = CLAMP(0.f, var->value, 1.f);
	if (alpha != var->value)
		Cvar_SetValueQuick (var, alpha);

	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent&SURF_DRAWTELE) && alpha&& alpha < 1)
				Con_Warning("Map does not appear to be tele-vised\n");
	map_telealpha = alpha;
}

/*
====================
R_SetSlimealpha_f -- ericw
====================
*/
static void R_SetSlimealpha_f (cvar_t *var)
{
	float alpha = CLAMP(0.f, var->value, 1.f);
	if (alpha != var->value)
		Cvar_SetValueQuick (var, alpha);

	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent&SURF_DRAWSLIME) && alpha && alpha < 1)
			Con_Warning("Map does not appear to be slime-vised\n");
	map_slimealpha = alpha;
}

/*
====================
R_OverbrightBits_f
====================
*/
static void R_OverbrightBits_f (cvar_t *var)
{
	int value = CLAMP (0, (int)Q_rint (var->value), 3);
	if (value != (int)var->value)
		Cvar_SetValueQuick (var, (float)value);
}

/*
====================
GL_WaterAlphaForTextureType
====================
*/
float GL_WaterAlphaForTextureType (textype_t type)
{
	if (type == TEXTYPE_LAVA)
		return map_lavaalpha;
	else if (type == TEXTYPE_TELE)
		return map_telealpha > 0 ? map_telealpha : map_fallbackalpha;
	else if (type == TEXTYPE_SLIME)
		return map_slimealpha > 0 ? map_slimealpha : map_fallbackalpha;
	else
		return map_wateralpha;
}



/*
===============
R_Init
===============
*/
void R_Init (void)
{
        cmd_function_t *cmd;

        Cmd_AddCommand ("timerefresh", R_TimeRefresh_f);
        Cmd_AddCommand ("pointfile", R_ReadPointFile_f);
        cmd = Cmd_AddCommand ("r_showbboxes_filter", R_ShowbboxesFilter_f);
        if (cmd)
                cmd->completion = R_ShowbboxesFilter_Completion_f;
        Cmd_AddCommand ("r_showbboxes_filter_clear", R_ShowbboxesFilterClear_f);

        Lightgrid_Init ();
        Material_Init ();

Cvar_RegisterVariable (&r_norefresh);
Cvar_RegisterVariable (&r_lightmap);
Cvar_RegisterVariable (&r_lightmap_linear);
Cvar_SetCallback (&r_lightmap_linear, TexMgr_LightmapLinearCompat_f);
Cvar_RegisterVariable (&r_lightmap_colorspace);
Cvar_SetCallback (&r_lightmap_colorspace, TexMgr_LightmapColorspace_f);
Cvar_RegisterVariable (&r_lightmap_colorspace_debug);
Cvar_RegisterVariable (&r_lightmap_mipmaps);
Cvar_RegisterVariable (&r_lightmap16f);
Cvar_RegisterVariable (&r_lightingdir);
Cvar_RegisterVariable (&r_rgblighting_enable);
Cvar_RegisterVariable (&r_fullbright);
Cvar_RegisterVariable (&r_drawentities);
Cvar_RegisterVariable (&r_drawviewmodel);
        Cvar_RegisterVariable (&r_debug_itemlight);
        Cvar_RegisterVariable (&r_minlight_models);
        Cvar_RegisterVariable (&r_model_lightgrid);
        Cvar_RegisterVariable (&r_model_light_multisample);
        Cvar_RegisterVariable (&r_model_light_smooth);
        Cvar_RegisterVariable (&r_dlight_models_directional);
        Cvar_RegisterVariable (&r_model_lightgrid_assist);
        Cvar_RegisterVariable (&r_model_lightgrid_assist_threshold);
        Cvar_RegisterVariable (&r_bmodel_relight);
        Cvar_RegisterVariable (&r_model_light_stats);
        Cvar_RegisterVariable (&r_model_light_samples_max);
        Cvar_RegisterVariable (&r_wateralpha);
        Cvar_SetCallback (&r_wateralpha, R_SetWateralpha_f);
        Cvar_RegisterVariable (&r_litwater);
        Cvar_RegisterVariable (&r_dynamic);
        Cvar_RegisterVariable (&r_dlight_entities);
	Cvar_RegisterVariable (&r_quality);
	Cvar_RegisterVariable (&r_shadow);
	Cvar_RegisterVariable (&r_shadow_sun);
	Cvar_RegisterVariable (&r_shadow_dlight);
	Cvar_RegisterVariable (&r_shadow_dlight_max);
	Cvar_RegisterVariable (&r_shadow_sun_size);
	Cvar_RegisterVariable (&r_shadow_dlight_size);
	Cvar_RegisterVariable (&r_shadow_sun_distance);
	Cvar_RegisterVariable (&r_shadow_sun_bias);
	Cvar_RegisterVariable (&r_shadow_dlight_bias);
	Cvar_RegisterVariable (&r_shadow_receiver_bias);
	Cvar_RegisterVariable (&r_shadow_sun_pcf);
	Cvar_RegisterVariable (&r_shadow_dlight_pcf);
	Cvar_RegisterVariable (&r_shadow_sun_snap);
	Cvar_RegisterVariable (&r_shadow_sun_cascades);
	Cvar_RegisterVariable (&r_shadow_sun_split1);
	Cvar_RegisterVariable (&r_shadow_sun_split2);
	Cvar_RegisterVariable (&r_shadow_sun_split3);
	Cvar_RegisterVariable (&r_shadow_sun_split_mode);
	Cvar_RegisterVariable (&r_shadow_sun_split_lambda);
	Cvar_RegisterVariable (&r_shadow_mark_mode);
	Cvar_RegisterVariable (&r_shadow_profile);
	Cvar_RegisterVariable (&r_shadow_cull_vis);
	Cvar_RegisterVariable (&r_shadow_cull_backface);
	Cvar_RegisterVariable (&r_shadow_cull_frustum);
	Cvar_RegisterVariable (&r_shadow_cull_sphere);
	Cvar_RegisterVariable (&r_shadow_debug);
	Cvar_RegisterVariable (&r_shadow_log);
	Cvar_RegisterVariable (&r_rimlight);
	Cvar_RegisterVariable (&r_rimlight_world);
	Cvar_RegisterVariable (&r_rimlight_models);
	Cvar_RegisterVariable (&r_rimlight_intensity);
	Cvar_RegisterVariable (&r_rimlight_power);
	Cvar_RegisterVariable (&r_rimlight_sun);
	Cvar_RegisterVariable (&r_rimlight_dlight);
	Cvar_RegisterVariable (&r_rimlight_shadow);
        Cvar_RegisterVariable (&r_novis);
#if defined(USE_SIMD)
        Cvar_RegisterVariable (&r_simd);
        Cvar_SetCallback (&r_simd, R_SIMD_f);
	R_SIMD_f(&r_simd);
#endif
	Cvar_RegisterVariable (&r_speeds);
	Cvar_RegisterVariable (&r_pos);
	Cvar_RegisterVariable (&r_alphasort);
	Cvar_RegisterVariable (&r_oit);
	Cvar_RegisterVariable (&r_dither);
	Cvar_RegisterVariable (&r_dof);
        Cvar_RegisterVariable (&r_dof_autofocus);
	Cvar_RegisterVariable (&r_dof_focus);
	Cvar_RegisterVariable (&r_dof_range);
Cvar_RegisterVariable (&r_dof_strength);
Cvar_RegisterVariable (&r_motionblur);
Cvar_RegisterVariable (&r_motionblur_shutter);
Cvar_RegisterVariable (&r_motionblur_maxradiuspixels);
Cvar_RegisterVariable (&r_motionblur_maxsamples);
Cvar_RegisterVariable (&r_motionblur_minvelocity);
Cvar_RegisterVariable (&r_motionblur_depththreshold);
Cvar_RegisterVariable (&r_tonemap);
Cvar_RegisterVariable (&r_tonemap_exposure);
Cvar_RegisterVariable (&r_autoexposure);
Cvar_RegisterVariable (&r_autoexposure_async);
Cvar_RegisterVariable (&r_ae_min_scene_luma);
Cvar_RegisterVariable (&r_ae_min_exposure);
Cvar_RegisterVariable (&r_ae_max_exposure);
Cvar_RegisterVariable (&r_exposure_bias);
Cvar_RegisterVariable (&r_exposure_min);
Cvar_RegisterVariable (&r_exposure_max);
Cvar_RegisterVariable (&r_exposure_speed_up);
Cvar_RegisterVariable (&r_exposure_speed_down);
Cvar_RegisterVariable (&r_exposure_lock);
Cvar_RegisterVariable (&r_exposure_debug);
Cvar_RegisterVariable (&r_tonemap_black_lift);
Cvar_RegisterVariable (&r_tonemap_black_lift_strength);
Cvar_RegisterVariable (&r_srgb_textures);
Cvar_SetCallback (&r_srgb_textures, TexMgr_SRGBTextures_f);
	Cvar_RegisterVariable (&r_srgb_framebuffer);
	Cvar_RegisterVariable (&r_debug_colorspace);
	Cvar_RegisterVariable (&r_lighting_debug_view);
	Cvar_RegisterVariable (&r_color_midtone);
	Cvar_RegisterVariable (&r_color_contrast);
	Cvar_RegisterVariable (&r_color_saturation);
	Cvar_RegisterVariable (&r_bloom);
	Cvar_RegisterVariable (&r_bloom_threshold);
	R_PPdlights_RegisterCvars ();
	/* CVar owners: r_postfx* in r_postfx.c, r_ssao* in r_ssao.c, r_godray* and r_godrays* in r_godrays.c. */
	R_PostFX_RegisterCvars ();
	R_SSAO_RegisterCvars ();
	R_Godrays_RegisterCvars ();
Cvar_RegisterVariable (&r_vignette);
	Cvar_RegisterVariable (&r_vignette_radius_inner);
	Cvar_RegisterVariable (&r_vignette_radius_outer);
	Cvar_RegisterVariable (&r_vignette_falloff);
	Cvar_RegisterVariable (&r_vignette_color_r);
	Cvar_RegisterVariable (&r_vignette_color_g);
	Cvar_RegisterVariable (&r_vignette_color_b);
	Cvar_RegisterVariable (&r_vignette_blend_mode);
	Cvar_RegisterVariable (&r_vignette_noise);
	Cvar_RegisterVariable (&r_teleportfx);
	Cvar_RegisterVariable (&r_teleportfx_time);
        Cvar_RegisterVariable (&r_overbrightbits);
        Cvar_SetCallback (&r_overbrightbits, R_OverbrightBits_f);

	Cvar_RegisterVariable (&gl_finish);
	Cvar_RegisterVariable (&gl_clear);
	Cvar_RegisterVariable (&gl_polyblend);
	Cvar_RegisterVariable (&gl_playermip);
	Cvar_RegisterVariable (&gl_nocolors);
	Cvar_SetCallback (&gl_nocolors, GL_NoColors_f);
	r_gl_nocolors_enabled = (gl_nocolors.value != 0.0f);

	//johnfitz -- new cvars
	Cvar_RegisterVariable (&r_clearcolor);
	Cvar_SetCallback (&r_clearcolor, R_SetClearColor_f);
	Cvar_RegisterVariable (&r_waterwarp);
	Cvar_RegisterVariable (&r_flatlightstyles);
	Cvar_RegisterVariable (&r_lerplightstyles);
	Cvar_RegisterVariable (&r_facenormals_enable);
	Cvar_RegisterVariable (&r_oldskyleaf);
	Cvar_RegisterVariable (&r_drawworld);
	Cvar_RegisterVariable (&r_showtris);
	Cvar_RegisterVariable (&r_showbboxes);
	Cvar_RegisterVariable (&r_showbboxes_think);
	Cvar_RegisterVariable (&r_showbboxes_health);
	Cvar_RegisterVariable (&r_showbboxes_links);
	Cvar_RegisterVariable (&r_showbboxes_targets);
	Cvar_RegisterVariable (&r_showfields);
	Cvar_RegisterVariable (&r_showfields_align);
	Cvar_RegisterVariable (&gl_farclip);
	Cvar_RegisterVariable (&gl_fullbrights);
	Cvar_RegisterVariable (&gl_lightmap_atlas_size);
	Cvar_RegisterVariable (&gl_lightmap_format_cvar);
	Cvar_SetCallback (&gl_lightmap_atlas_size, GL_OnLightmapAtlasSizeChanged);
	GL_OnLightmapAtlasSizeChanged (&gl_lightmap_atlas_size);
	Cvar_SetCallback (&gl_fullbrights, GL_Fullbrights_f);
	Cvar_RegisterVariable (&gl_overbright_models);
	Cvar_RegisterVariable (&r_viewmodel_light_boost);
	Cvar_RegisterVariable (&r_viewmodel_minlight);
	Cvar_RegisterVariable (&r_model_halflambert);
	Cvar_RegisterVariable (&r_gl_state_validate);
	Cvar_RegisterVariable (&r_framegraph_autobind);
	Cvar_RegisterVariable (&r_framegraph_debug);
	Cvar_RegisterVariable (&r_lerpmodels);
	Cvar_RegisterVariable (&r_lerpmove);
	Cvar_RegisterVariable (&r_nolerp_list);
	Cvar_RegisterVariable (&r_noshadow_list);
	Cvar_SetCallback (&r_nolerp_list, R_Model_ExtraFlags_List_f);
	Cvar_SetCallback (&r_noshadow_list, R_Model_ExtraFlags_List_f);
	//johnfitz

	Cvar_RegisterVariable (&gl_zfix); // QuakeSpasm z-fighting fix
	Cvar_RegisterVariable (&r_telealpha);
	Cvar_RegisterVariable (&r_slimealpha);
	Cvar_RegisterVariable (&r_scale);
	Cvar_RegisterVariable (&r_sun_light);
	Cvar_SetCallback (&r_telealpha, R_SetTelealpha_f);
	Cvar_SetCallback (&r_slimealpha, R_SetSlimealpha_f);

	/* CVar owner: r_fogvol* in r_fogvol.c. */
	R_FogVol_RegisterCvars ();
	R_Quality_Init ();
	R_PostFX_Init ();
	R_FogVol_Init ();

	R_InitParticles ();
	R_InitDecals ();
	R_SetClearColor_f (&r_clearcolor); //johnfitz

	Sky_Init (); //johnfitz
	Fog_Init (); //johnfitz
}

/*
===============
R_TranslatePlayerSkin -- johnfitz -- rewritten.  also, only handles new colors, not new skins
===============
*/
void R_TranslatePlayerSkin (int playernum)
{
	int			top, bottom;

	top = (cl.scores[playernum].colors & 0xf0)>>4;
	bottom = cl.scores[playernum].colors &15;

	//FIXME: if gl_nocolors is on, then turned off, the textures may be out of sync with the scoreboard colors.
	if (!gl_nocolors.value)
	{
		if (playertextures[playernum])
			TexMgr_ReloadImage (playertextures[playernum], top, bottom);
	}
}

/*
===============
R_TranslateNewPlayerSkin -- johnfitz -- split off of TranslatePlayerSkin -- this is called when
the skin or model actually changes, instead of just new colors
added bug fix from bengt jardup
===============
*/
void R_TranslateNewPlayerSkin (int playernum)
{
	char		name[64];
	byte		*pixels;
	aliashdr_t	*paliashdr;
	entity_t	*e;
	int		skinnum;

//get correct texture pixels
	e = &cl_entities[1+playernum];

	if (!e->model || e->model->type != mod_alias)
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);

	skinnum = e->skinnum;

	//TODO: move these tests to the place where skinnum gets received from the server
	if (skinnum < 0 || skinnum >= paliashdr->numskins)
	{
		Con_DPrintf("(%d): Invalid player skin #%d\n", playernum, skinnum);
		skinnum = 0;
	}

	pixels = (byte *)paliashdr + paliashdr->texels[skinnum]; // This is not a persistent place!

//upload new image
	q_snprintf(name, sizeof(name), "player_%i", playernum);
	playertextures[playernum] = TexMgr_LoadImage (e->model, name, paliashdr->skinwidth, paliashdr->skinheight,
		SRC_INDEXED, pixels, paliashdr->gltextures[skinnum][0]->source_file, paliashdr->gltextures[skinnum][0]->source_offset, TEXPREF_PAD | TEXPREF_OVERWRITE);

//now recolor it
	R_TranslatePlayerSkin (playernum);
}

/*
===============
R_NewGame -- johnfitz -- handle a game switch
===============
*/

typedef struct
{
	qboolean dir_defined;
	qboolean color_defined;
	qboolean intensity_defined;
} sun_presence_t;

static qboolean r_worldspawn_has_sun = false;
static sun_presence_t r_worldspawn_sun_presence;
sun_t r_sun_state;
cvar_t r_sun_light = { "r_sun_light", "1", CVAR_ARCHIVE };
// Enables dynamic directional sunlight from parsed worldspawn sun keys (not baked lightmaps).

static void R_SetDefaultSunDirection (vec3_t dir)
{
	vec3_t ang;
	ang[PITCH] = 45.0f;
	ang[YAW] = 225.0f;
	ang[ROLL] = 0.0f;
	AngleVectors (ang, dir, NULL, NULL);
}

void R_Sun_ResetToDefaults (sun_t *s)
{
	if (!s)
		return;
	R_SetDefaultSunDirection (s->dir);
	VectorSet (s->color, 1.f, 1.f, 1.f);
	s->intensity = 1.f;
	s->volumetric_intensity = 1.f;
	s->anisotropy = 0.f;
	s->shadow_bias = 0.001f;
	s->shadow_strength = CLAMP (0.f, r_fogvol_shadow_strength.value, 4.f);
	s->shadow_distance = 0.f;
	s->ray_decay = CLAMP (0.f, r_godrays_decay.value, 1.f);
	s->ray_density = q_max (0.f, r_godrays_density.value);
}

void R_Sun_ApplyMapOverrides (sun_t *s, const char *worldspawn_entity_string)
{
	char key[128], value[4096];
	const char *data;

	if (!s || !worldspawn_entity_string)
		return;

	r_worldspawn_sun_presence.dir_defined = false;
	r_worldspawn_sun_presence.color_defined = false;
	r_worldspawn_sun_presence.intensity_defined = false;

	data = COM_Parse (worldspawn_entity_string);
	if (!data || com_token[0] != '{')
		return;

	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return;
		if (com_token[0] == '}')
			break;
		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ')
			key[strlen (key) - 1] = 0;

		data = COM_ParseEx (data, CPE_ALLOWTRUNC);
		if (!data)
			return;
		q_strlcpy (value, com_token, sizeof (value));

		if (!strcmp ("sun_mangle", key))
		{
			vec3_t ang;
			if (sscanf (value, "%f %f %f", &ang[0], &ang[1], &ang[2]) == 3)
			{
				AngleVectors (ang, s->dir, NULL, NULL);
				r_worldspawn_sun_presence.dir_defined = true;
			}
			continue;
		}

		if (!strcmp ("sunlight", key) || !strcmp ("sunlight2", key))
		{
			char *endptr;
			float intensity = strtof (value, &endptr);
			while (endptr && *endptr == ' ')
				endptr++;
			if (endptr && endptr != value && *endptr == '\0')
			{
				s->intensity = intensity;
				r_worldspawn_sun_presence.intensity_defined = true;
			}
			continue;
		}

		if (!strcmp ("sunlight_color", key) || !strcmp ("sun_color", key) || !strcmp ("suncolour", key))
		{
			if (sscanf (value, "%f %f %f", &s->color[0], &s->color[1], &s->color[2]) == 3)
				r_worldspawn_sun_presence.color_defined = true;
			continue;
		}
	}
}

void R_Sun_Finalize (sun_t *s)
{
	if (!s)
		return;
	if (DotProduct (s->dir, s->dir) <= 1e-6f)
		R_SetDefaultSunDirection (s->dir);
	if (DotProduct (s->dir, s->dir) <= 1e-6f)
		VectorSet (s->dir, 0.f, 0.f, -1.f);
	else
		VectorNormalize (s->dir);

	s->color[0] = q_max (0.f, s->color[0]);
	s->color[1] = q_max (0.f, s->color[1]);
	s->color[2] = q_max (0.f, s->color[2]);
	s->intensity = q_max (0.f, s->intensity);
	s->volumetric_intensity = q_max (0.f, s->volumetric_intensity);
	s->anisotropy = CLAMP (-0.99f, s->anisotropy, 0.99f);
	s->shadow_bias = q_max (0.f, s->shadow_bias);
	s->shadow_strength = CLAMP (0.f, s->shadow_strength, 4.f);
	s->shadow_distance = q_max (0.f, s->shadow_distance);
	s->ray_decay = CLAMP (0.f, s->ray_decay, 1.f);
	s->ray_density = q_max (0.f, s->ray_density);
}

static qboolean R_Sun_IsRenderable (const sun_t *s)
{
	if (!s)
		return false;
	if (DotProduct (s->dir, s->dir) <= 1e-6f)
		return false;
	if (s->intensity <= 0.f)
		return false;
	return (s->color[0] > 0.f || s->color[1] > 0.f || s->color[2] > 0.f);
}

static qboolean R_Sun_HasExplicitWorldspawnKeys (void)
{
	return r_worldspawn_sun_presence.dir_defined
		|| r_worldspawn_sun_presence.color_defined
		|| r_worldspawn_sun_presence.intensity_defined;
}

const sun_t* R_GetSun (void)
{
	return &r_sun_state;
}

void R_SetSun (const sun_t *src)
{
	if (!src)
		return;
	r_sun_state = *src;
	R_Sun_Finalize (&r_sun_state);
}

qboolean R_WorldHasSun (void)
{
	return r_worldspawn_has_sun;
}

void R_Sun_UpdateFromWorld (void)
{
	sun_t sun;

	r_worldspawn_sun_presence.dir_defined = false;
	r_worldspawn_sun_presence.color_defined = false;
	r_worldspawn_sun_presence.intensity_defined = false;
	R_Sun_ResetToDefaults (&sun);
	if (cl.worldmodel)
		R_Sun_ApplyMapOverrides (&sun, cl.worldmodel->entities);
	R_Sun_Finalize (&sun);
	R_SetSun (&sun);

	/* Only explicit worldspawn sun keys should activate global sun lighting.
	 * Fallback defaults must not leak fake "sky light" into indoor maps. */
	r_worldspawn_has_sun =
		R_Sun_HasExplicitWorldspawnKeys () &&
		R_Sun_IsRenderable (&r_sun_state);
}

void R_NewGame (void)
{
	int i;

	//clear playertexture pointers (the textures themselves were freed by texmgr_newgame)
	for (i=0; i<MAX_SCOREBOARD; i++)
		playertextures[i] = NULL;
}

/*
=============
R_ParseWorldspawn

called at map load
=============
*/
static void R_ParseWorldspawn (void)
{
	char key[128], value[4096];
	const char *data;
	map_fallbackalpha = r_wateralpha.value;
	map_wateralpha = (cl.worldmodel->contentstransparent&SURF_DRAWWATER)?r_wateralpha.value:1;
	map_lavaalpha = 1.0f;
	map_telealpha = (cl.worldmodel->contentstransparent&SURF_DRAWTELE)?r_telealpha.value:1;
	map_slimealpha = (cl.worldmodel->contentstransparent&SURF_DRAWSLIME)?r_slimealpha.value:1;

	data = COM_Parse(cl.worldmodel->entities);
	if (!data)
		return; // error
	if (com_token[0] != '{')
		return; // error

	while (1)
	{
		data = COM_Parse(data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy(key, com_token + 1, sizeof(key));
		else
			q_strlcpy(key, com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_ParseEx(data, CPE_ALLOWTRUNC);
		if (!data)
			return; // error
		q_strlcpy(value, com_token, sizeof(value));

		if (!strcmp("wateralpha", key))
			map_wateralpha = atof(value);

		if (!strcmp("telealpha", key))
			map_telealpha = atof(value);

		if (!strcmp("slimealpha", key))
			map_slimealpha = atof(value);
	}


	map_fallbackalpha = CLAMP(0.f, map_fallbackalpha, 1.f);
	map_wateralpha = CLAMP(0.f, map_wateralpha, 1.f);
	map_lavaalpha = 1.0f;
	map_telealpha = CLAMP(0.f, map_telealpha, 1.f);
	map_slimealpha = CLAMP(0.f, map_slimealpha, 1.f);

}


/*
===============
R_NewMap
===============
*/
void R_NewMap (void)
{
	int		i;

	for (i=0 ; i<256 ; i++)
		d_lightstylevalue[i] = 264;		// normal light value

	R_ClearEfrags ();
	r_viewleaf = NULL;
	R_ClearParticles ();
	VEC_CLEAR (r_pointfile);

	R_ResetGodraysStabilization ();
	R_FogVol_ClearHistory ();
	R_ReloadDecals ();
	R_ClearDecals ();

	GL_BuildLightmaps ();
        GL_BuildBModelVertexBuffer ();
        GL_BuildBModelMarkBuffers ();
        //ericw -- no longer load alias models into a VBO here, it's done in Mod_LoadAliasModel

        r_framecount = 0; //johnfitz -- paranoid?
        r_visframecount = 0; //johnfitz -- paranoid?


        Sky_NewMap (); //johnfitz -- skybox in worldspawn
        Fog_NewMap (); //johnfitz -- global fog in worldspawn
        R_ParseWorldspawn (); //ericw -- wateralpha, telealpha, slimealpha in worldspawn
        R_Sun_UpdateFromWorld ();
        R_ParseDlightEntities (); // persistent dlights from BSP entities
        R_FogVol_ParseEntities (); // fog volume entities from BSP

	// Load pointfile if map has no vis data and either developer mode is on or the game was started from a map editing tool
	if (developer.value || map_checks.value)
		if (!cl.worldmodel->visdata && COM_FileExists (va ("maps/%s.pts", cl.mapname), NULL))
			Cbuf_AddText ("pointfile leak\n");
}

/*
====================
R_TimeRefresh_f

For program optimization
====================
*/
void R_TimeRefresh_f (void)
{
	int		i;
	double	start, stop, time;

	if (cls.state != ca_connected)
	{
		Con_Printf("Not connected to a server\n");
		return;
	}

	start = Sys_DoubleTime ();
	for (i = 0; i < 128; i++)
	{
		GL_BeginRendering(&glx, &gly, &glwidth, &glheight);
		r_refdef.viewangles[1] = i*(360.0/128.0);
		R_RenderView ();
		GL_EndRendering ();
	}

	glFinish ();
	stop = Sys_DoubleTime ();
	time = stop-start;
	Con_Printf ("%lf seconds (%.1lf fps)\n", time, 128/time);
}

static GLuint current_array_buffer;
static GLuint current_element_array_buffer;
static GLuint current_shader_storage_buffer;
static GLuint current_draw_indirect_buffer;

/*
====================
GL_CreateBuffer
====================
*/
GLuint GL_CreateBuffer (GLenum target, GLenum usage, const char *name, size_t size, const void *data)
{
	GLuint buffer;
	GL_GenBuffersFunc (1, &buffer);
	GL_BindBuffer (target, buffer);
	if (name)
		GL_ObjectLabelFunc (GL_BUFFER, buffer, -1, name);
	GL_BufferDataFunc (target, size, data, usage);
	return buffer;
}

/*
====================
GL_BindBuffer

glBindBuffer wrapper
====================
*/
void GL_BindBuffer (GLenum target, GLuint buffer)
{
	GLuint *cache;

	switch (target)
	{
		case GL_ARRAY_BUFFER:
			cache = &current_array_buffer;
			break;
		case GL_ELEMENT_ARRAY_BUFFER:
			cache = &current_element_array_buffer;
			break;
		case GL_SHADER_STORAGE_BUFFER:
			cache = &current_shader_storage_buffer;
			break;
		case GL_DRAW_INDIRECT_BUFFER:
			cache = &current_draw_indirect_buffer;
			break;
		default:
			goto apply;
	}

	if (*cache != buffer)
	{
		*cache = buffer;
	apply:
		GL_BindBufferFunc (target, buffer);
	}
}

typedef struct {
	GLuint		buffer;
	GLintptr	offset;
	GLsizeiptr	size;
} bufferrange_t;

#define CACHED_BUFFER_RANGES 8

static bufferrange_t ssbo_ranges[CACHED_BUFFER_RANGES];

/*
====================
GL_BindBufferRange

glBindBufferRange wrapper
====================
*/
void GL_BindBufferRange (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	if (target == GL_SHADER_STORAGE_BUFFER)
	{
		if (index < CACHED_BUFFER_RANGES)
		{
			bufferrange_t *range = &ssbo_ranges[index];
			if (range->buffer == buffer && range->offset == offset && range->size == size)
				return;
			range->buffer = buffer;
			range->offset = offset;
			range->size   = size;
		}
		current_shader_storage_buffer = buffer;
	}

	GL_BindBufferRangeFunc (target, index, buffer, offset, size);
}

/*
====================
GL_BindBuffersRange

glBindBuffersRange wrapper with fallback
if ARB_multi_bind is not present
====================
*/
void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count, const GLuint *buffers, const GLintptr *offsets, const GLsizeiptr *sizes)
{
	GLsizei i;
	if (gl_multi_bind_able)
	{
		if (target == GL_SHADER_STORAGE_BUFFER)
		{
			for (i = 0; i < count && first + i < countof (ssbo_ranges); i++)
			{
				bufferrange_t *range = &ssbo_ranges[first + i];
				range->buffer = buffers[i];
				range->offset = offsets[i];
				range->size   = sizes[i];
			}
		}
		GL_BindBuffersRangeFunc (target, first, count, buffers, offsets, sizes);
	}
	else
	{
		for (i = 0; i < count; i++)
			GL_BindBufferRange (target, first + i, buffers[i], offsets[i], sizes[i]);
	}
}

/*
====================
GL_DeleteBuffer
====================
*/
void GL_DeleteBuffer (GLuint buffer)
{
	int i;

	if (buffer == current_array_buffer)
		current_array_buffer = 0;
	if (buffer == current_element_array_buffer)
		current_element_array_buffer = 0;
	if (buffer == current_draw_indirect_buffer)
		current_draw_indirect_buffer = 0;
	if (buffer == current_shader_storage_buffer)
		current_shader_storage_buffer = 0;

	for (i = 0; i < countof(ssbo_ranges); i++)
		if (ssbo_ranges[i].buffer == buffer)
			ssbo_ranges[i].buffer = 0;

	GL_DeleteBuffersFunc (1, &buffer);
}

/*
====================
GL_ClearBufferBindings

This must be called if you do anything that could make the cached bindings
invalid (e.g. manually binding, destroying the context).
====================
*/
void GL_ClearBufferBindings (void)
{
	int i;

	current_array_buffer = 0;
	current_element_array_buffer = 0;
	current_draw_indirect_buffer = 0;
	current_shader_storage_buffer = 0;

	for (i = 0; i < countof(ssbo_ranges); i++)
		ssbo_ranges[i].buffer = 0;

	GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
	GL_BindBufferFunc (GL_ELEMENT_ARRAY_BUFFER, 0);
	GL_BindBufferFunc (GL_DRAW_INDIRECT_BUFFER, 0);
	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, 0);
}

/*
============================================================================
								FRAME RESOURCES
============================================================================
*/

#define FRAMES_IN_FLIGHT 3

typedef enum
{
	FRAMERES_HOST_BUFFER_BIT	= 1 << 0,
	FRAMERES_DEVICE_BUFFER_BIT	= 1 << 1,

	FRAMERES_ALL_BITS			= FRAMERES_HOST_BUFFER_BIT | FRAMERES_DEVICE_BUFFER_BIT
} frameres_bits_t;

typedef struct frameres_t
{
	GLsync			fence;
	GLuint			device_buffer;
	GLuint			host_buffer;
	GLubyte			*host_ptr;
	GLuint			*garbage;
} frameres_t;

static frameres_t	frameres[FRAMES_IN_FLIGHT];
static int			frameres_idx = 0;
static size_t		frameres_host_offset = 0;
static size_t		frameres_device_offset = 0;
static size_t		frameres_host_buffer_size = 1 * 1024 * 1024;
static size_t		frameres_device_buffer_size = 1 * 1024 * 1024;

/*
====================
GL_AddGarbageBuffer
====================
*/
void GL_AddGarbageBuffer (GLuint handle)
{
	VEC_PUSH (frameres[frameres_idx].garbage, handle);
}

/*
====================
GL_AllocFrameResources
====================
*/
static void GL_AllocFrameResources (frameres_bits_t bits)
{
	int i;
	for (i = 0; i < countof (frameres); i++)
	{
		char name[64];
		frameres_t *frame = &frameres[i];

		if (bits & FRAMERES_HOST_BUFFER_BIT)
		{
			GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

			if (frame->host_buffer)
			{
				if (frame->host_ptr)
				{
					GL_BindBuffer (GL_ARRAY_BUFFER, frame->host_buffer);
					GL_UnmapBufferFunc (GL_ARRAY_BUFFER);
				}
				GL_AddGarbageBuffer (frame->host_buffer);
			}

			GL_GenBuffersFunc (1, &frame->host_buffer);
			GL_BindBuffer (GL_ARRAY_BUFFER, frame->host_buffer);
			q_snprintf (name, sizeof (name), "dynamic host buffer %d", i);
			GL_ObjectLabelFunc (GL_BUFFER, frame->host_buffer, -1, name);
			if (gl_buffer_storage_able)
			{
				GL_BufferStorageFunc (GL_ARRAY_BUFFER, frameres_host_buffer_size, NULL, flags);
				frame->host_ptr = GL_MapBufferRangeFunc (GL_ARRAY_BUFFER, 0, frameres_host_buffer_size, flags);
				if (!frame->host_ptr)
					Sys_Error ("GL_AllocFrameResources: MapBufferRange failed on %" SDL_PRIu64 " bytes", (uint64_t)frameres_host_buffer_size);
			}
			else
			{
				GL_BufferDataFunc (GL_ARRAY_BUFFER, frameres_host_buffer_size, NULL, GL_STREAM_DRAW);
			}
		}

		if (bits & FRAMERES_DEVICE_BUFFER_BIT)
		{
			if (frame->device_buffer)
				GL_AddGarbageBuffer (frame->device_buffer);

			GL_GenBuffersFunc (1, &frame->device_buffer);
			GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, frame->device_buffer);
			q_snprintf (name, sizeof (name), "dynamic device buffer %d", i);
			GL_ObjectLabelFunc (GL_BUFFER, frame->device_buffer, -1, name);
			GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, frameres_device_buffer_size, NULL, GL_STREAM_DRAW);
		}
	}

	if (bits & FRAMERES_HOST_BUFFER_BIT)
		frameres_host_offset = 0;
	if (bits & FRAMERES_DEVICE_BUFFER_BIT)
		frameres_device_offset = 0;
}

/*
====================
GL_CreateFrameResources
====================
*/
void GL_CreateFrameResources (void)
{
	GL_AllocFrameResources (FRAMERES_ALL_BITS);
}

/*
====================
GL_DeleteFrameResources
====================
*/
void GL_DeleteFrameResources (void)
{
	size_t i, j, num_garbage_bufs;

	glFinish ();

	for (i = 0; i < countof (frameres); i++)
	{
		frameres_t *frame = &frameres[i];

		if (frame->fence)
		{
			GL_DeleteSyncFunc (frame->fence);
			frame->fence = NULL;
		}

		for (j = 0, num_garbage_bufs = VEC_SIZE (frame->garbage); j < num_garbage_bufs; j++)
			GL_DeleteBuffer (frame->garbage[j]);
		VEC_CLEAR (frame->garbage);

		if (frame->host_ptr)
		{
			GL_BindBuffer (GL_ARRAY_BUFFER, frame->host_buffer);
			GL_UnmapBufferFunc (GL_ARRAY_BUFFER);
			frame->host_ptr = NULL;
		}

		if (frame->host_buffer)
		{
			GL_DeleteBuffer (frame->host_buffer);
			frame->host_buffer = 0;
		}

		if (frame->device_buffer)
		{
			GL_DeleteBuffer (frame->device_buffer);
			frame->device_buffer = 0;
		}
	}
}

/*
====================
GL_AcquireFrameResources
====================
*/
void GL_AcquireFrameResources (void)
{
	frameres_t *prev_frame = &frameres[(frameres_idx + FRAMES_IN_FLIGHT - 1) % FRAMES_IN_FLIGHT];
	frameres_t *frame = &frameres[frameres_idx];
	size_t i, num_garbage_bufs;

	if (prev_frame->fence)
		GL_WaitSyncFunc (prev_frame->fence, 0, GL_TIMEOUT_IGNORED);

	if (frame->fence)
	{
		GLuint64 timeout = 1ull * 1000 * 1000 * 1000; // 1 second
		GLenum result = GL_ClientWaitSyncFunc (frame->fence, GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
		if (result == GL_TIMEOUT_EXPIRED)
			glFinish ();
		else if (result == GL_WAIT_FAILED)
			Sys_Error ("GL_AcquireFrameResources: wait failed (0x%04X)", glGetError ());
		else if (result != GL_CONDITION_SATISFIED && result != GL_ALREADY_SIGNALED)
			Sys_Error ("GL_AcquireFrameResources: sync failed (0x%04X)", result);
		GL_DeleteSyncFunc (frame->fence);
		frame->fence = NULL;
	}

	num_garbage_bufs = VEC_SIZE (frame->garbage);
	for (i = 0; i < num_garbage_bufs; i++)
		GL_DeleteBuffer (frame->garbage[i]);
	VEC_CLEAR (frame->garbage);
}

/*
====================
GL_ReleaseFrameResources
====================
*/
void GL_ReleaseFrameResources (void)
{
	frameres_t *frame = &frameres[frameres_idx];

	SDL_assert (!frame->fence);
	frame->fence = GL_FenceSyncFunc (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

	if (!frame->fence)
		Sys_Error ("glFenceSync failed (error code 0x%04X)", glGetError ());

	dev_stats.gpu_upload = frameres_host_offset;
	dev_peakstats.gpu_upload = q_max (dev_peakstats.gpu_upload, dev_stats.gpu_upload);

	if (++frameres_idx == countof (frameres))
		frameres_idx = 0;

	frameres_host_offset = 0;
	frameres_device_offset = 0;
}

/*
====================
GL_Upload
====================
*/
void GL_Upload (GLenum target, const void *data, size_t numbytes, GLuint *outbuf, GLbyte **outofs)
{
	size_t align;
	frameres_t *frame;

	align = (target == GL_UNIFORM_BUFFER) ? ubo_align : ssbo_align;
	frameres_host_offset = (frameres_host_offset + align) & ~align;

	if (frameres_host_offset + numbytes > frameres_host_buffer_size)
	{
		frameres_host_buffer_size = frameres_host_offset + ((numbytes + align) & ~align);
		frameres_host_buffer_size += frameres_host_buffer_size >> 1;
		GL_AllocFrameResources (FRAMERES_HOST_BUFFER_BIT);
	}

	frame = &frameres[frameres_idx];
	if (frame->host_ptr)
		memcpy (frame->host_ptr + frameres_host_offset, data, numbytes);
	else
	{
		GL_BindBuffer (target, frame->host_buffer);
		GL_BufferSubDataFunc (target, frameres_host_offset, numbytes, data);
	}

	*outbuf = frame->host_buffer;
	*outofs = (GLbyte*) frameres_host_offset;

	frameres_host_offset += numbytes;
}

/*
====================
GL_ReserveDeviceMemory
====================
*/
void GL_ReserveDeviceMemory (GLenum target, size_t numbytes, GLuint *outbuf, size_t *outofs)
{
	size_t align;
	frameres_t *frame;

	align = (target == GL_UNIFORM_BUFFER) ? ubo_align : ssbo_align;
	frameres_device_offset = (frameres_device_offset + align) & ~align;

	if (frameres_device_offset + numbytes > frameres_device_buffer_size)
	{
		frameres_device_buffer_size = frameres_device_offset + ((numbytes + align) & ~align);
		frameres_device_buffer_size += frameres_device_buffer_size >> 1;
		GL_AllocFrameResources (FRAMERES_DEVICE_BUFFER_BIT);
	}

	frame = &frameres[frameres_idx];

	*outbuf = frame->device_buffer;
	*outofs = frameres_device_offset;

	frameres_device_offset += numbytes;
}
