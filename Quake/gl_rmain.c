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
// r_main.c

#include "quakedef.h"
#include "cl_postfx.h"
#include "r_postfx.h"
#include "r_framegraph.h"
#include "r_fogvol.h"
#include "r_godrays.h"
#include "r_skyvis.h"
#include "r_ssao.h"
#include "r_tonemap.h"
#include "r_quality.h"
#include "r_realtimelight.h"
#include "gl_dlight.h"
#include "gl_oit.h"
#include "gl_shadow.h"
#include "gl_shadow_runtime.h"
#include "gl_lightgrid.h"
#include "mat_material.h"
#include <float.h>
#include <math.h>

#define NOISESCALE     (1.0f / 127.0f)

#ifndef GL_NEGATIVE_ONE_TO_ONE
#define GL_NEGATIVE_ONE_TO_ONE 0x935E
#endif

#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif

extern gltexture_t *lightmap_dir_texture;
extern cvar_t r_sun_light;
extern cvar_t r_sun_visibility;

qboolean	r_cache_thrash;		// compatability

gpuframedata_t r_framedata;

static int r_fogvol_update_called = 0;
static int r_fogvol_draw_called = 0;

/* BUG FIX #1 (SSAO/Fog): GL_GenerateSSAOTexture runs inside GL_PostProcess, after
 * the 3D scene pass in R_RenderView.
 * Fog_DisableGFog clears r_framedata.fogdata[3] (density) to 0 so 2D overlays stay
 * fog-free. At SSAO generation time the UBO therefore has density=0, making
 * FogTransmittanceFromViewPos always return 1.0 â†’ ssao_fog_strength has zero
 * effect â†’ SSAO darkens far fogged/purple areas with full strength instead of
 * fading to AO=1.0.
 * Fix: save fog params just before Fog_DisableGFog and pass them explicitly to
 * ssao.frag via uniform 14 .yzw (previously unused, only .x = max_distance). */
/* Cached global fogvol parameters for SSAO/postprocess fog damping. */
static r_ssao_fog_state_t r_ssao_fog_state;
static qboolean r_ssao_invalid_warned = false;

float r_autoexposure_debug_exposure = 1.f;
float r_autoexposure_debug_luminance = 0.f;

vec3_t* r_pointfile;

int			r_visframecount;	// bumped when going to a new PVS
int			r_framecount;		// used for dlight push checking

static entity_t* cl_sorted_visedicts[MAX_VISEDICTS + 1];
static int cl_modtype_ofs[mod_numtypes * 2 + 1];
static entity_t* cl_shadow_visedicts[MAX_VISEDICTS];
static int cl_numshadowedicts = 0;
static vec3_t	frustum_absnormal[4];
mplane_t	frustum[4];
float		r_matview[16];
float		r_matproj[16];
static float r_matinvproj[16];
float		r_matviewproj[16];
static float r_prev_matviewproj[16] = {
		1.f, 0.f, 0.f, 0.f,
		0.f, 1.f, 0.f, 0.f,
		0.f, 0.f, 1.f, 0.f,
		0.f, 0.f, 0.f, 1.f
};
static vec3_t r_prev_vieworg = { 0.f, 0.f, 0.f };
static double r_prev_frame_time = 0.0;
static qboolean r_prev_frame_valid = false;
static qboolean r_frame_rendered_this_update;

static godrays_stabilization_t r_godrays_stabilization;
static int r_godrays_generated_frame = -1;
static GLuint r_godrays_cached_shafts = 0;
static GLuint r_godrays_cached_mask = 0;
static GLuint r_godrays_cached_source = 0;
static qboolean r_godrays_cached_debug_source_generated = false;


static GLuint r_godrays_coupling_shafts_tex = 0;
static int r_godrays_coupling_shafts_w = 0;
static int r_godrays_coupling_shafts_h = 0;

qboolean R_Godrays_GetFogCouplingSource (GLuint *out_shafts_tex, int *out_width, int *out_height)
{
	if (out_shafts_tex)
		*out_shafts_tex = r_godrays_coupling_shafts_tex;
	if (out_width)
		*out_width = r_godrays_coupling_shafts_w;
	if (out_height)
		*out_height = r_godrays_coupling_shafts_h;

	return r_godrays_coupling_shafts_tex != 0;
}

static void R_GetFramePlanDecisions (qboolean *out_needs_scene_effects, qboolean *out_needs_postprocess);

static const float r_identity_mat4[16] = {
		1.f, 0.f, 0.f, 0.f,
		0.f, 1.f, 0.f, 0.f,
		0.f, 0.f, 1.f, 0.f,
		0.f, 0.f, 0.f, 1.f
};




static void GL_LogErrorIfDeveloper (const char *label)
{
	GLenum err = glGetError ();
	if (err != GL_NO_ERROR)
		Con_DPrintf ("GL error after %s: 0x%04X\n", label, err);
}


// Returns how much of the console is currently covering the screen in the range [0, 1].
static float GL_ConsoleVisibility (void)
{
	if (scr_con_current <= 0.f)
		return 0.f;

	float height = (float)glheight;
	if (height <= 0.f)
		return 1.f;

	return CLAMP (0.f, scr_con_current / height, 1.f);
}

static void R_DebugDRSNativeEffects (qboolean bloom_enabled, qboolean ssao_enabled, qboolean fogvol_enabled,
	qboolean godrays_enabled, qboolean motion_enabled)
{
	static int last_logged_scale = 0;
	static int last_logged_mask = 0;
	int scene_scale;
	int mask = 0;

	if (r_drs.value <= 0.f || r_drs_debug.value <= 0.f)
	{
		last_logged_scale = 0;
		last_logged_mask = 0;
		return;
	}

	scene_scale = R_GetSceneRenderScale ();
	if (scene_scale <= 1)
	{
		last_logged_scale = 0;
		last_logged_mask = 0;
		return;
	}

	if (bloom_enabled)
		mask |= 1 << 0;
	if (ssao_enabled)
		mask |= 1 << 1;
	if (fogvol_enabled)
		mask |= 1 << 2;
	if (godrays_enabled)
		mask |= 1 << 3;
	if (motion_enabled)
		mask |= 1 << 4;

	if (mask == 0)
		return;

	if (last_logged_scale == scene_scale && last_logged_mask == mask)
		return;

	last_logged_scale = scene_scale;
	last_logged_mask = mask;

	Con_DPrintf ("drs note: scene=1/%d with native-domain postfx bloom=%d ssao=%d fogvol=%d godrays=%d motion=%d\n",
		scene_scale,
		bloom_enabled ? 1 : 0,
		ssao_enabled ? 1 : 0,
		fogvol_enabled ? 1 : 0,
		godrays_enabled ? 1 : 0,
		motion_enabled ? 1 : 0);
}


static qboolean R_IsUnderwaterContents (int contents)
{
	return contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA;
}

static int R_ResolveUnderwaterContents (int view_contents, qboolean forced, const vec3_t vieworg)
{
	vec3_t probe;
	mleaf_t *leaf;
	int i;

	if (R_IsUnderwaterContents (view_contents) || !forced || !cl.worldmodel)
		return view_contents;

	VectorCopy (vieworg, probe);
	for (i = 0; i < 32; ++i)
	{
		probe[2] -= 8.f;
		leaf = Mod_PointInLeaf (probe, cl.worldmodel);
		if (!leaf)
			break;
		if (R_IsUnderwaterContents (leaf->contents))
			return leaf->contents;
		if (leaf->contents == CONTENTS_SOLID)
			break;
	}

	return view_contents;
}


//johnfitz -- rendering statistics
int rs_brushpolys, rs_aliaspolys, rs_skypolys;
int rs_dynamiclightmaps, rs_brushpasses, rs_aliaspasses, rs_skypasses;

//
// view origin
//
vec3_t	vup;
vec3_t	vpn;
vec3_t	vright;
vec3_t	r_origin;

float r_fovx, r_fovy; //johnfitz -- rendering fov may be different becuase of r_waterwarp and r_stereo
qboolean water_warp;

extern byte* SV_FatPVS (vec3_t org, qmodel_t* worldmodel);
extern qboolean SV_EdictInPVS (edict_t* test, byte* pvs);
extern qboolean SV_BoxInPVS (vec3_t mins, vec3_t maxs, byte* pvs, mnode_t* node);

//
// screen size info
//
refdef_t	r_refdef;

mleaf_t* r_viewleaf, * r_oldviewleaf;

int		d_lightstylevalue[256];	// 8.8 fraction of base light value

static qboolean gl_framebuffer_srgb_enabled = false;
static qboolean gl_srgb_capability_warned = false;


cvar_t	r_norefresh = { "r_norefresh","0",CVAR_NONE };
cvar_t	r_drawentities = { "r_drawentities","1",CVAR_NONE };
cvar_t	r_drawviewmodel = { "r_drawviewmodel","1",CVAR_NONE };
cvar_t	r_speeds = { "r_speeds","0",CVAR_NONE };
cvar_t	r_pos = { "r_pos","0",CVAR_NONE };
cvar_t	r_fullbright = { "r_fullbright","0",CVAR_NONE };
cvar_t	r_lightmap = { "r_lightmap","0",CVAR_NONE };
cvar_t	r_lightmap_linear = { "r_lightmap_linear", "1", CVAR_ARCHIVE };
cvar_t	r_lightmap_mipmaps = { "r_lightmap_mipmaps", "1", CVAR_ARCHIVE };
cvar_t	r_lightmap16f = { "r_lightmap16f", "1", CVAR_ARCHIVE };
cvar_t	r_lightingdir = { "r_lightingdir", "0", CVAR_ARCHIVE };
cvar_t	r_rgblighting_enable = { "r_rgblighting_enable", "1", CVAR_ARCHIVE };
cvar_t	r_srgb_textures = { "r_srgb_textures", "1", CVAR_ARCHIVE };
cvar_t	r_srgb_framebuffer = { "r_srgb_framebuffer", "1", CVAR_ARCHIVE };
cvar_t	r_debug_colorspace = { "r_debug_colorspace", "0", CVAR_ARCHIVE };
cvar_t	r_lighting_debug_view = { "r_lighting_debug_view", "0", CVAR_ARCHIVE };
cvar_t	r_color_midtone = { "r_color_midtone", "1.0", CVAR_ARCHIVE };
cvar_t	r_color_contrast = { "r_color_contrast", "1.0", CVAR_ARCHIVE };
cvar_t	r_color_saturation = { "r_color_saturation", "1.05", CVAR_ARCHIVE };
cvar_t	r_lightmap_colorspace = { "r_lightmap_colorspace", "srgb", CVAR_ARCHIVE };
cvar_t	r_lightmap_colorspace_debug = { "r_lightmap_colorspace_debug", "0", CVAR_ARCHIVE };
cvar_t	r_wateralpha = { "r_wateralpha","1",CVAR_ARCHIVE };
cvar_t	r_litwater = { "r_litwater","1",CVAR_NONE };
cvar_t	r_dynamic = { "r_dynamic","1",CVAR_ARCHIVE };
cvar_t  r_gl_state_validate = { "r_gl_state_validate", "0", CVAR_NONE };
cvar_t  r_framegraph_autobind = { "r_framegraph_autobind", "0", CVAR_NONE };
cvar_t  r_framegraph_debug = { "r_framegraph_debug", "0", CVAR_NONE };
cvar_t	r_dlight_entities = { "r_dlight_entities", "1", CVAR_ARCHIVE };
cvar_t	r_quality = { "r_quality", "high", CVAR_ARCHIVE };
cvar_t  r_shadow = { "r_shadow", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_sun = { "r_shadow_sun", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_dlight = { "r_shadow_dlight", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_dlight_max = { "r_shadow_dlight_max", "4", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_size = { "r_shadow_sun_size", "2048", CVAR_ARCHIVE };
cvar_t  r_shadow_dlight_size = { "r_shadow_dlight_size", "512", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_distance = { "r_shadow_sun_distance", "1200", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_bias = { "r_shadow_sun_bias", "0.0015", CVAR_ARCHIVE };
cvar_t  r_shadow_dlight_bias = { "r_shadow_dlight_bias", "0.02", CVAR_ARCHIVE };
cvar_t  r_shadow_receiver_bias = { "r_shadow_receiver_bias", "2.0", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_pcf = { "r_shadow_sun_pcf", "1.5", CVAR_ARCHIVE };
cvar_t  r_shadow_dlight_pcf = { "r_shadow_dlight_pcf", "0.75", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_snap = { "r_shadow_sun_snap", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_cascades = { "r_shadow_sun_cascades", "3", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_split1 = { "r_shadow_sun_split1", "0.12", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_split2 = { "r_shadow_sun_split2", "0.35", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_split3 = { "r_shadow_sun_split3", "0.70", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_split_mode = { "r_shadow_sun_split_mode", "0", CVAR_ARCHIVE };
cvar_t  r_shadow_sun_split_lambda = { "r_shadow_sun_split_lambda", "0.65", CVAR_ARCHIVE };
cvar_t  r_shadow_mark_mode = { "r_shadow_mark_mode", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_profile = { "r_shadow_profile", "0", CVAR_ARCHIVE };
cvar_t  r_shadow_cull_vis = { "r_shadow_cull_vis", "0", CVAR_ARCHIVE };
cvar_t  r_shadow_cull_backface = { "r_shadow_cull_backface", "0", CVAR_ARCHIVE };
cvar_t  r_shadow_cull_frustum = { "r_shadow_cull_frustum", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_cull_sphere = { "r_shadow_cull_sphere", "1", CVAR_ARCHIVE };
cvar_t  r_shadow_debug = { "r_shadow_debug", "0", CVAR_NONE };
cvar_t  r_shadow_log = { "r_shadow_log", "0", CVAR_ARCHIVE };
cvar_t  r_rimlight = { "r_rimlight", "1", CVAR_ARCHIVE };
cvar_t  r_rimlight_world = { "r_rimlight_world", "1", CVAR_ARCHIVE };
cvar_t  r_rimlight_models = { "r_rimlight_models", "1", CVAR_ARCHIVE };
cvar_t  r_rimlight_intensity = { "r_rimlight_intensity", "0.48", CVAR_ARCHIVE };
cvar_t  r_rimlight_power = { "r_rimlight_power", "3.2", CVAR_ARCHIVE };
cvar_t  r_rimlight_sun = { "r_rimlight_sun", "1.0", CVAR_ARCHIVE };
cvar_t  r_rimlight_dlight = { "r_rimlight_dlight", "1.0", CVAR_ARCHIVE };
cvar_t  r_rimlight_shadow = { "r_rimlight_shadow", "1", CVAR_ARCHIVE };
cvar_t	r_novis = { "r_novis","0",CVAR_ARCHIVE };
#if defined(USE_SIMD)
cvar_t	r_simd = { "r_simd","1",CVAR_ARCHIVE };
#endif
cvar_t	r_alphasort = { "r_alphasort","1",CVAR_ARCHIVE };
cvar_t	r_oit = { "r_oit","1",CVAR_ARCHIVE };
cvar_t	r_dither = { "r_dither", "1.0", CVAR_ARCHIVE };
cvar_t	r_dof = { "r_dof", "1", CVAR_ARCHIVE };
cvar_t	r_dof_focus = { "r_dof_focus", "64", CVAR_ARCHIVE };
cvar_t	r_dof_range = { "r_dof_range", "255", CVAR_ARCHIVE };
cvar_t	r_dof_strength = { "r_dof_strength", "3", CVAR_ARCHIVE };
cvar_t	r_dof_autofocus = { "r_dof_autofocus", "1", CVAR_ARCHIVE };

cvar_t	r_motionblur = { "r_motionblur", "0", CVAR_ARCHIVE };
cvar_t	r_motionblur_shutter = { "r_motionblur_shutter", "0.75", CVAR_ARCHIVE };
cvar_t	r_motionblur_maxradiuspixels = { "r_motionblur_maxradiuspixels", "32", CVAR_ARCHIVE };
cvar_t	r_motionblur_maxsamples = { "r_motionblur_maxsamples", "16", CVAR_ARCHIVE };
cvar_t	r_motionblur_minvelocity = { "r_motionblur_minvelocity", "0.0", CVAR_ARCHIVE };
cvar_t	r_motionblur_depththreshold = { "r_motionblur_depththreshold", "0.1", CVAR_ARCHIVE };

cvar_t	r_tonemap = { "r_tonemap", "2", CVAR_ARCHIVE };
cvar_t	r_tonemap_exposure = { "r_tonemap_exposure", "1.0", CVAR_ARCHIVE };
cvar_t	r_autoexposure = { "r_autoexposure", "1", CVAR_ARCHIVE };
cvar_t	r_autoexposure_async = { "r_autoexposure_async", "1", CVAR_ARCHIVE };
cvar_t	r_ae_min_scene_luma = { "r_ae_min_scene_luma", "0.001", CVAR_ARCHIVE };
cvar_t	r_ae_min_exposure = { "r_ae_min_exposure", "0.25", CVAR_ARCHIVE };
cvar_t	r_ae_max_exposure = { "r_ae_max_exposure", "8.0", CVAR_ARCHIVE };
cvar_t	r_exposure_bias = { "r_exposure_bias", "1.0", CVAR_ARCHIVE };
cvar_t	r_exposure_min = { "r_exposure_min", "0.85", CVAR_ARCHIVE };
cvar_t	r_exposure_max = { "r_exposure_max", "1.15", CVAR_ARCHIVE };
cvar_t	r_exposure_speed_up = { "r_exposure_speed_up", "0.6", CVAR_ARCHIVE };
cvar_t	r_exposure_speed_down = { "r_exposure_speed_down", "0.3", CVAR_ARCHIVE };
cvar_t	r_exposure_lock = { "r_exposure_lock", "0", CVAR_ARCHIVE };
cvar_t	r_exposure_debug = { "r_exposure_debug", "0", CVAR_NONE };


cvar_t	r_bloom = { "r_bloom", "3.00", CVAR_ARCHIVE };
cvar_t	r_bloom_threshold = { "r_bloom_threshold", "1.0", CVAR_ARCHIVE };

cvar_t	r_postfx = { "r_postfx", "1", CVAR_ARCHIVE };
cvar_t	r_polyblend_legacy = { "r_polyblend_legacy", "0", CVAR_ARCHIVE };
cvar_t	r_postfx_pickup = { "r_postfx_pickup", "1", CVAR_ARCHIVE };
cvar_t	r_postfx_pickup_exposure = { "r_postfx_pickup_exposure", "0.4", CVAR_ARCHIVE };
cvar_t	r_postfx_pickup_bloom = { "r_postfx_pickup_bloom", "0.6", CVAR_ARCHIVE };
cvar_t	r_postfx_pickup_duration = { "r_postfx_pickup_duration", "0.35", CVAR_ARCHIVE };
cvar_t	r_postfx_damage = { "r_postfx_damage", "1", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_vignette = { "r_postfx_damage_vignette", "0.45", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_vignette_softness = { "r_postfx_damage_vignette_softness", "0.6", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_desat = { "r_postfx_damage_desat", "0.35", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_exposure = { "r_postfx_damage_exposure", "-0.35", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_duration = { "r_postfx_damage_duration", "0.6", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_accum_window = { "r_postfx_damage_accum_window", "0.1", CVAR_ARCHIVE };
cvar_t	r_postfx_damage_accum_scale = { "r_postfx_damage_accum_scale", "0.5", CVAR_ARCHIVE };
/*
Damage double-vision post effect.
- r_post_damage_doublevision: master toggle for the effect.
- r_post_damage_dv_strength: overall intensity (scaled by trauma).
- r_post_damage_dv_px: max offset in pixels at trauma=1.
- r_post_damage_dv_freq: oscillation frequency in Hz.
- r_post_damage_trauma_scale: damage-to-trauma multiplier.
- r_post_damage_trauma_decay: exponential decay rate per second.
- r_post_damage_dv_quality: 0=off, 1=two ghosts, 2=three ghosts + mild smear.
- r_post_damage_dv_debug: show trauma intensity as a grayscale output.
*/
cvar_t	r_post_damage_doublevision = { "r_post_damage_doublevision", "1", CVAR_ARCHIVE };
cvar_t	r_post_damage_dv_strength = { "r_post_damage_dv_strength", "0.9", CVAR_ARCHIVE };
cvar_t	r_post_damage_dv_px = { "r_post_damage_dv_px", "2.0", CVAR_ARCHIVE };
cvar_t	r_post_damage_dv_freq = { "r_post_damage_dv_freq", "12.0", CVAR_ARCHIVE };
cvar_t	r_post_damage_trauma_scale = { "r_post_damage_trauma_scale", "0.02", CVAR_ARCHIVE };
cvar_t	r_post_damage_trauma_decay = { "r_post_damage_trauma_decay", "6.0", CVAR_ARCHIVE };
cvar_t	r_post_damage_dv_quality = { "r_post_damage_dv_quality", "1", CVAR_ARCHIVE };
cvar_t	r_post_damage_dv_debug = { "r_post_damage_dv_debug", "0", CVAR_NONE };
cvar_t	r_postfx_powerup = { "r_postfx_powerup", "1", CVAR_ARCHIVE };
cvar_t	r_postfx_powerup_lut_strength = { "r_postfx_powerup_lut_strength", "0.6", CVAR_ARCHIVE };
cvar_t	r_postfx_powerup_ramp_in = { "r_postfx_powerup_ramp_in", "0.2", CVAR_ARCHIVE };
cvar_t	r_postfx_powerup_ramp_out = { "r_postfx_powerup_ramp_out", "0.3", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater = { "r_postfx_underwater", "1", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_grade_strength = { "r_postfx_underwater_grade_strength", "0.5", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_strength = { "r_postfx_underwater_fog_strength", "0.4", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_ramp_in = { "r_postfx_underwater_ramp_in", "0.2", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_ramp_out = { "r_postfx_underwater_ramp_out", "0.2", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_water_r = { "r_postfx_underwater_fog_water_r", "0.2", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_water_g = { "r_postfx_underwater_fog_water_g", "0.35", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_water_b = { "r_postfx_underwater_fog_water_b", "0.5", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_slime_r = { "r_postfx_underwater_fog_slime_r", "0.1", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_slime_g = { "r_postfx_underwater_fog_slime_g", "0.25", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_slime_b = { "r_postfx_underwater_fog_slime_b", "0.1", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_lava_r = { "r_postfx_underwater_fog_lava_r", "0.6", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_lava_g = { "r_postfx_underwater_fog_lava_g", "0.2", CVAR_ARCHIVE };
cvar_t	r_postfx_underwater_fog_lava_b = { "r_postfx_underwater_fog_lava_b", "0.05", CVAR_ARCHIVE };
cvar_t	r_postfx_quad = { "r_postfx_quad", "1", CVAR_ARCHIVE };
cvar_t	r_postfx_quad_emissive_boost = { "r_postfx_quad_emissive_boost", "0.5", CVAR_ARCHIVE };
cvar_t	r_postfx_quad_bloom_boost = { "r_postfx_quad_bloom_boost", "0.4", CVAR_ARCHIVE };
cvar_t	r_postfx_quad_pulse_speed = { "r_postfx_quad_pulse_speed", "2.0", CVAR_ARCHIVE };
cvar_t	r_postfx_quad_pulse_intensity = { "r_postfx_quad_pulse_intensity", "0.1", CVAR_ARCHIVE };
cvar_t	r_postfx_bloom_mode = { "r_postfx_bloom_mode", "0", CVAR_ARCHIVE };
cvar_t	r_postfx_lut = { "r_postfx_lut", "1", CVAR_ARCHIVE };
cvar_t	r_postfx_lut_strength_powerup = { "r_postfx_lut_strength_powerup", "0.6", CVAR_ARCHIVE };
cvar_t	r_postfx_lut_strength_underwater = { "r_postfx_lut_strength_underwater", "0.5", CVAR_ARCHIVE };
cvar_t	r_postfx_lut_debug_id = { "r_postfx_lut_debug_id", "0", CVAR_NONE };
cvar_t	r_postfx_debug = { "r_postfx_debug", "0", CVAR_NONE };

cvar_t	r_ssao = { "r_ssao", "1", CVAR_ARCHIVE };
cvar_t	r_ssao_radius = { "r_ssao_radius", "24", CVAR_ARCHIVE };
cvar_t	r_ssao_intensity = { "r_ssao_intensity", "1.5", CVAR_ARCHIVE };
cvar_t	r_ssao_bias = { "r_ssao_bias", "0.02", CVAR_ARCHIVE };
cvar_t	r_ssao_power = { "r_ssao_power", "1.5", CVAR_ARCHIVE };
cvar_t	r_ssao_min = { "r_ssao_min", "0.55", CVAR_ARCHIVE };
cvar_t	r_ssao_samples = { "r_ssao_samples", "12", CVAR_ARCHIVE };
cvar_t	r_ssao_blur = { "r_ssao_blur", "1", CVAR_ARCHIVE };
cvar_t	r_ssao_blur_radius = { "r_ssao_blur_radius", "2", CVAR_ARCHIVE };
cvar_t	r_ssao_blur_sigma = { "r_ssao_blur_sigma", "2.0", CVAR_ARCHIVE };
cvar_t	r_ssao_blur_bilateral = { "r_ssao_blur_bilateral", "1", CVAR_ARCHIVE };
cvar_t	r_ssao_halfres = { "r_ssao_halfres", "1", CVAR_ARCHIVE };
// r_ssao_debug modes: 0 off, 1 raw AO, 2 AO*fog, 3 fog factor, 4 depth raw, 5 view-space Z, 6 view-space position, 7 normals, 8 noise, 9 sample hit ratio, 10 AO raw, 11 blur debug, 12 AO mask, 13 fog transmittance+source, 14 fog-damped AO.
cvar_t	r_ssao_debug = { "r_ssao_debug", "0", CVAR_ARCHIVE };
cvar_t	r_ssao_debug_far = { "r_ssao_debug_far", "4096", CVAR_ARCHIVE };
cvar_t	r_ssao_reversedz_mode = { "r_ssao_reversedz_mode", "0", CVAR_ARCHIVE };
cvar_t	r_ssao_noise = { "r_ssao_noise", "1", CVAR_ARCHIVE };
cvar_t	r_ssao_noise_mode = { "r_ssao_noise_mode", "1", CVAR_ARCHIVE };
cvar_t	r_ssao_noise_scale = { "r_ssao_noise_scale", "1.0", CVAR_ARCHIVE };
cvar_t	r_ssao_normalsource = { "r_ssao_normalsource", "0", CVAR_ARCHIVE };
cvar_t	r_ssao_freeze_noise = { "r_ssao_freeze_noise", "0", CVAR_ARCHIVE };
cvar_t	r_ssao_force_fullres = { "r_ssao_force_fullres", "0", CVAR_ARCHIVE };
cvar_t	r_ssao_format = { "r_ssao_format", "1", CVAR_ARCHIVE };
cvar_t	r_ssao_upscale_nearest = { "r_ssao_upscale_nearest", "0", CVAR_ARCHIVE };
cvar_t	r_ssao_fog_strength = { "r_ssao_fog_strength", "1.0", CVAR_ARCHIVE };
cvar_t	r_ssao_fog_power = { "r_ssao_fog_power", "1.0", CVAR_ARCHIVE };
cvar_t	r_ssao_max_distance = { "r_ssao_max_distance", "1024", CVAR_ARCHIVE };
cvar_t	r_ssao_validate = { "r_ssao_validate", "0", CVAR_ARCHIVE };

cvar_t	r_godrays = { "r_godrays", "0", CVAR_ARCHIVE };
cvar_t	r_godrays_sky_threshold = { "r_godrays_sky_threshold", "0.05", CVAR_ARCHIVE };
cvar_t	r_godrays_sky_intensity = { "r_godrays_sky_intensity", "1.0", CVAR_ARCHIVE };
cvar_t	r_godrays_sky_tint = { "r_godrays_sky_tint", "1 1 1", CVAR_ARCHIVE };
cvar_t	r_godrays_emissive_intensity = { "r_godrays_emissive_intensity", "1.0", CVAR_ARCHIVE };
cvar_t	r_godrays_lighttex_intensity = { "r_godrays_lighttex_intensity", "1.0", CVAR_ARCHIVE };
cvar_t	r_godrays_emissive_threshold = { "r_godrays_emissive_threshold", "0.4", CVAR_ARCHIVE };
cvar_t	r_godrays_light_threshold = { "r_godrays_light_threshold", "0.6", CVAR_ARCHIVE };
cvar_t	r_godrays_mask_knee = { "r_godrays_mask_knee", "0.0", CVAR_ARCHIVE };
cvar_t	r_godrays_blur = { "r_godrays_blur", "1.5", CVAR_ARCHIVE };
/* When enabled, light-texture godray stages are emitted only if the texture/stage/material
 * name contains a light token (light, lamp, glow, flare, neon, torch, lantern). */
cvar_t	r_godrays_lighttex_name_match = { "r_godrays_lighttex_name_match", "1", CVAR_ARCHIVE };
cvar_t	r_godrays_samples = { "r_godrays_samples", "48", CVAR_ARCHIVE };
cvar_t	r_godrays_density = { "r_godrays_density", "0.9", CVAR_ARCHIVE };
cvar_t	r_godrays_weight = { "r_godrays_weight", "0.015", CVAR_ARCHIVE };
cvar_t	r_godrays_decay = { "r_godrays_decay", "0.97", CVAR_ARCHIVE };
cvar_t	r_godrays_exposure = { "r_godrays_exposure", "1.0", CVAR_ARCHIVE };
cvar_t	r_godrays_threshold = { "r_godrays_threshold", "0.0", CVAR_ARCHIVE };
cvar_t	r_godrays_debug = { "r_godrays_debug", "0", CVAR_ARCHIVE };
cvar_t	r_godrays_debug_source = { "r_godrays_debug_source", "0", CVAR_ARCHIVE };
cvar_t	r_godrays_vol_pow = { "r_godrays_vol_pow", "1.0", CVAR_ARCHIVE };

cvar_t	r_vignette = { "r_vignette", "0.15", CVAR_ARCHIVE };
cvar_t	r_vignette_radius_inner = { "r_vignette_radius_inner", "0.8", CVAR_ARCHIVE };
cvar_t	r_vignette_radius_outer = { "r_vignette_radius_outer", "2.0", CVAR_ARCHIVE };
cvar_t	r_vignette_falloff = { "r_vignette_falloff", "2.0", CVAR_ARCHIVE };
cvar_t	r_vignette_color_r = { "r_vignette_color_r", "0.0", CVAR_ARCHIVE };
cvar_t	r_vignette_color_g = { "r_vignette_color_g", "0.0", CVAR_ARCHIVE };
cvar_t	r_vignette_color_b = { "r_vignette_color_b", "0.0", CVAR_ARCHIVE };
cvar_t	r_vignette_blend_mode = { "r_vignette_blend_mode", "0", CVAR_ARCHIVE };
cvar_t	r_vignette_noise = { "r_vignette_noise", "0.0", CVAR_ARCHIVE };
cvar_t	r_teleportfx = { "r_teleportfx", "1", CVAR_ARCHIVE };
cvar_t	r_teleportfx_time = { "r_teleportfx_time", "0.35", CVAR_ARCHIVE };

cvar_t	r_overbrightbits = { "r_overbrightbits", "2", CVAR_ARCHIVE };

cvar_t	gl_finish = { "gl_finish","0",CVAR_NONE };
cvar_t	gl_clear = { "gl_clear","1",CVAR_NONE };
cvar_t	gl_polyblend = { "gl_polyblend","1",CVAR_NONE };
cvar_t	gl_playermip = { "gl_playermip","0",CVAR_NONE };
cvar_t	gl_nocolors = { "gl_nocolors","0",CVAR_NONE };

//johnfitz -- new cvars
cvar_t	r_clearcolor = { "r_clearcolor","2",CVAR_ARCHIVE };
cvar_t	r_flatlightstyles = { "r_flatlightstyles", "0", CVAR_NONE };
cvar_t	r_lerplightstyles = { "r_lerplightstyles", "1", CVAR_ARCHIVE }; // 0=off; 1=skip abrupt transitions; 2=always lerp
cvar_t	gl_fullbrights = { "gl_fullbrights", "1", CVAR_ARCHIVE };
cvar_t	gl_farclip = { "gl_farclip", "65536", CVAR_ARCHIVE };
cvar_t	gl_overbright_models = { "gl_overbright_models", "0", CVAR_ARCHIVE };
cvar_t	r_viewmodel_light_boost = { "r_viewmodel_light_boost", "1.5", CVAR_ARCHIVE };
cvar_t	r_viewmodel_minlight = { "r_viewmodel_minlight", "72", CVAR_ARCHIVE };
cvar_t	r_model_halflambert = { "r_model_halflambert", "0", CVAR_ARCHIVE };
cvar_t	r_facenormals_enable = { "r_facenormals_enable", "1", CVAR_ARCHIVE };
cvar_t	r_oldskyleaf = { "r_oldskyleaf", "0", CVAR_NONE };
cvar_t	r_drawworld = { "r_drawworld", "1", CVAR_NONE };
cvar_t	r_showtris = { "r_showtris", "0", CVAR_NONE };
cvar_t	r_showbboxes = { "r_showbboxes", "0", CVAR_NONE };
cvar_t	r_showbboxes_think = { "r_showbboxes_think", "0", CVAR_NONE }; // 0=show all; 1=thinkers only; -1=non-thinkers only
cvar_t	r_showbboxes_health = { "r_showbboxes_health", "0", CVAR_NONE }; // 0=show all; 1=healthy only; -1=non-healthy only
cvar_t	r_showbboxes_links = { "r_showbboxes_links", "3", CVAR_NONE }; // 0=off; 1=outgoing only; 2=incoming only; 3=incoming+outgoing
cvar_t	r_showbboxes_targets = { "r_showbboxes_targets", "1", CVAR_NONE };
cvar_t	r_showfields = { "r_showfields", "0", CVAR_NONE };
cvar_t	r_showfields_align = { "r_showfields_align", "1", CVAR_ARCHIVE }; // 0=entity pos; 1=bottom-right
cvar_t	r_lerpmodels = { "r_lerpmodels", "1", CVAR_ARCHIVE };
cvar_t	r_lerpmove = { "r_lerpmove", "1", CVAR_ARCHIVE };
cvar_t	r_nolerp_list = { "r_nolerp_list", "progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,progs/flame_pyre.mdl,progs/v_saw.mdl,progs/v_xfist.mdl,progs/h2stuff/newfire.mdl", CVAR_NONE };
cvar_t	r_noshadow_list = { "r_noshadow_list", "progs/missile.mdl,progs/grenade.mdl,progs/spike.mdl,progs/s_spike.mdl,progs/bolt.mdl,progs/bolt2.mdl,progs/bolt3.mdl,progs/beam.mdl,progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,progs/flame_pyre.mdl,progs/h2stuff/newfire.mdl", CVAR_NONE };

extern cvar_t	r_vfog;
extern cvar_t	vid_fsaa;
//johnfitz
extern cvar_t	r_softemu_dither_screen;
extern cvar_t	r_softemu_dither_texture;

cvar_t	gl_zfix = { "gl_zfix", "1", CVAR_ARCHIVE }; // QuakeSpasm z-fighting fix

cvar_t	r_telealpha = { "r_telealpha","0",CVAR_NONE };
cvar_t	r_slimealpha = { "r_slimealpha","0",CVAR_NONE };

float	map_wateralpha, map_lavaalpha, map_telealpha, map_slimealpha;
float	map_fallbackalpha;

qboolean r_fullbright_cheatsafe, r_lightmap_cheatsafe, r_drawworld_cheatsafe; //johnfitz

cvar_t	r_scale = { "r_scale", "1", CVAR_ARCHIVE };
cvar_t	r_scene_scale_debug = { "r_scene_scale_debug", "0", CVAR_NONE };
cvar_t	r_drs = { "r_drs", "0", CVAR_ARCHIVE };
cvar_t	r_drs_use_gpu = { "r_drs_use_gpu", "1", CVAR_ARCHIVE };
cvar_t	r_drs_target_ms = { "r_drs_target_ms", "16.6", CVAR_ARCHIVE };
cvar_t	r_drs_target_fps = { "r_drs_target_fps", "0", CVAR_ARCHIVE };
cvar_t	r_drs_min_scale = { "r_drs_min_scale", "1", CVAR_ARCHIVE };
cvar_t	r_drs_max_scale = { "r_drs_max_scale", "4", CVAR_ARCHIVE };
cvar_t	r_drs_step_up = { "r_drs_step_up", "1", CVAR_ARCHIVE };
cvar_t	r_drs_step_down = { "r_drs_step_down", "1", CVAR_ARCHIVE };
cvar_t	r_drs_hysteresis_ms = { "r_drs_hysteresis_ms", "0.5", CVAR_ARCHIVE };
cvar_t	r_drs_filter_alpha = { "r_drs_filter_alpha", "0.25", CVAR_ARCHIVE };
cvar_t	r_drs_debug = { "r_drs_debug", "0", CVAR_NONE };

static float view_znear;
static float view_zfar;

typedef struct render_scene_size_state_s {
	int native_width;
	int native_height;
	int scene_width;
	int scene_height;
	int scene_scale;
	int prev_scene_width;
	int prev_scene_height;
	qboolean initialized;
} render_scene_size_state_t;

static render_scene_size_state_t r_scene_size_state;

typedef struct render_drs_state_s {
	int dynamic_scale;
	float filtered_ms;
	float last_raw_ms;
	qboolean initialized;
	qboolean last_gpu_valid;
	int upscale_cooldown;
} render_drs_state_t;

static render_drs_state_t r_drs_state;

static int R_GetDRSMinScale (void)
{
	int max_supported = q_max (1, vid.maxscale);
	return CLAMP (1, (int)Q_rint (r_drs_min_scale.value), max_supported);
}

static int R_GetDRSMaxScale (int min_scale)
{
	int max_supported = q_max (1, vid.maxscale);
	return CLAMP (min_scale, (int)Q_rint (r_drs_max_scale.value), max_supported);
}

static int R_ClampSceneScaleSupported (int scale)
{
	int max_supported = q_max (1, vid.maxscale);
	return CLAMP (1, scale, max_supported);
}

static int R_ClampDRSScale (int scale)
{
	int min_scale = q_max (1, R_GetDRSMinScale ());
	int max_scale = q_max (min_scale, R_GetDRSMaxScale (min_scale));
	return CLAMP (min_scale, scale, max_scale);
}

static void R_UpdateDynamicResolutionScale (void)
{
	double gpu_ms = 0.0;
	double cpu_ms = 0.0;
	qboolean gpu_valid = false;
	double raw_ms = 0.0;
	float target_ms;
	float alpha;
	float hysteresis_ms;
	int min_scale;
	int max_scale;
	int step_up;
	int step_down;
	int current_scale;
	int new_scale;
	int debug_level;
	qboolean use_gpu = (r_drs_use_gpu.value > 0.f);
	qboolean use_gpu_timing = false;

	if (r_drs.value <= 0.f)
	{
		memset (&r_drs_state, 0, sizeof (r_drs_state));
		return;
	}

	R_FrameGraph_GetTimingSummary (&gpu_ms, &cpu_ms, &gpu_valid);
	use_gpu_timing = (use_gpu && gpu_valid && gpu_ms > 0.0);
	raw_ms = use_gpu_timing ? gpu_ms : ((host_frametime > 0.0) ? host_frametime * 1000.0 : cpu_ms);
	if (raw_ms <= 0.0)
		raw_ms = 0.001;

	target_ms = (r_drs_target_ms.value > 0.f)
		? r_drs_target_ms.value
		: ((r_drs_target_fps.value > 1.f) ? (1000.f / r_drs_target_fps.value) : (1000.f / 60.f));
	target_ms = CLAMP (1.f, target_ms, 1000.f);

	alpha = CLAMP (0.01f, r_drs_filter_alpha.value, 1.f);
	hysteresis_ms = q_max (0.f, r_drs_hysteresis_ms.value);
	min_scale = R_GetDRSMinScale ();
	max_scale = R_GetDRSMaxScale (min_scale);
	step_up = CLAMP (1, (int)Q_rint (r_drs_step_up.value), 8);
	step_down = CLAMP (1, (int)Q_rint (r_drs_step_down.value), 8);
	debug_level = (int)Q_rint (r_drs_debug.value);

	if (!r_drs_state.initialized)
	{
		r_drs_state.dynamic_scale = R_ClampDRSScale (q_max (1, r_refdef.scale));
		r_drs_state.filtered_ms = (float)raw_ms;
		r_drs_state.last_raw_ms = (float)raw_ms;
		r_drs_state.last_gpu_valid = use_gpu_timing;
		r_drs_state.initialized = true;
		r_drs_state.upscale_cooldown = 0;
		if (debug_level > 0)
			Con_DPrintf ("drs init: scale=%d raw_ms=%.2f source=%s target=%.2f\n",
				r_drs_state.dynamic_scale, (float)raw_ms, use_gpu_timing ? "gpu" : "frame", target_ms);
		return;
	}

	r_drs_state.filtered_ms = r_drs_state.filtered_ms * (1.f - alpha) + (float)raw_ms * alpha;
	r_drs_state.last_raw_ms = (float)raw_ms;
	r_drs_state.last_gpu_valid = use_gpu_timing;
	current_scale = R_ClampDRSScale (r_drs_state.dynamic_scale > 0 ? r_drs_state.dynamic_scale : q_max (1, r_refdef.scale));
	new_scale = current_scale;

	if (r_drs_state.filtered_ms > target_ms + hysteresis_ms)
	{
		new_scale = q_min (max_scale, current_scale + step_down);
		r_drs_state.upscale_cooldown = 8;
	}
	else if (r_drs_state.filtered_ms < target_ms - hysteresis_ms)
	{
		if (r_drs_state.upscale_cooldown > 0)
			r_drs_state.upscale_cooldown--;
		else
		{
			new_scale = q_max (min_scale, current_scale - step_up);
			r_drs_state.upscale_cooldown = 2;
		}
	}
	else if (r_drs_state.upscale_cooldown > 0)
	{
		r_drs_state.upscale_cooldown--;
	}

	if (new_scale != current_scale && debug_level > 0)
	{
		Con_DPrintf ("drs scale: %d -> %d raw_ms=%.2f filtered_ms=%.2f target_ms=%.2f source=%s\n",
			current_scale, new_scale, (float)raw_ms, r_drs_state.filtered_ms, target_ms,
			use_gpu_timing ? "gpu" : "frame");
	}
	else if (debug_level > 1)
	{
		Con_DPrintf ("drs sample: scale=%d raw_ms=%.2f filtered_ms=%.2f target_ms=%.2f source=%s\n",
			current_scale, (float)raw_ms, r_drs_state.filtered_ms, target_ms,
			use_gpu_timing ? "gpu" : "frame");
	}

	r_drs_state.dynamic_scale = new_scale;
}

static void R_UpdateSceneSizeState (void)
{
	int debug = (int)Q_rint (r_scene_scale_debug.value);
	int requested_scale = q_max (1, r_refdef.scale);
	int native_width = q_max (1, vid.width);
	int native_height = q_max (1, vid.height);
	int base_scene_width = q_max (1, r_refdef.vrect.width);
	int base_scene_height = q_max (1, r_refdef.vrect.height);
	if (r_drs.value > 0.f && r_drs_state.dynamic_scale > 0)
		requested_scale = r_drs_state.dynamic_scale;
	requested_scale = R_ClampSceneScaleSupported (q_max (1, requested_scale));
	if (r_drs.value > 0.f)
		requested_scale = R_ClampDRSScale (requested_scale);

	int scene_width = q_max (1, base_scene_width / requested_scale);
	int scene_height = q_max (1, base_scene_height / requested_scale);
	qboolean size_changed;
	qboolean scale_changed;

	if (!r_scene_size_state.initialized)
	{
		r_scene_size_state.prev_scene_width = scene_width;
		r_scene_size_state.prev_scene_height = scene_height;
	}

	size_changed = !r_scene_size_state.initialized
		|| r_scene_size_state.scene_width != scene_width
		|| r_scene_size_state.scene_height != scene_height
		|| r_scene_size_state.native_width != native_width
		|| r_scene_size_state.native_height != native_height;
	scale_changed = !r_scene_size_state.initialized
		|| r_scene_size_state.scene_scale != requested_scale;

	if (size_changed || scale_changed)
	{
		r_scene_size_state.prev_scene_width = r_scene_size_state.initialized ? r_scene_size_state.scene_width : scene_width;
		r_scene_size_state.prev_scene_height = r_scene_size_state.initialized ? r_scene_size_state.scene_height : scene_height;
	}

	r_scene_size_state.native_width = native_width;
	r_scene_size_state.native_height = native_height;
	r_scene_size_state.scene_width = scene_width;
	r_scene_size_state.scene_height = scene_height;
	r_scene_size_state.scene_scale = requested_scale;
	r_scene_size_state.initialized = true;

	if (debug > 0 && (debug > 1 || size_changed || scale_changed))
	{
		Con_DPrintf ("scene_size: output=%dx%d view=%dx%d scale=%d scene=%dx%d prev=%dx%d\n",
			r_scene_size_state.native_width, r_scene_size_state.native_height,
			base_scene_width, base_scene_height,
			r_scene_size_state.scene_scale,
			r_scene_size_state.scene_width, r_scene_size_state.scene_height,
			r_scene_size_state.prev_scene_width, r_scene_size_state.prev_scene_height);
	}
}

int R_GetNativeRenderWidth (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.native_width;
}

int R_GetNativeRenderHeight (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.native_height;
}

int R_GetSceneRenderWidth (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_width;
}

int R_GetSceneRenderHeight (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_height;
}

int R_GetSceneRenderScale (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_scale;
}

float R_GetViewZNear (void)
{
	return (view_znear > 0.f) ? view_znear : 0.5f;
}

float R_GetViewZFar (void)
{
	float znear = R_GetViewZNear ();
	if (view_zfar > znear)
		return view_zfar;
	return znear + 1.f;
}

static qboolean R_DoFEnabled (void)
{
	return r_dof.value > 0.f && r_dof_strength.value > 0.f;
}

static qboolean r_dof_autofocus_initialized = false;
static float r_dof_autofocus_value = 0.f;

static float R_GetDynamicDoFFocus (float fallback)
{
	trace_t trace = {0};
	vec3_t end;
	float range;
	float target;
	qboolean traced = false;
	const hull_t *world_hull;

	if (r_dof_autofocus.value <= 0.f)
	{
		r_dof_autofocus_initialized = false;
		return fallback;
	}

	if (cls.state != ca_connected || !cl.worldmodel)
	{
		r_dof_autofocus_initialized = false;
		return fallback;
	}

	world_hull = &cl.worldmodel->hulls[0];
	if (!world_hull->clipnodes || !world_hull->planes)
	{
		r_dof_autofocus_initialized = false;
		return fallback;
	}

	range = view_zfar > 0.f ? view_zfar : gl_farclip.value;
	if (range <= 0.f)
		range = 8192.f;

	VectorMA (r_origin, range, vpn, end);

	if (sv.active)
	{
		extern edict_t* sv_player;
		qcvm_t* oldvm = qcvm;

		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (&sv.qcvm);

		trace = SV_Move (r_origin, vec3_origin, vec3_origin, end, MOVE_NORMAL, sv_player);
		traced = true;

		PR_SwitchQCVM (oldvm);
	}

	if (!traced)
	{
		memset (&trace, 0, sizeof (trace));
		trace.fraction = 1.f;
		VectorCopy (end, trace.endpos);

		SV_RecursiveHullCheck (world_hull, 0, 0.f, 1.f, r_origin, end, &trace);
	}

	if (trace.allsolid || trace.startsolid || trace.fraction <= 0.f)
		target = fallback;
	else
		target = q_max (trace.fraction * range, 0.f);

	if (!r_dof_autofocus_initialized)
	{
		r_dof_autofocus_value = target;
		r_dof_autofocus_initialized = true;
	}
	else
	{
		float lerp = (float)host_frametime * 8.f;
		if (lerp < 0.f)
			lerp = 0.f;
		else if (lerp > 1.f)
			lerp = 1.f;
		r_dof_autofocus_value += (target - r_dof_autofocus_value) * lerp;
	}

	return r_dof_autofocus_value;
}

static void ExtractFrustumPlane (float mvp[16], int axis, float ndcval, qboolean flip, mplane_t* out);


//==============================================================================
//
// FRAMEBUFFERS
//
//==============================================================================

glframebufs_t framebufs;

#define SSAO_MAX_SAMPLES 32
// SSAO FIX: Use a larger noise tile to reduce visible tiling in half-res AO.
#define SSAO_NOISE_SIZE 64

static GLuint GL_CreateSSAONoiseTexture (void)
{
	GLuint texnum = 0;
	unsigned char noise[SSAO_NOISE_SIZE * SSAO_NOISE_SIZE * 2];

	for (int i = 0; i < SSAO_NOISE_SIZE * SSAO_NOISE_SIZE; ++i)
	{
		float angle = (float)rand () / (float)RAND_MAX;
		angle *= (float)(2.0 * M_PI);
		float x = cosf (angle) * 0.5f + 0.5f;
		float y = sinf (angle) * 0.5f + 0.5f;
		noise[i * 2 + 0] = (unsigned char)CLAMP (0.f, x * 255.f, 255.f);
		noise[i * 2 + 1] = (unsigned char)CLAMP (0.f, y * 255.f, 255.f);
	}

	glGenTextures (1, &texnum);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, texnum);
	GL_ObjectLabelFunc (GL_TEXTURE, texnum, -1, "ssao noise");
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, GL_RG8, SSAO_NOISE_SIZE, SSAO_NOISE_SIZE);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, SSAO_NOISE_SIZE, SSAO_NOISE_SIZE, GL_RG, GL_UNSIGNED_BYTE, noise);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	GL_LogErrorIfDeveloper ("GL_CreateSSAONoiseTexture");

	return texnum;
}

/*
=============
GL_CreateFBOAttachment
=============
*/
static GLuint GL_CreateFBOAttachment (GLenum format, int width, int height, int samples, GLenum filter, const char* name)
{
	GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
	GLuint texnum;
	qboolean is_depth_format = (format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8 || format == GL_DEPTH_COMPONENT24 || format == GL_DEPTH_COMPONENT32F);
	width = q_max (1, width);
	height = q_max (1, height);

	glGenTextures (1, &texnum);
	GL_BindNative (GL_TEXTURE0, target, texnum);
	GL_ObjectLabelFunc (GL_TEXTURE, texnum, -1, name);
	if (samples > 1)
	{
		GL_TexStorage2DMultisampleFunc (target, samples, format, width, height, GL_FALSE);
	}
	else
	{
		GL_TexStorage2DFunc (target, 1, format, width, height);
		glTexParameteri (target, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri (target, GL_TEXTURE_MIN_FILTER, filter);
		if (is_depth_format)
			glTexParameteri (target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	}
	glTexParameteri (target, GL_TEXTURE_MAX_LEVEL, 0);
	GL_LogErrorIfDeveloper ("GL_CreateFBOAttachment");

	return texnum;
}

/*
=============
GL_CreateFBO
=============
*/

static GLuint GL_CreateTexture2D (GLenum format, int width, int height, GLenum filter, const char* name)
{
	GLuint texnum;

	glGenTextures (1, &texnum);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, texnum);
	GL_ObjectLabelFunc (GL_TEXTURE, texnum, -1, name);
	GL_TexStorage2DFunc (GL_TEXTURE_2D, 1, format, width, height);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	GL_LogErrorIfDeveloper ("GL_CreateTexture2D");

	return texnum;
}

static GLuint GL_CreateFBO (GLenum target, const GLuint* colors, int numcolors, GLuint depth, GLuint stencil, const char* name)
{
	GLenum status;
	GLuint fbo;
	GLenum buffers[8];
	int i;

	if (numcolors > (int)countof (buffers))
		Sys_Error ("GL_CreateFBO: too many color buffers (%d)", numcolors);

	GL_GenFramebuffersFunc (1, &fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, fbo);
	GL_ObjectLabelFunc (GL_FRAMEBUFFER, fbo, -1, name);
	GL_LogErrorIfDeveloper ("GL_CreateFBO bind");

	for (i = 0; i < numcolors; i++)
	{
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, target, colors[i], 0);
		buffers[i] = GL_COLOR_ATTACHMENT0 + i;
	}
	GL_LogErrorIfDeveloper ("GL_CreateFBO color attachments");
	GL_DrawBuffersFunc (numcolors, buffers);

	if (depth)
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, depth, 0);
	if (stencil)
		GL_FramebufferTexture2DFunc (GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, target, stencil, 0);
	GL_LogErrorIfDeveloper ("GL_CreateFBO depth/stencil attachments");

	status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		Sys_Error ("Failed to create %s (status code 0x%X)", name, status);

	return fbo;
}

/*
=============
GL_CreateSimpleFBO
=============
*/
static GLuint GL_CreateSimpleFBO (GLenum target, GLuint colors, GLuint depth, GLuint stencil, const char* name)
{
	return GL_CreateFBO (target, colors ? &colors : NULL, colors ? 1 : 0, depth, stencil, name);
}

static qboolean GL_ValidateSimpleFramebuffer (GLuint fbo, const char* name)
{
	if (fbo == 0)
		return false;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, fbo);
	GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf ("%s incomplete (0x%X)\n", name, status);
		return false;
	}

	return true;
}

static qboolean GL_AutoExposurePBOAvailable (void);
static void GL_AutoExposureDeletePBOs (void);
static void GL_AutoExposureInitPBOs (void);

/*
=============
GL_CreateFrameBuffers
=============
*/
void GL_CreateFrameBuffers (void)
{
	GLenum color_format = GL_RGBA16F;
	GLenum depth_format = GL_DEPTH24_STENCIL8;
	int native_w = R_GetNativeRenderWidth ();
	int native_h = R_GetNativeRenderHeight ();
	int scene_w = R_GetSceneRenderWidth ();
	int scene_h = R_GetSceneRenderHeight ();

	framebufs.ssao.valid = false;
	r_ssao_invalid_warned = false;

	/* query MSAA limits */
	glGetIntegerv (GL_MAX_COLOR_TEXTURE_SAMPLES, &framebufs.max_color_tex_samples);
	glGetIntegerv (GL_MAX_DEPTH_TEXTURE_SAMPLES, &framebufs.max_depth_tex_samples);
	framebufs.max_samples = q_min (framebufs.max_color_tex_samples, framebufs.max_depth_tex_samples);

	/* main framebuffer (color + depth + stencil) */
	framebufs.composite.color_tex = GL_CreateFBOAttachment (color_format, native_w, native_h, 1, GL_NEAREST, "composite colors");
	framebufs.composite.depth_stencil_tex = GL_CreateFBOAttachment (depth_format, native_w, native_h, 1, GL_NEAREST, "composite depth/stencil");
	framebufs.composite.fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D,
		framebufs.composite.color_tex,
		framebufs.composite.depth_stencil_tex,
		framebufs.composite.depth_stencil_tex,
		"composite fbo"
	);
	R_Shadow_CreateFrameBuffers ();
	framebufs.fogvol.width = native_w;
	framebufs.fogvol.height = native_h;
	if (r_fogvol_halfres.value > 0.f)
	{
		framebufs.fogvol.width = q_max (1, native_w / 2);
		framebufs.fogvol.height = q_max (1, native_h / 2);
	}
	for (int i = 0; i < 2; ++i)
	{
		const char *suffix = (i == 0) ? "fogvol color 0" : "fogvol color 1";
		const char *fbo_suffix = (i == 0) ? "fogvol fbo 0" : "fogvol fbo 1";
		framebufs.fogvol.color_tex[i] = GL_CreateTexture2D (GL_RGBA16F, framebufs.fogvol.width,
			framebufs.fogvol.height, GL_NEAREST, suffix);
		framebufs.fogvol.fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.fogvol.color_tex[i], 0, 0, fbo_suffix);
	}
	for (int i = 0; i < 2; ++i)
	{
		const char *suffix = (i == 0) ? "fogvol history 0" : "fogvol history 1";
		const char *fbo_suffix = (i == 0) ? "fogvol history fbo 0" : "fogvol history fbo 1";
		const char *composite_suffix = (i == 0) ? "fogvol composite 0" : "fogvol composite 1";
		const char *composite_fbo_suffix = (i == 0) ? "fogvol composite fbo 0" : "fogvol composite fbo 1";
		framebufs.fogvol.history_tex[i] = GL_CreateTexture2D (GL_RGBA16F, framebufs.fogvol.width,
			framebufs.fogvol.height, GL_NEAREST, suffix);
		framebufs.fogvol.history_fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.fogvol.history_tex[i], 0, 0, fbo_suffix);
		framebufs.fogvol.composite_tex[i] = GL_CreateTexture2D (GL_RGBA16F, framebufs.fogvol.width,
			framebufs.fogvol.height, GL_NEAREST, composite_suffix);
		framebufs.fogvol.composite_fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.fogvol.composite_tex[i], 0, 0, composite_fbo_suffix);
	}
	/* finalcopy stores the upscaled fog result, so it must match native output size. */
	framebufs.fogvol.finalcopy_tex = GL_CreateTexture2D (GL_RGBA16F, native_w,
		native_h, GL_NEAREST, "fogvol finalcopy");
	framebufs.fogvol.finalcopy_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.fogvol.finalcopy_tex, 0, 0, "fogvol finalcopy fbo");

	framebufs.autoexposure.width = 16;
	framebufs.autoexposure.height = 16;
	framebufs.autoexposure.tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.autoexposure.width, framebufs.autoexposure.height,
		GL_LINEAR, "autoexposure downscale");
	framebufs.autoexposure.fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.autoexposure.tex, 0, 0, "autoexposure fbo");
	GL_AutoExposureInitPBOs ();

	framebufs.bloom.width = q_max (1, native_w / 2);
	framebufs.bloom.height = q_max (1, native_h / 2);
	framebufs.bloom.extract_tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.bloom.width, framebufs.bloom.height, GL_LINEAR, "bloom extract");
	framebufs.bloom.pingpong_tex[0] = GL_CreateTexture2D (GL_RGBA16F, framebufs.bloom.width, framebufs.bloom.height, GL_LINEAR, "bloom blur 0");
	framebufs.bloom.pingpong_tex[1] = GL_CreateTexture2D (GL_RGBA16F, framebufs.bloom.width, framebufs.bloom.height, GL_LINEAR, "bloom blur 1");
	framebufs.bloom.extract_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.bloom.extract_tex, 0, 0, "bloom extract fbo");
	framebufs.bloom.pingpong_fbo[0] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.bloom.pingpong_tex[0], 0, 0, "bloom blur fbo 0");
	framebufs.bloom.pingpong_fbo[1] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.bloom.pingpong_tex[1], 0, 0, "bloom blur fbo 1");

	framebufs.godrays.width = q_max (1, native_w / 2);
	framebufs.godrays.height = q_max (1, native_h / 2);
	framebufs.godrays.source_tex = GL_CreateTexture2D (GL_RGBA16F, native_w, native_h, GL_LINEAR, "godrays source");
	framebufs.godrays.mask_tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.godrays.width, framebufs.godrays.height, GL_LINEAR, "godrays mask");
	framebufs.godrays.shafts_tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.godrays.width, framebufs.godrays.height, GL_LINEAR, "godrays shafts");
	framebufs.godrays.source_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.godrays.source_tex,
		framebufs.composite.depth_stencil_tex,
		framebufs.composite.depth_stencil_tex,
		"godrays source fbo");
	framebufs.godrays.mask_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.godrays.mask_tex, 0, 0, "godrays mask fbo");
	framebufs.godrays.shafts_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.godrays.shafts_tex, 0, 0, "godrays shafts fbo");

	framebufs.ssao.width[0] = scene_w;
	framebufs.ssao.height[0] = scene_h;
	framebufs.ssao.width[1] = q_max (1, scene_w / 2);
	framebufs.ssao.height[1] = q_max (1, scene_h / 2);
	framebufs.ssao.noise_tex = GL_CreateSSAONoiseTexture ();
	// SSAO FIX: Prefer higher precision AO targets to avoid R8 banding in dark areas.
	GLenum ssao_format = (Q_rint (r_ssao_format.value) > 0) ? GL_R16F : GL_R8;
	for (int i = 0; i < 2; ++i)
	{
		int width = framebufs.ssao.width[i];
		int height = framebufs.ssao.height[i];
		const char *suffix = (i == 0) ? "full" : "half";
		framebufs.ssao.ao_tex[i] = GL_CreateTexture2D (ssao_format, width, height, GL_NEAREST, va ("ssao %s", suffix));
		framebufs.ssao.blur_tex[i] = GL_CreateTexture2D (ssao_format, width, height, GL_NEAREST, va ("ssao blur %s", suffix));
		framebufs.ssao.ao_fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.ssao.ao_tex[i], 0, 0, va ("ssao fbo %s", suffix));
		framebufs.ssao.blur_fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.ssao.blur_tex[i], 0, 0, va ("ssao blur fbo %s", suffix));
	}

	framebufs.ssao.valid = (framebufs.ssao.noise_tex != 0);
	for (int i = 0; i < 2 && framebufs.ssao.valid; ++i)
	{
		framebufs.ssao.valid = GL_ValidateSimpleFramebuffer (framebufs.ssao.ao_fbo[i], va ("ssao fbo %d", i));
		framebufs.ssao.valid = framebufs.ssao.valid && GL_ValidateSimpleFramebuffer (framebufs.ssao.blur_fbo[i], va ("ssao blur fbo %d", i));
	}
	if (!framebufs.ssao.valid)
		Con_Warning ("SSAO framebuffer resources are invalid; SSAO disabled until framebuffer rebuild.\n");

	/* scene framebuffer (color + depth + stencil, potentially multisampled) */
	framebufs.scene.samples = Q_nextPow2 ((int)q_max (1.f, vid_fsaa.value));
	framebufs.scene.samples = CLAMP (1, framebufs.scene.samples, framebufs.max_samples);
	framebufs.scene.width = scene_w;
	framebufs.scene.height = scene_h;

	framebufs.scene.color_tex = GL_CreateFBOAttachment (color_format, scene_w, scene_h, framebufs.scene.samples, GL_NEAREST, "scene colors");
	framebufs.scene.velocity_tex = GL_CreateFBOAttachment (GL_RGBA16F, scene_w, scene_h, framebufs.scene.samples, GL_NEAREST, "scene velocity");
	framebufs.scene.depth_stencil_tex = GL_CreateFBOAttachment (depth_format, scene_w, scene_h, framebufs.scene.samples, GL_NEAREST, "scene depth/stencil");
	{
		GLuint colors[2] = { framebufs.scene.color_tex, framebufs.scene.velocity_tex };
		framebufs.scene.fbo = GL_CreateFBO (framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D,
			colors, 2,
			framebufs.scene.depth_stencil_tex,
			framebufs.scene.depth_stencil_tex,
			"scene fbo"
		);
	}

	/* weighted blended order-independent transparency (accum + revealage, potentially multisampled */
	framebufs.oit.accum_tex = GL_CreateFBOAttachment (GL_RGBA16F, scene_w, scene_h, framebufs.scene.samples, GL_NEAREST, "oit accum");
	framebufs.oit.revealage_tex = GL_CreateFBOAttachment (GL_R8, scene_w, scene_h, framebufs.scene.samples, GL_NEAREST, "oit revealage");

	// FIX #1: Initialize MRT array before using it
	framebufs.oit.mrt[0] = framebufs.oit.accum_tex;
	framebufs.oit.mrt[1] = framebufs.oit.revealage_tex;

	framebufs.oit.fbo_scene = GL_CreateFBO (framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D,
		framebufs.oit.mrt, 2,
		framebufs.scene.depth_stencil_tex,
		framebufs.scene.depth_stencil_tex,
		"oit scene fbo"
	);

	/* resolved scene framebuffer (color only) */
	if (framebufs.scene.samples > 1)
	{
		framebufs.resolved_scene.width = scene_w;
		framebufs.resolved_scene.height = scene_h;
		framebufs.resolved_scene.color_tex = GL_CreateFBOAttachment (color_format, scene_w, scene_h, 1, GL_NEAREST, "resolved scene colors");
		framebufs.resolved_scene.velocity_tex = GL_CreateFBOAttachment (GL_RGBA16F, scene_w, scene_h, 1, GL_NEAREST, "resolved scene velocity");
		{
			GLuint colors[2] = { framebufs.resolved_scene.color_tex, framebufs.resolved_scene.velocity_tex };
			framebufs.resolved_scene.fbo = GL_CreateFBO (GL_TEXTURE_2D, colors, 2, 0, 0, "resolved scene fbo");
		}
	}
	else
	{
		framebufs.resolved_scene.width = 0;
		framebufs.resolved_scene.height = 0;
		framebufs.resolved_scene.color_tex = 0;
		framebufs.resolved_scene.velocity_tex = 0;
		framebufs.resolved_scene.fbo = 0;

		// FIX #1: MRT array already initialized above, reuse it
		framebufs.oit.fbo_composite = GL_CreateFBO (GL_TEXTURE_2D,
			framebufs.oit.mrt, 2,
			framebufs.composite.depth_stencil_tex,
			framebufs.composite.depth_stencil_tex,
			"oit composite fbo"
		);
	}

        GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
        GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, 0);
}

/*
=============
GL_DeleteFrameBuffers
=============
*/
void GL_DeleteFrameBuffers (void)
{
	R_Shadow_DeleteFrameBuffers ();
	GL_DeleteFramebuffersFunc (1, &framebufs.resolved_scene.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.oit.fbo_composite);
	GL_DeleteFramebuffersFunc (1, &framebufs.oit.fbo_scene);
	GL_DeleteFramebuffersFunc (1, &framebufs.scene.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.composite.fbo);
	GL_DeleteFramebuffersFunc (2, framebufs.fogvol.fbo);
	GL_DeleteFramebuffersFunc (2, framebufs.fogvol.history_fbo);
	GL_DeleteFramebuffersFunc (2, framebufs.fogvol.composite_fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.fogvol.finalcopy_fbo);
	R_Froxel_ResetResources ();
	GL_DeleteFramebuffersFunc (1, &framebufs.autoexposure.fbo);
	GL_AutoExposureDeletePBOs ();
	GL_DeleteFramebuffersFunc (1, &framebufs.bloom.extract_fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.bloom.pingpong_fbo[0]);
	GL_DeleteFramebuffersFunc (1, &framebufs.bloom.pingpong_fbo[1]);
	GL_DeleteFramebuffersFunc (1, &framebufs.godrays.source_fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.godrays.mask_fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.godrays.shafts_fbo);
	for (int i = 0; i < 2; ++i)
	{
		GL_DeleteFramebuffersFunc (1, &framebufs.ssao.ao_fbo[i]);
		GL_DeleteFramebuffersFunc (1, &framebufs.ssao.blur_fbo[i]);
	}
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);

	GL_DeleteNativeTexture (framebufs.resolved_scene.color_tex);
	GL_DeleteNativeTexture (framebufs.resolved_scene.velocity_tex);
	GL_DeleteNativeTexture (framebufs.fogvol.color_tex[0]);
	GL_DeleteNativeTexture (framebufs.fogvol.color_tex[1]);
	GL_DeleteNativeTexture (framebufs.fogvol.history_tex[0]);
	GL_DeleteNativeTexture (framebufs.fogvol.history_tex[1]);
	GL_DeleteNativeTexture (framebufs.fogvol.composite_tex[0]);
	GL_DeleteNativeTexture (framebufs.fogvol.composite_tex[1]);
	GL_DeleteNativeTexture (framebufs.fogvol.finalcopy_tex);
	GL_DeleteNativeTexture (framebufs.oit.revealage_tex);
	GL_DeleteNativeTexture (framebufs.oit.accum_tex);
	GL_DeleteNativeTexture (framebufs.scene.depth_stencil_tex);
	GL_DeleteNativeTexture (framebufs.scene.color_tex);
	GL_DeleteNativeTexture (framebufs.scene.velocity_tex);
	GL_DeleteNativeTexture (framebufs.autoexposure.tex);
	GL_DeleteNativeTexture (framebufs.bloom.pingpong_tex[0]);
	GL_DeleteNativeTexture (framebufs.bloom.pingpong_tex[1]);
	GL_DeleteNativeTexture (framebufs.bloom.extract_tex);
	GL_DeleteNativeTexture (framebufs.godrays.source_tex);
	GL_DeleteNativeTexture (framebufs.godrays.mask_tex);
	GL_DeleteNativeTexture (framebufs.godrays.shafts_tex);
	GL_DeleteNativeTexture (framebufs.ssao.noise_tex);
	framebufs.ssao.valid = false;
	r_ssao_invalid_warned = false;
	for (int i = 0; i < 2; ++i)
	{
		GL_DeleteNativeTexture (framebufs.ssao.ao_tex[i]);
		GL_DeleteNativeTexture (framebufs.ssao.blur_tex[i]);
	}
	GL_DeleteNativeTexture (framebufs.composite.depth_stencil_tex);
	GL_DeleteNativeTexture (framebufs.composite.color_tex);

	memset (&framebufs, 0, sizeof (framebufs));
}

static void GL_OrthoMatrix (float matrix[16], float left, float right, float bottom, float top, float n, float f)
{
	float rl = right - left;
	float tb = top - bottom;
	float fn = f - n;

	memset (matrix, 0, 16 * sizeof (float));

	if (rl == 0.f || tb == 0.f || fn == 0.f)
	{
		IdentityMatrix (matrix);
		return;
	}

	matrix[0 * 4 + 0] = 2.f / rl;
	matrix[1 * 4 + 1] = 2.f / tb;
	if (gl_clipcontrol_able)
	{
		matrix[2 * 4 + 2] = 1.f / (n - f);
		matrix[3 * 4 + 2] = n / (n - f);
	}
	else
	{
		matrix[2 * 4 + 2] = -2.f / fn;
		matrix[3 * 4 + 2] = -(f + n) / fn;
	}
	matrix[3 * 4 + 0] = -(right + left) / rl;
	matrix[3 * 4 + 1] = -(top + bottom) / tb;
	matrix[3 * 4 + 3] = 1.f;
}


static GLuint GL_GenerateBloomTexture (void)
{
	int width = framebufs.bloom.width;
	int height = framebufs.bloom.height;
	GLuint fallback = framebufs.bloom.pingpong_tex[0] ? framebufs.bloom.pingpong_tex[0] : framebufs.bloom.extract_tex;
	if (fallback == 0)
		fallback = framebufs.bloom.extract_tex;
	if (width <= 0 || height <= 0)
		return fallback;
	if (!glprogs.bloom_extract || !glprogs.bloom_blur)
		return fallback;

	float threshold = q_max (0.f, r_bloom_threshold.value);

	GL_BeginGroup ("Bloom extract");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.bloom.extract_fbo);
	glViewport (0, 0, width, height);
	GL_UseProgram (glprogs.bloom_extract);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.color_tex);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, 0);
	GL_Uniform4fFunc (0, threshold, 0.f, 0.f, 0.f);
	GL_Uniform4fFunc (1, (float)R_GetNativeRenderWidth (), (float)R_GetNativeRenderHeight (),
		(float)R_GetNativeRenderWidth () / (float)width,
		(float)R_GetNativeRenderHeight () / (float)height);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	GL_EndGroup ();

	GLuint input_tex = framebufs.bloom.extract_tex;
	const int passes = 4;
	GL_BeginGroup ("Bloom blur");
	for (int pass = 0; pass < passes; ++pass)
	{
		int target_index = pass & 1;
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.bloom.pingpong_fbo[target_index]);
		glViewport (0, 0, width, height);
		GL_UseProgram (glprogs.bloom_blur);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, input_tex);
		float dirx = (pass & 1) ? 0.f : 1.f;
		float diry = (pass & 1) ? 1.f : 0.f;
		GL_Uniform4fFunc (0, 1.f / (float)width, 1.f / (float)height, dirx, diry);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		input_tex = framebufs.bloom.pingpong_tex[target_index];
	}
	GL_EndGroup ();

	return input_tex;
}

static GLuint GL_GenerateBloomTextureFrom (GLuint source_tex, float threshold, float radius_scale)
{
	int width = framebufs.bloom.width;
	int height = framebufs.bloom.height;
	GLuint fallback = framebufs.bloom.pingpong_tex[0] ? framebufs.bloom.pingpong_tex[0] : framebufs.bloom.extract_tex;

	if (fallback == 0)
		fallback = framebufs.bloom.extract_tex;
	if (width <= 0 || height <= 0 || source_tex == 0)
		return fallback;
	if (!glprogs.bloom_extract || !glprogs.bloom_blur)
		return fallback;

	threshold = q_max (0.f, threshold);
	float radius = q_max (0.f, radius_scale);
	if (radius <= 0.f)
		radius = 1.f;

	GL_BeginGroup ("Dlight bloom extract");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.bloom.extract_fbo);
	glViewport (0, 0, width, height);
	GL_UseProgram (glprogs.bloom_extract);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, source_tex);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, 0);
	GL_Uniform4fFunc (0, threshold, 0.f, 0.f, 0.f);
	GL_Uniform4fFunc (1, (float)R_GetNativeRenderWidth (), (float)R_GetNativeRenderHeight (),
		(float)R_GetNativeRenderWidth () / (float)width,
		(float)R_GetNativeRenderHeight () / (float)height);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	GL_EndGroup ();

	GLuint input_tex = framebufs.bloom.extract_tex;
	const int passes = 4;
	GL_BeginGroup ("Dlight bloom blur");
	for (int pass = 0; pass < passes; ++pass)
	{
		int target_index = pass & 1;
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.bloom.pingpong_fbo[target_index]);
		glViewport (0, 0, width, height);
		GL_UseProgram (glprogs.bloom_blur);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, input_tex);
		float dirx = (pass & 1) ? 0.f : 1.f;
		float diry = (pass & 1) ? 1.f : 0.f;
		GL_Uniform4fFunc (0, radius / (float)width, radius / (float)height, dirx, diry);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		input_tex = framebufs.bloom.pingpong_tex[target_index];
	}
	GL_EndGroup ();

	return input_tex;
}

static void GL_LogSSAODepthInfo (GLuint depth_tex, GLuint ao_tex, int ssao_width, int ssao_height, float view_min_x, float view_min_y, float view_max_x, float view_max_y)
{
	static GLuint last_depth_tex = 0;
	static GLuint last_ao_tex = 0;
	static int last_ssao_width = 0;
	static int last_ssao_height = 0;
	static int last_debug_mode = -1;
	static float last_view_min_x = -1.f;
	static float last_view_min_y = -1.f;
	static float last_view_max_x = -1.f;
	static float last_view_max_y = -1.f;

	int debug_mode = (int)Q_rint (r_ssao_debug.value);
	if (depth_tex == 0 || ao_tex == 0 || debug_mode <= 0)
		return;

	if (debug_mode == last_debug_mode &&
		depth_tex == last_depth_tex &&
		ao_tex == last_ao_tex &&
		ssao_width == last_ssao_width &&
		ssao_height == last_ssao_height &&
		view_min_x == last_view_min_x &&
		view_min_y == last_view_min_y &&
		view_max_x == last_view_max_x &&
		view_max_y == last_view_max_y)
		return;

	last_debug_mode = debug_mode;
	last_depth_tex = depth_tex;
	last_ao_tex = ao_tex;
	last_ssao_width = ssao_width;
	last_ssao_height = ssao_height;
	last_view_min_x = view_min_x;
	last_view_min_y = view_min_y;
	last_view_max_x = view_max_x;
	last_view_max_y = view_max_y;

	GLint internal_format = 0;
	GLint tex_width = 0;
	GLint tex_height = 0;
	GLint compare_mode = 0;
	GLint bound_depth_tex = 0;
	GLint ao_internal_format = 0;
	GLint ao_width = 0;
	GLint ao_height = 0;
	GLint normal_width = 0;
	GLint normal_height = 0;
	GLfloat depth_range[2] = { 0.f, 1.f };
	GLint viewport[4] = { 0, 0, 0, 0 };
	GLint scissor_box[4] = { 0, 0, 0, 0 };
	GLint draw_fbo = 0;
	GLboolean scissor_enabled = GL_FALSE;
	GLboolean color_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	const char *target_name = "GL_TEXTURE_2D";

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, depth_tex);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &bound_depth_tex);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_width);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_height);
	glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &compare_mode);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, ao_tex);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ao_internal_format);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &ao_width);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ao_height);
	glGetFloatv (GL_DEPTH_RANGE, depth_range);
	glGetIntegerv (GL_VIEWPORT, viewport);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_FRAMEBUFFER_BINDING, &draw_fbo);
	scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
	glGetBooleanv (GL_COLOR_WRITEMASK, color_mask);

	Con_DPrintf ("SSAO depth input: tex=%u bound=%d target=%s format=0x%X size=%dx%d compare=0x%X viewport=%dx%d+%d+%d fbo=%d scissor=%s %dx%d+%d+%d colorMask=%d%d%d%d\n",
		depth_tex, bound_depth_tex, target_name, internal_format, tex_width, tex_height,
		compare_mode, viewport[2], viewport[3], viewport[0], viewport[1], draw_fbo,
		scissor_enabled ? "on" : "off",
		scissor_box[2], scissor_box[3], scissor_box[0], scissor_box[1],
		color_mask[0] ? 1 : 0, color_mask[1] ? 1 : 0, color_mask[2] ? 1 : 0, color_mask[3] ? 1 : 0);
	Con_DPrintf ("SSAO AO output: tex=%u format=0x%X size=%dx%d screen=%dx%d normals=%dx%d\n",
	ao_tex, ao_internal_format, ao_width, ao_height, R_GetNativeRenderWidth (), R_GetNativeRenderHeight (), normal_width, normal_height);
	Con_DPrintf ("SSAO depth range: [%0.4f, %0.4f] reversedZ=%d mode=%d znear=%0.4f zfar=%0.4f cutoff=%0.4f viewRect=[%0.3f,%0.3f]-[%0.3f,%0.3f] ssao=%dx%d debug=%d\n",
		depth_range[0], depth_range[1],
		gl_clipcontrol_able ? 1 : 0,
		(int)Q_rint (r_ssao_reversedz_mode.value),
		view_znear, view_zfar,
		gl_clipcontrol_able ? 0.001f : 0.999f,
		view_min_x, view_min_y, view_max_x, view_max_y,
		ssao_width, ssao_height, debug_mode);
	GL_LogErrorIfDeveloper ("GL_LogSSAODepthInfo");
}

static GLuint GL_GenerateSSAOTexture (float view_min_x, float view_min_y, float view_max_x, float view_max_y)
{
	if ((r_ssao.value <= 0.f || r_ssao_intensity.value <= 0.f) && r_ssao_debug.value <= 0.f)
		return 0;
	if (!glprogs.ssao || !framebufs.composite.depth_stencil_tex || !framebufs.ssao.noise_tex)
		return 0;
	if (!framebufs.ssao.valid)
	{
		if (!r_ssao_invalid_warned)
		{
			Con_Warning ("Skipping SSAO: framebuffer resources are invalid (rebuild required).\n");
			r_ssao_invalid_warned = true;
		}
		return 0;
	}

	int samples = (int)Q_rint (r_ssao_samples.value);
	samples = CLAMP (4, samples, SSAO_MAX_SAMPLES);
	float radius = R_SSAO_SanitizeValue (r_ssao_radius.value, 24.f, 0.01f, 8192.f);
	float bias = R_SSAO_SanitizeValue (r_ssao_bias.value, 0.02f, 0.f, 1.f) * radius;
	float power = R_SSAO_SanitizeValue (r_ssao_power.value, 1.f, 0.01f, 8.f);
	float min_ao = CLAMP (0.f, r_ssao_min.value, 1.f);
	qboolean use_halfres = (r_ssao_halfres.value > 0.f && r_ssao_force_fullres.value <= 0.f);
	int index = use_halfres ? 1 : 0;
	int width = framebufs.ssao.width[index];
	int height = framebufs.ssao.height[index];
	if (width <= 0 || height <= 0)
		return 0;

	r_ssao_invalid_warned = false;

	float reversed_z = gl_clipcontrol_able ? 1.f : 0.f;
	float depth_cutoff = gl_clipcontrol_able ? 0.001f : 0.999f;
	int reversed_z_mode = (int)Q_rint (r_ssao_reversedz_mode.value);
	reversed_z_mode = CLAMP (0, reversed_z_mode, 2);
	int debug_mode_cvar = (int)Q_rint (r_ssao_debug.value);
	int debug_mode_i = (debug_mode_cvar > 0) ? CLAMP (1, debug_mode_cvar, 14) : -1;
	qboolean debug_show_ao_raw = (debug_mode_i == 10);
	qboolean debug_show_blur_debug = (debug_mode_i == 11);
	int debug_mode_ssao = -1;
	if (debug_mode_i >= 1 && debug_mode_i <= 9)
		debug_mode_ssao = debug_mode_i;
	float debug_mode = (float)debug_mode_ssao;
	float debug_far = q_max (0.1f, r_ssao_debug_far.value);
	float noise_enabled = (r_ssao_noise.value > 0.f) ? 1.f : 0.f;
	float noise_seed = (r_ssao_freeze_noise.value > 0.f) ? 0.f : (float)r_framecount;
	int noise_mode = (int)Q_rint (r_ssao_noise_mode.value);
	float noise_scale = R_SSAO_SanitizeValue (r_ssao_noise_scale.value, 1.f, 0.1f, 64.f);
	noise_mode = CLAMP (0, noise_mode, 2);
	if (noise_enabled <= 0.5f)
		noise_mode = 0;
	else if (noise_mode == 0)
		noise_mode = 1;
	int normal_source = (int)Q_rint (r_ssao_normalsource.value);
	normal_source = CLAMP (0, normal_source, 1);
	float max_distance = R_SSAO_SanitizeValue (r_ssao_max_distance.value, 1024.f, 1.f, 65536.f);
	static qboolean ssao_logged = false;
	if (!ssao_logged)
	{
		// SSAO FIX: Log depth configuration once to validate reversed-Z/clip control wiring.
		Con_DPrintf ("SSAO config: reversedZ=%d clipcontrol=%d znear=%0.4f zfar=%0.4f\n",
			(int)reversed_z, gl_clipcontrol_able ? 1 : 0, view_znear, view_zfar);
		ssao_logged = true;
	}

	GL_BeginGroup ("SSAO");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.ssao.ao_fbo[index]);
	GL_LogErrorIfDeveloper ("SSAO bind FBO");
	if (r_ssao_validate.value > 0.f)
	{
		GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_DPrintf ("SSAO runtime validation failed (0x%X)\n", status);
			framebufs.ssao.valid = false;
			GL_EndGroup ();
			return 0;
		}
	}
	// SSAO FIX: Reset viewport/scissor/color mask per pass to avoid banding from stale state.
	GL_SetScissorEnabled (false);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glViewport (0, 0, width, height);
	{
		const float clear[4] = { 1.f, 1.f, 1.f, 1.f };
		GL_ClearBufferfvFunc (GL_COLOR, 0, clear);
	}

	GL_LogSSAODepthInfo (framebufs.composite.depth_stencil_tex, framebufs.ssao.ao_tex[index], width, height, view_min_x, view_min_y, view_max_x, view_max_y);

	GL_UseProgram (glprogs.ssao);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, framebufs.ssao.noise_tex);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matproj);
	GL_UniformMatrix4fvFunc (1, 1, GL_FALSE, r_matinvproj);
	GL_Uniform4fFunc (2, radius, bias, power, min_ao);
	GL_Uniform4fFunc (3,
		1.f / (float)R_GetNativeRenderWidth (),
		1.f / (float)R_GetNativeRenderHeight (),
		(float)R_GetNativeRenderWidth (),
		(float)R_GetNativeRenderHeight ());
	GL_Uniform4fFunc (4,
		1.f / (float)width,
		1.f / (float)height,
		(float)width,
		(float)height);
	GL_Uniform4fFunc (5,
		((float)width / (float)SSAO_NOISE_SIZE) * noise_scale,
		((float)height / (float)SSAO_NOISE_SIZE) * noise_scale,
		noise_enabled,
		noise_seed);
	GL_Uniform4fFunc (6, view_znear, view_zfar, reversed_z, depth_cutoff);
	GL_Uniform4fFunc (7, view_min_x, view_min_y, view_max_x, view_max_y);
	GL_Uniform1iFunc (8, samples);
	GL_Uniform4fFunc (9, debug_mode, debug_far, 0.f, 0.f);
	GL_Uniform1iFunc (10, reversed_z_mode);
	GL_Uniform1iFunc (11, normal_source);
	GL_Uniform1iFunc (12, 0);
	GL_Uniform1iFunc (13, noise_mode);
	/* Feed cached fogvol global density to SSAO instead of the removed analytic
	 * GL fog UBO channel. */
	GL_Uniform4fFunc (14, max_distance, r_ssao_fog_state.density,
		r_ssao_fog_state.color[0] * 0.299f + r_ssao_fog_state.color[1] * 0.587f + r_ssao_fog_state.color[2] * 0.114f,
		0.f);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	GL_LogErrorIfDeveloper ("SSAO draw");

	qboolean allow_blur = (r_ssao_blur.value > 0.f) && (debug_mode_i < 0);
	qboolean run_blur = glprogs.ssao_blur && allow_blur && !debug_show_ao_raw && !debug_show_blur_debug;
	if (run_blur)
	{
		int blur_radius = (int)Q_rint (r_ssao_blur_radius.value);
		blur_radius = CLAMP (1, blur_radius, 4);
		float blur_sigma = q_max (0.01f, r_ssao_blur_sigma.value);
		float depth_threshold_scale = 0.02f;
		float blur_bilateral = (r_ssao_blur_bilateral.value > 0.f) ? 1.f : 0.f;

		GL_UseProgram (glprogs.ssao_blur);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
		GL_Uniform4fFunc (0,
			1.f / (float)R_GetNativeRenderWidth (),
			1.f / (float)R_GetNativeRenderHeight (),
			(float)R_GetNativeRenderWidth (),
			(float)R_GetNativeRenderHeight ());
		GL_Uniform4fFunc (1,
			1.f / (float)width,
			1.f / (float)height,
			(float)width,
			(float)height);
		GL_Uniform4fFunc (3, view_znear, view_zfar, reversed_z, depth_cutoff);
		GL_Uniform4fFunc (4, blur_sigma, (float)blur_radius, depth_threshold_scale, blur_bilateral);
		GL_Uniform4fFunc (5, view_min_x, view_min_y, view_max_x, view_max_y);
		GL_Uniform1iFunc (6, reversed_z_mode);
		GL_Uniform1iFunc (7, 0);
		GL_UniformMatrix4fvFunc (8, 1, GL_FALSE, r_matinvproj);

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.ssao.blur_fbo[index]);
		GL_LogErrorIfDeveloper ("SSAO blur bind FBO");
		GL_SetScissorEnabled (false);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glViewport (0, 0, width, height);
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.ssao.ao_tex[index]);
		GL_Uniform4fFunc (2, 1.f, 0.f, 0.f, 0.f);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		GL_LogErrorIfDeveloper ("SSAO blur horizontal draw");

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.ssao.ao_fbo[index]);
		GL_SetScissorEnabled (false);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.ssao.blur_tex[index]);
		GL_Uniform4fFunc (2, 0.f, 1.f, 0.f, 0.f);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		GL_LogErrorIfDeveloper ("SSAO blur vertical draw");
	}

	GL_EndGroup ();

	return framebufs.ssao.ao_tex[index];
}

void R_ResetGodraysStabilization (void)
{
	R_Godrays_ResetStabilization (&r_godrays_stabilization);
}

static void R_GetGodraysSkyParams_Current (godrays_sky_params_t *params)
{
	R_Godrays_GetSkyParams (
		r_godrays.value,
		r_godrays_sky_threshold.value,
		r_godrays_sky_intensity.value,
		r_godrays_blur.value,
		r_godrays_sky_tint.string,
		params);
}

static void R_InvalidateGodraysFrameCache (void)
{
	r_godrays_generated_frame = -1;
	r_godrays_cached_shafts = 0;
	r_godrays_cached_mask = 0;
	r_godrays_cached_source = 0;
	r_godrays_cached_debug_source_generated = false;
}

static void R_InvalidateTemporalHistoryOnSceneResize (void)
{
	R_FogVol_ClearHistory ();
	R_ResetGodraysStabilization ();
	R_InvalidateGodraysFrameCache ();
	r_prev_frame_valid = false;
}

static qboolean R_GodraysMediumEnabled (void)
{
	/* Godrays are a volumetric effect and should only run when a volumetric
	 * fog medium path is active (fog volumes and/or froxel fog). */
	return R_FogVol_ShouldAffectPostFX ();
}

static void R_GetGodraysLightPos_Current (float *out_x, float *out_y)
{
	const sun_t *sun;
	float dist;
	vec3_t sun_world;
	vec3_t proj;
	float x = 0.5f;
	float y = 0.0f;

	if (!out_x || !out_y)
		return;

	if (!R_WorldHasSun ())
	{
		/* Maps without sun keys: force a sky-anchored source. */
		*out_x = 0.5f;
		*out_y = 0.0f;
		return;
	}

	sun = R_GetSun ();
	dist = q_max (256.f, q_max (gl_farclip.value * 0.5f, 1024.f));
	VectorMA (r_refdef.vieworg, dist, sun->dir, sun_world);
	ProjectVector (sun_world, r_matviewproj, proj);

	if (proj[2] > 0.f)
	{
		x = CLAMP (0.f, proj[0] * 0.5f + 0.5f, 1.f);
		y = CLAMP (0.f, proj[1] * 0.5f + 0.5f, 1.f);
	}
	else
	{
		/*
		 * Keep the source anchored to the sky even when the sun direction is
		 * behind the camera: project toward the nearest screen edge instead of
		 * collapsing to screen center.
		 */
		float edge_x = proj[0];
		float edge_y = proj[1];
		float edge_scale = q_max (fabsf (edge_x), fabsf (edge_y));

		if (edge_scale > 1e-6f)
		{
			edge_x /= edge_scale;
			edge_y /= edge_scale;
			x = CLAMP (0.f, edge_x * 0.5f + 0.5f, 1.f);
			y = CLAMP (0.f, edge_y * 0.5f + 0.5f, 1.f);
		}
	}

	*out_x = x;
	*out_y = y;
}

static void GL_GenerateGodraysSource (qboolean draw_sky, qboolean draw_brush)
{
	int width = R_GetNativeRenderWidth ();
	int height = R_GetNativeRenderHeight ();
	if (framebufs.godrays.source_fbo == 0 || framebufs.godrays.source_tex == 0)
		return;
	if (!draw_sky && !draw_brush)
		return;
	float mask_knee = q_max (0.f, r_godrays_mask_knee.value);

	GL_BeginGroup ("Godrays source");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.source_fbo);
	glViewport (0, 0, width, height);
	{
		const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
		GL_ClearBufferfvFunc (GL_COLOR, 0, zero);
	}

	godrays_sky_params_t sky_params;

	R_GetGodraysSkyParams_Current (&sky_params);

	if (draw_sky && glprogs.godrays_source_sky && sky_params.enabled)
	{
		float sky_intensity = sky_params.intensity;
		float reversed_z = gl_clipcontrol_able ? 1.f : 0.f;
		float sky_depth_cutoff = gl_clipcontrol_able ? 0.001f : 0.999f;

		/*
		 * Keep sky godrays available whenever the effect itself is active.
		 * A zeroed intensity cvar should not silently suppress the sky source pass.
		 */
		if (sky_intensity <= 0.f)
			sky_intensity = 1.f;

		GL_UseProgram (glprogs.godrays_source_sky);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
		GL_Uniform4fFunc (0, sky_depth_cutoff, sky_intensity, reversed_z, sky_params.threshold);
		GL_Uniform4fFunc (1, sky_params.tint[0], sky_params.tint[1], sky_params.tint[2], 0.f);
		GL_Uniform4fFunc (2, mask_knee, 0.f, 0.f, 0.f);
		glDrawArrays (GL_TRIANGLES, 0, 3);
	}

	if (draw_brush && glprogs.godrays_source)
	{
		int count = 0;
		entity_t **ents = R_GetVisEntities (mod_brush, false, &count);
		if (count > 0)
		{
			GL_UseProgram (glprogs.godrays_source);
			GL_SetState (GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (6));
			GL_Uniform4fFunc (0,
				q_max (0.f, r_godrays_emissive_intensity.value),
				q_max (0.f, r_godrays_lighttex_intensity.value),
				q_max (0.f, r_godrays_emissive_threshold.value),
				q_max (0.f, r_godrays_light_threshold.value));
			GL_Uniform4fFunc (1, mask_knee, 0.f, 0.f, 0.f);
			R_DrawBrushModels_Godrays (ents, count);
		}
	}

	GL_EndGroup ();
}


typedef struct medium_scatter_source_s {
	GLuint texture;
	float valid;
} medium_scatter_source_t;

static medium_scatter_source_t GL_GetMediumScatterSource (void)
{
	medium_scatter_source_t medium = { 0, 0.f };

	/*
	 * Medium scatter/transmittance texture contract (shared by postprocess + godrays):
	 *  - RGB: medium in-scatter/radiance already composited in display space.
	 *  - A:   medium coverage proxy = 1 - transmittance.
	 *         Expected normalized range: [0, 1] where 0 means no medium and 1 means
	 *         fully opaque medium for this pixel.
	 *  - valid: CPU-side frame validity gate. Only sample texture when valid > 0.5.
	 *
	 * Compatibility layer: FogVol is the first implementation of this interface.
	 */
	if (R_FogVol_HasValidComposite ())
	{
		medium.texture = R_FogVol_GetCompositeTex ();
		medium.valid = 1.f;
	}

	return medium;
}

static GLuint GL_GenerateGodraysTexture (GLuint *out_mask)
{
	int width = framebufs.godrays.width;
	int height = framebufs.godrays.height;
	GLuint fallback = 0;
	if (out_mask)
		*out_mask = 0;
	if (width <= 0 || height <= 0)
		return fallback;
	if (!glprogs.godrays_mask || !glprogs.godrays)
		return fallback;
	if (framebufs.godrays.mask_fbo == 0 || framebufs.godrays.shafts_fbo == 0)
		return fallback;
	if (framebufs.godrays.source_fbo == 0 || framebufs.godrays.source_tex == 0)
		return fallback;
	if (!R_Godrays_IsReady (cl.worldmodel, r_framecount))
		return fallback;

	godrays_sky_params_t sky_params;
	qboolean emit_sky;

	R_GetGodraysSkyParams_Current (&sky_params);
	emit_sky = (glprogs.godrays_source_sky != 0 && sky_params.enabled);
	/* Force sky-driven godrays only: ignore local brush/lighttex emitters. */
	qboolean emit_brush = false;
	if (!emit_sky && !emit_brush)
		return fallback;

	float samples_value = R_Godrays_SanitizeValue (r_godrays_samples.value, 48.f, 1.f, 128.f);
	int samples = (int)Q_rint (samples_value);
	samples = CLAMP (8, samples, 128);

	float threshold = R_Godrays_SanitizeValue (r_godrays_threshold.value, 0.f, 0.f, FLT_MAX);
	float density = R_GetSun ()->ray_density;
	float weight = R_Godrays_SanitizeValue (r_godrays_weight.value, 0.015f, 0.f, FLT_MAX);
	float decay = R_GetSun ()->ray_decay;
	float exposure = R_Godrays_SanitizeValue (r_godrays_exposure.value, 1.f, 0.f, FLT_MAX);
	float softness = R_Godrays_SanitizeValue (r_godrays_blur.value, 1.5f, 0.f, FLT_MAX);
	float sharpness = 1.25f;
	float max_radius = 1.f;
	float light_x = 0.5f;
	float light_y = 0.5f;
	float stabilized_x = light_x;
	float stabilized_y = light_y;
	medium_scatter_source_t medium = { 0, 0.f };
	GLuint volumetric_tex = 0;
	float volumetric_enabled = 0.f;

	R_GetGodraysLightPos_Current (&light_x, &light_y);

	if (r_godrays.value > 0.f)
	{
		medium = GL_GetMediumScatterSource ();
		volumetric_tex = medium.texture;
		volumetric_enabled = medium.valid;
	}

	{
		r_godrays_stabilize_input_t stabilize_input;
		stabilize_input.width = width;
		stabilize_input.height = height;
		stabilize_input.raw_x = light_x;
		stabilize_input.raw_y = light_y;
		VectorCopy (r_refdef.viewangles, stabilize_input.viewangles);
		stabilize_input.time = cl.time;
		stabilize_input.stabilize = 0.f;
		stabilize_input.smooth_rate = 8.f;
		stabilize_input.stabilize_strength = 0.5f;
		stabilize_input.stabilize_max_px = 0.f;
		stabilize_input.max_shift_per_sec = 0.f;
		stabilize_input.reset_on_teleport = true;
		R_Godrays_ComputeLightPos (&r_godrays_stabilization, &stabilize_input, &stabilized_x, &stabilized_y);
	}

	GL_BeginGroup ("Godrays scatter");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.shafts_fbo);
	glViewport (0, 0, width, height);
	{
		const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
		GL_ClearBufferfvFunc (GL_COLOR, 0, zero);
	}
	GL_EndGroup ();

	{
		qboolean first_pass = true;
		float sky_softness = sky_params.softness;

		if (emit_sky)
		{
			GL_GenerateGodraysSource (true, false);

			GL_BeginGroup ("Godrays mask (sky)");
			GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.mask_fbo);
			glViewport (0, 0, width, height);
			{
				const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
				GL_ClearBufferfvFunc (GL_COLOR, 0, zero);
			}
			GL_UseProgram (glprogs.godrays_mask);
			GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
			GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.godrays.source_tex);
			GL_Uniform4fFunc (0, threshold, sky_softness, 1.f, 0.f);
			GL_Uniform4fFunc (1, (float)R_GetNativeRenderWidth (), (float)R_GetNativeRenderHeight (),
				(float)R_GetNativeRenderWidth () / (float)width,
				(float)R_GetNativeRenderHeight () / (float)height);
			glDrawArrays (GL_TRIANGLES, 0, 3);
			GL_EndGroup ();

			GL_BeginGroup ("Godrays scatter (sky)");
			GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.shafts_fbo);
			glViewport (0, 0, width, height);
			GL_UseProgram (glprogs.godrays);
			GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
			GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.godrays.mask_tex);
			GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, volumetric_tex);
			GL_Uniform4fFunc (0, stabilized_x, stabilized_y, density, weight);
			GL_Uniform4fFunc (1, decay, exposure, max_radius, (float)samples);
			GL_Uniform4fFunc (2, volumetric_enabled, q_max (0.f, r_godrays_vol_pow.value), volumetric_enabled, 0.f);
			glDrawArrays (GL_TRIANGLES, 0, 3);
			GL_EndGroup ();
			first_pass = false;
		}

		if (emit_brush)
		{
			GL_GenerateGodraysSource (false, true);

			GL_BeginGroup ("Godrays mask (brush)");
			GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.mask_fbo);
			glViewport (0, 0, width, height);
			{
				const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
				GL_ClearBufferfvFunc (GL_COLOR, 0, zero);
			}
			GL_UseProgram (glprogs.godrays_mask);
			GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
			GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.godrays.source_tex);
			GL_Uniform4fFunc (0, threshold, softness, sharpness, 0.f);
			GL_Uniform4fFunc (1, (float)R_GetNativeRenderWidth (), (float)R_GetNativeRenderHeight (),
				(float)R_GetNativeRenderWidth () / (float)width,
				(float)R_GetNativeRenderHeight () / (float)height);
			glDrawArrays (GL_TRIANGLES, 0, 3);
			GL_EndGroup ();

			GL_BeginGroup ("Godrays scatter (brush)");
			GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.shafts_fbo);
			glViewport (0, 0, width, height);
			GL_UseProgram (glprogs.godrays);
			GL_SetState ((first_pass ? GLS_BLEND_OPAQUE : GLS_BLEND_ADD) | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
			GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.godrays.mask_tex);
			GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, volumetric_tex);
			GL_Uniform4fFunc (0, stabilized_x, stabilized_y, density, weight);
			GL_Uniform4fFunc (1, decay, exposure, max_radius, (float)samples);
			GL_Uniform4fFunc (2, volumetric_enabled, q_max (0.f, r_godrays_vol_pow.value), volumetric_enabled, 0.f);
			glDrawArrays (GL_TRIANGLES, 0, 3);
			GL_EndGroup ();
		}
	}

	if (out_mask)
		*out_mask = framebufs.godrays.mask_tex;

	return framebufs.godrays.shafts_tex;
}

static void R_EnsureGodraysTexturesForFrame (qboolean allow_debug_source)
{
	qboolean godrays_enabled;
	qboolean godrays_debug_enabled;
	qboolean godrays_preview;
	float godrays_debug;
	float godrays_debug_source;
	qboolean ready;
	qboolean medium_enabled;

	if (r_godrays_generated_frame == r_framecount)
	{
		if (allow_debug_source
			&& !r_godrays_cached_debug_source_generated
			&& R_Godrays_IsReady (cl.worldmodel, r_framecount)
			&& r_godrays_debug_source.value > 0.f)
		{
			GL_GenerateGodraysSource (
				(glprogs.godrays_source_sky != 0),
				true);
			r_godrays_cached_debug_source_generated = true;
			r_godrays_cached_source = framebufs.godrays.source_tex;
		}
		return;
	}

	r_godrays_generated_frame = r_framecount;
	r_godrays_cached_shafts = 0;
	r_godrays_cached_mask = 0;
	r_godrays_cached_source = 0;
	r_godrays_cached_debug_source_generated = false;

	ready = R_Godrays_IsReady (cl.worldmodel, r_framecount);
	medium_enabled = R_GodraysMediumEnabled ();
	godrays_enabled = (r_godrays.value > 0.f && medium_enabled && ready);
	godrays_debug = (r_godrays_debug.value > 0.f) ? 1.f : 0.f;
	godrays_debug_source = CLAMP (0.f, r_godrays_debug_source.value, 2.f);
	godrays_debug_enabled = (godrays_debug > 0.f || godrays_debug_source > 0.f);
	godrays_preview = (godrays_enabled || (godrays_debug_enabled && ready));
	if (!godrays_preview)
		return;

	if (allow_debug_source && godrays_debug_source > 0.f && ready)
	{
		GL_GenerateGodraysSource (
			(glprogs.godrays_source_sky != 0),
			true);
		r_godrays_cached_debug_source_generated = true;
	}

	if (godrays_enabled || godrays_debug > 0.f)
		r_godrays_cached_shafts = GL_GenerateGodraysTexture (&r_godrays_cached_mask);

	r_godrays_cached_source = framebufs.godrays.source_tex;
}

static void R_PrepareFogVolInputs (void)
{
	const qboolean need_godray_inputs = R_FogVol_IsEnabledForFrame () && R_FogVol_HasRenderableContent ();

	if (!need_godray_inputs || !R_FogVol_IsEnabledForFrame () || !R_FogVol_HasRenderableContent ())
	{
		R_FogVol_SetGodrayCouplingTextures (0, 0, 0, false);
		return;
	}

	R_EnsureGodraysTexturesForFrame (false);
	R_FogVol_SetGodrayCouplingTextures (
		r_godrays_cached_shafts,
		r_godrays_cached_mask,
		r_godrays_cached_source,
		r_godrays_cached_shafts != 0);
}


static qboolean GL_ShouldApplyMotionBlur (void)
{
	if (r_motionblur.value <= 0.f)
	return false;

	return GL_ConsoleVisibility () <= 0.f;
}

static void GL_SetFramebufferSRGB (qboolean enable)
{
#ifdef GL_FRAMEBUFFER_SRGB
	if (enable && !gl_framebuffer_srgb_enabled)
	{
		glEnable (GL_FRAMEBUFFER_SRGB);
		gl_framebuffer_srgb_enabled = true;
	}
	else if (!enable && gl_framebuffer_srgb_enabled)
	{
		glDisable (GL_FRAMEBUFFER_SRGB);
		gl_framebuffer_srgb_enabled = false;
	}
#else
	(void)enable;
#endif
}

static qboolean GL_UseSRGBFramebuffer (void)
{
	if (r_srgb_framebuffer.value <= 0.f)
		return false;
	if (!vid_framebuffer_srgb_capable)
	{
		if (!gl_srgb_capability_warned)
		{
			Con_Warning ("Default framebuffer is not sRGB-capable; disabling r_srgb_framebuffer.\n");
			gl_srgb_capability_warned = true;
		}
		Cvar_SetValueQuick (&r_srgb_framebuffer, 0.f);
		return false;
	}
	return true;
}

static void GL_PostProcessFallback (void)
{
	int width = glwidth;
	int height = glheight;
	size_t numpixels = (size_t)width * (size_t)height;
	size_t bufsize = numpixels * 4;
	byte *pixels;

	if (framebufs.composite.fbo == 0 || framebufs.composite.color_tex == 0)
		return;

	pixels = (byte *)q_malloc(bufsize);
	if (!pixels)
		return;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.composite.fbo);
	glReadBuffer (GL_COLOR_ATTACHMENT0);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	glReadPixels (0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
	glReadBuffer (GL_BACK);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	{
		qboolean srgb_output = GL_UseSRGBFramebuffer ();
		GL_SetFramebufferSRGB (srgb_output);
	}

	float sat = CLAMP (0.9f, r_color_saturation.value, 1.2f);
	float post_contrast = CLAMP (0.8f, r_color_contrast.value, 1.2f);
	float midtone = q_max (0.1f, r_color_midtone.value);
	qboolean output_srgb = (r_srgb_framebuffer.value <= 0.f);
	if (vid_framebuffer_srgb_capable && r_srgb_framebuffer.value > 0.f)
		output_srgb = false;
	for (size_t i = 0; i < numpixels; ++i)
	{
		float color[3] = {
			pixels[i * 4 + 0] * (1.f / 255.f),
			pixels[i * 4 + 1] * (1.f / 255.f),
			pixels[i * 4 + 2] * (1.f / 255.f)
		};
		if (midtone != 1.f)
		{
			for (int c = 0; c < 3; ++c)
				color[c] = powf (color[c], 1.0f / midtone);
		}
		if (post_contrast != 1.f)
		{
			for (int c = 0; c < 3; ++c)
			{
				float t = color[c] * (1.f - color[c]);
				color[c] = CLAMP (0.f, color[c] + t * ((post_contrast - 1.f) * 2.f), 1.f);
			}
		}
		if (sat != 1.f)
		{
			float l = color[0] * 0.299f + color[1] * 0.587f + color[2] * 0.114f;
			color[0] = l + (color[0] - l) * sat;
			color[1] = l + (color[1] - l) * sat;
			color[2] = l + (color[2] - l) * sat;
		}
		if (output_srgb)
		{
			for (int c = 0; c < 3; ++c)
			{
				if (color[c] <= 0.0031308f)
					color[c] = color[c] * 12.92f;
				else
					color[c] = 1.055f * powf (color[c], 1.0f / 2.4f) - 0.055f;
			}
		}
		pixels[i * 4 + 0] = (byte)CLAMP (0, (int)Q_rint (color[0] * 255.f), 255);
		pixels[i * 4 + 1] = (byte)CLAMP (0, (int)Q_rint (color[1] * 255.f), 255);
		pixels[i * 4 + 2] = (byte)CLAMP (0, (int)Q_rint (color[2] * 255.f), 255);
	}

	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
        GL_UseProgram (0);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, width, 0, height, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glRasterPos2i (0, 0);
	glDrawPixels (width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glPopMatrix ();
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);

	q_free(pixels);
}

static int GL_CompareFloat (const void *a, const void *b)
{
	const float fa = *(const float *)a;
	const float fb = *(const float *)b;

	if (fa < fb)
		return -1;
	if (fa > fb)
		return 1;
	return 0;
}

static qboolean GL_AutoExposurePBOAvailable (void)
{
	return GL_BindBufferFunc && GL_GenBuffersFunc && GL_BufferDataFunc && GL_DeleteBuffersFunc
		&& GL_MapBufferRangeFunc && GL_UnmapBufferFunc;
}

static void GL_AutoExposureDeletePBOs (void)
{
	if (GL_AutoExposurePBOAvailable ())
	{
		if (framebufs.autoexposure.pbo[0] || framebufs.autoexposure.pbo[1])
			GL_DeleteBuffersFunc (2, framebufs.autoexposure.pbo);
		GL_BindBufferFunc (GL_PIXEL_PACK_BUFFER, 0);
	}

	memset (framebufs.autoexposure.pbo, 0, sizeof (framebufs.autoexposure.pbo));
	framebufs.autoexposure.pbo_index = 0;
	framebufs.autoexposure.pbo_ready = false;
}

static void GL_AutoExposureInitPBOs (void)
{
	const GLsizeiptr size = (GLsizeiptr)(framebufs.autoexposure.width * framebufs.autoexposure.height * 4 * (int)sizeof (float));

	GL_AutoExposureDeletePBOs ();

	if (!GL_AutoExposurePBOAvailable () || size <= 0)
		return;

	GL_GenBuffersFunc (2, framebufs.autoexposure.pbo);
	if (!framebufs.autoexposure.pbo[0] || !framebufs.autoexposure.pbo[1])
	{
		GL_AutoExposureDeletePBOs ();
		return;
	}

	for (int i = 0; i < 2; ++i)
	{
		GL_BindBufferFunc (GL_PIXEL_PACK_BUFFER, framebufs.autoexposure.pbo[i]);
		GL_BufferDataFunc (GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);
	}
	GL_BindBufferFunc (GL_PIXEL_PACK_BUFFER, 0);
	framebufs.autoexposure.pbo_ready = false;
}

static qboolean GL_SampleAutoExposureLuminance (float *out_luminance)
{
	const int width = framebufs.autoexposure.width;
	const int height = framebufs.autoexposure.height;
	const int pixel_count = width * height;
	const GLsizeiptr pbo_size = (GLsizeiptr)(pixel_count * 4 * (int)sizeof (float));
	float pixels[16 * 16 * 4];
	float luminance_samples[16 * 16];
	GLint prev_pack_alignment = 4;

	if (framebufs.composite.fbo == 0 || framebufs.autoexposure.fbo == 0)
		return false;
	if (width <= 0 || height <= 0 || pixel_count > (int)countof (luminance_samples))
		return false;

	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.composite.fbo);
	GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.autoexposure.fbo);
	GL_BlitFramebufferFunc (0, 0, R_GetNativeRenderWidth (), R_GetNativeRenderHeight (), 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.autoexposure.fbo);
	glReadBuffer (GL_COLOR_ATTACHMENT0);
	glGetIntegerv (GL_PACK_ALIGNMENT, &prev_pack_alignment);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);

	if (r_autoexposure_async.value > 0.f && framebufs.autoexposure.pbo[0] && framebufs.autoexposure.pbo[1] && GL_AutoExposurePBOAvailable ())
	{
		const int write_index = framebufs.autoexposure.pbo_index;
		const int read_index = write_index ^ 1;
		qboolean got_data = false;

		GL_BindBufferFunc (GL_PIXEL_PACK_BUFFER, framebufs.autoexposure.pbo[write_index]);
		glReadPixels (0, 0, width, height, GL_RGBA, GL_FLOAT, NULL);

		GL_BindBufferFunc (GL_PIXEL_PACK_BUFFER, framebufs.autoexposure.pbo[read_index]);
		if (framebufs.autoexposure.pbo_ready)
		{
			void *mapped = GL_MapBufferRangeFunc (GL_PIXEL_PACK_BUFFER, 0, pbo_size, GL_MAP_READ_BIT);
			if (mapped)
			{
				memcpy (pixels, mapped, (size_t)pbo_size);
				GL_UnmapBufferFunc (GL_PIXEL_PACK_BUFFER);
				got_data = true;
			}
		}

		GL_BindBufferFunc (GL_PIXEL_PACK_BUFFER, 0);
		framebufs.autoexposure.pbo_index = read_index;
		framebufs.autoexposure.pbo_ready = true;
		glPixelStorei (GL_PACK_ALIGNMENT, prev_pack_alignment);

		if (!got_data)
		{
			GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
			glReadBuffer (GL_BACK);
			return false;
		}
	}
	else
	{
		if (r_autoexposure_async.value > 0.f && GL_AutoExposurePBOAvailable () && !framebufs.autoexposure.pbo_ready)
			GL_AutoExposureInitPBOs ();
		else if (r_autoexposure_async.value <= 0.f && framebufs.autoexposure.pbo_ready)
			GL_AutoExposureDeletePBOs ();

		glReadPixels (0, 0, width, height, GL_RGBA, GL_FLOAT, pixels);
		glPixelStorei (GL_PACK_ALIGNMENT, prev_pack_alignment);
	}
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
	glReadBuffer (GL_BACK);

	for (int i = 0; i < pixel_count; ++i)
	{
		const float r = pixels[i * 4 + 0];
		const float g = pixels[i * 4 + 1];
		const float b = pixels[i * 4 + 2];
		float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		lum = q_max (lum, 0.0001f);
		luminance_samples[i] = lum;
	}

	qsort (luminance_samples, pixel_count, sizeof (float), GL_CompareFloat);

	{
		const int low = (int)floorf (pixel_count * 0.05f);
		const int high = q_min (pixel_count - 1, (int)ceilf (pixel_count * 0.95f) - 1);
		const int count = high - low + 1;
		if (count <= 0)
			return false;

		double log_sum = 0.0;
		for (int i = low; i <= high; ++i)
			log_sum += logf (luminance_samples[i]);

		*out_luminance = expf ((float)(log_sum / (double)count));
	}

	return true;
}

static float GL_UpdateAutoExposure (void)
{
	static qboolean initialized = false;
	static double last_time = 0.0;
	static float current_exposure = 1.f;
	float scene_luminance = r_autoexposure_debug_luminance;

	if (!initialized)
	{
		current_exposure = 1.f;
		last_time = cl.time;
		initialized = true;
	}

	if (GL_SampleAutoExposureLuminance (&scene_luminance))
		r_autoexposure_debug_luminance = scene_luminance;

	if (r_autoexposure.value <= 0.f)
		return current_exposure;

	if (r_exposure_lock.value > 0.f)
		return current_exposure;

	if ((in_attack.state & 1) || cl.cshifts[CSHIFT_DAMAGE].percent > 0.f)
		return current_exposure;

	{
		const float min_scene_luma = q_max (0.f, r_ae_min_scene_luma.value);
		scene_luminance = q_max (scene_luminance, min_scene_luma);
		r_autoexposure_debug_luminance = scene_luminance;
	}

	if (scene_luminance <= 0.f)
		return current_exposure;

	{
		const float min_scene_luma = 0.001f;
		const float max_scene_luma = 0.01f;
		const float min_scene_log = log10f (min_scene_luma);
		const float max_scene_log = log10f (max_scene_luma);
		const float scene_log = log10f (q_max (scene_luminance, min_scene_luma));
		const float bias = q_max (0.f, r_exposure_bias.value);
		const float min_exposure = q_min (r_exposure_min.value, r_exposure_max.value);
		const float max_exposure = q_max (r_exposure_min.value, r_exposure_max.value);
		const float hard_min_exposure = q_max (0.f, q_min (r_ae_min_exposure.value, r_ae_max_exposure.value));
		const float hard_max_exposure = q_max (hard_min_exposure, q_max (r_ae_min_exposure.value, r_ae_max_exposure.value));
		float interpolation = (scene_log - min_scene_log) / (max_scene_log - min_scene_log);
		float target = LERP (hard_max_exposure, hard_min_exposure, interpolation) * bias;
		float speed_up = q_max (0.f, r_exposure_speed_up.value);
		float speed_down = q_max (0.f, r_exposure_speed_down.value);
		float adaptation_speed = (target > current_exposure) ? speed_up : speed_down;
		float delta = (float)(cl.time - last_time);
		float change;
		float max_delta;

		if (delta < 0.f)
			delta = 0.f;

		last_time = cl.time;

		target = CLAMP (hard_min_exposure, target, hard_max_exposure);
		target = CLAMP (min_exposure, target, max_exposure);
		target = CLAMP (hard_min_exposure, target, hard_max_exposure);
		change = (target - current_exposure) * delta * adaptation_speed;
		max_delta = current_exposure * 0.02f;
		change = CLAMP (-max_delta, change, max_delta);
		current_exposure = CLAMP (min_exposure, current_exposure + change, max_exposure);
		current_exposure = CLAMP (hard_min_exposure, current_exposure, hard_max_exposure);
	}

	return current_exposure;
}


static void GL_PostProcess_SetMediumScatterUniforms (const r_ssao_fog_state_t *fog_state)
{
	medium_scatter_source_t medium = { 0, 0.f };
	float fogvol_valid = 0.f;
	float transmittance_policy = (float)R_SSAO_FOG_TRANS_GLOBAL_ONLY;

	if (fog_state)
	{
		fogvol_valid = fog_state->fogvol_valid ? 1.f : 0.f;
		transmittance_policy = (float)fog_state->transmittance_policy;
	}

	/* Framegraph fog handoff captures whether FogVol composite is valid for this
	 * frame; only sample the medium texture when that handoff says it's ready. */
	if (fogvol_valid > 0.5f)
		medium = GL_GetMediumScatterSource ();

	GL_BindNative (GL_TEXTURE10, GL_TEXTURE_2D, medium.texture);
	GL_Uniform4fFunc (27, medium.valid, transmittance_policy, 0.f, 0.f);
}

static float GL_ComputeEffectiveBloomIntensity (float bloom_base, float bloom_boost)
{
	float base = q_max (0.f, bloom_base);
	float boost = q_max (0.f, bloom_boost);

	if (r_postfx_bloom_mode.value > 0.f)
		return q_max (base, boost);
	return base + boost;
}

static qboolean GL_PostFXBloomBoostActive (void)
{
	postfx_state_t state;

	if (r_postfx.value <= 0.f)
		return false;

	R_PostFX_GetState (&state);
	return state.bloom_boost > 0.f;
}

void GL_PostProcess (const RenderGraphResourceHandle *resources)
{
	int palidx, variant;
	float saturation;
	float dither;
	qboolean needs_postprocess;
	qboolean dof_enabled;
	float dof_focus, dof_range, dof_strength;
	float dof_znear, dof_zfar;
	qboolean motion_enabled;
	qboolean msaa;
	GLuint velocity_texture;
	GLuint depth_texture;
	GLuint godrays_texture;
	GLuint godrays_mask;
	GLuint godrays_source;
	GLuint ssao_texture;
	float motion_strength;
	float motion_shutter;
	float motion_effective_shutter;
	float motion_max_radius;
	float motion_min_velocity;
	float motion_depth_threshold;
	int motion_max_samples;
	float teleport_fade;
	float teleport_blur;
	qboolean godrays_enabled;
	qboolean godrays_debug_enabled;
	qboolean godrays_preview;
	float godrays_debug;
	float godrays_debug_source;
	float ssao_intensity;
	float ssao_debug_mode;
	float ssao_fog_strength;
	float ssao_fog_power;
	float view_min_x;
	float view_min_y;
	float view_max_x;
	float view_max_y;
	float inv_scale;
	qboolean scaled_scene = false;
	qboolean drs_postfx_guard = false;
	postfx_state_t postfx_state;
	float postfx_exposure_add;
	float postfx_bloom_boost;
	float postfx_emissive_boost;
	float postfx_desat;
	float postfx_vignette;
	float postfx_vignette_softness;
	float postfx_lut_strength;
	int postfx_lut_id;
	int postfx_lut_size;
	float fog_r;
	float fog_g;
	float fog_b;
	float postfx_damage_trauma;
	float dv_strength;
	float dv_max_px;
	float dv_freq;
	float dv_quality;
	float dv_debug;
	float dv_time;
	GLuint scene_fbo = (resources && resources->scene_fbo) ? resources->scene_fbo : framebufs.scene.fbo;
	GLuint scene_velocity_tex = (resources && resources->scene_velocity_tex) ? resources->scene_velocity_tex : framebufs.scene.velocity_tex;
	GLuint resolved_scene_velocity_tex = (resources && resources->resolved_scene_velocity_tex) ? resources->resolved_scene_velocity_tex : framebufs.resolved_scene.velocity_tex;
	GLuint composite_fbo = (resources && resources->composite_fbo) ? resources->composite_fbo : framebufs.composite.fbo;
	GLuint composite_color_tex = (resources && resources->composite_color_tex) ? resources->composite_color_tex : framebufs.composite.color_tex;
	GLuint composite_depth_tex = (resources && resources->composite_depth_tex) ? resources->composite_depth_tex : framebufs.composite.depth_stencil_tex;
	int scene_samples = (resources && resources->scene_samples > 0) ? resources->scene_samples : framebufs.scene.samples;
	saturation = CLAMP (0.9f, r_color_saturation.value, 1.2f);
	R_GetFramePlanDecisions (NULL, &needs_postprocess);
	if (!needs_postprocess)
		return;
	if (composite_fbo == 0 || composite_color_tex == 0)
		return;
	if (!framesetup.composite_ready)
	{
		GL_BeginGroup ("Postprocess backbuffer copy");
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, 0);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, composite_fbo);
		glReadBuffer (GL_BACK);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		GL_BlitFramebufferFunc (0, 0, R_GetNativeRenderWidth (), R_GetNativeRenderHeight (), 0, 0, R_GetNativeRenderWidth (), R_GetNativeRenderHeight (), GL_COLOR_BUFFER_BIT, GL_NEAREST);
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
		glDrawBuffer (GL_BACK);
		glReadBuffer (GL_BACK);
		framesetup.composite_ready = true;
		GL_EndGroup ();
	}

	GL_BeginGroup ("Postprocess");

	r_godrays_coupling_shafts_tex = 0;
	r_godrays_coupling_shafts_w = 0;
	r_godrays_coupling_shafts_h = 0;

	R_PostFX_GetState (&postfx_state);

	palidx = GLPalette_Postprocess ();
	dither = (softemu == SOFTEMU_FINE) ? NOISESCALE * r_dither.value * r_softemu_dither_screen.value : 0.f;

	postfx_exposure_add = CLAMP (-2.f, postfx_state.exposure_add_stops, 2.f);
	postfx_bloom_boost = q_max (0.f, postfx_state.bloom_boost);
	postfx_emissive_boost = q_max (0.f, postfx_state.emissive_boost);
	postfx_desat = q_min (1.f, q_max (0.f, postfx_state.desat));
	postfx_vignette = q_min (1.f, q_max (0.f, q_max (r_vignette.value, postfx_state.vignette)));
	postfx_vignette_softness = q_min (1.f, q_max (0.f, postfx_state.vignette_softness));
	postfx_lut_strength = q_min (1.f, q_max (0.f, postfx_state.lut_strength));
	postfx_lut_id = postfx_state.lut_id;
	postfx_lut_size = R_PostFX_GetLUTSize ();
	fog_r = postfx_state.underwater_fog_color[0];
	fog_g = postfx_state.underwater_fog_color[1];
	fog_b = postfx_state.underwater_fog_color[2];
	postfx_damage_trauma = CLAMP (0.f, postfx_state.damage_trauma, 1.f);

	float bloom_intensity = q_max (0.f, r_bloom.value);
	float bloom_intensity_effective = GL_ComputeEffectiveBloomIntensity (bloom_intensity, postfx_bloom_boost);
	float exposure = q_max (0.f, r_tonemap_exposure.value);
	float tonemap_mode = q_max (0.f, r_tonemap.value);
	teleport_fade = 0.f;
	teleport_blur = 0.f;
	{
		float teleport_duration = q_max (0.f, r_teleportfx_time.value);
		if (r_teleportfx.value > 0.f && teleport_duration > 0.f && cl.teleport_fx_time > 0.0)
		{
			double teleport_elapsed = cl.time - cl.teleport_fx_time;
			if (teleport_elapsed >= 0.0 && teleport_elapsed < teleport_duration)
			{
				teleport_fade = 1.f - (float)(teleport_elapsed / teleport_duration);
				teleport_blur = teleport_fade * 2.f;
			}
		}
	}
	GLuint bloom_texture = framebufs.bloom.extract_tex ? framebufs.bloom.extract_tex : 0;
	if (framebufs.bloom.pingpong_tex[0])
		bloom_texture = framebufs.bloom.pingpong_tex[0];
	if (bloom_intensity_effective > 0.f)
		bloom_texture = GL_GenerateBloomTexture ();

	if (r_autoexposure.value > 0.f || r_exposure_debug.value > 0.f)
	{
		float auto_exposure = GL_UpdateAutoExposure ();
		if (r_autoexposure.value > 0.f)
			exposure *= auto_exposure;
	}
	r_autoexposure_debug_exposure = exposure;
	bloom_intensity_effective = GL_ComputeEffectiveBloomIntensity (bloom_intensity, postfx_bloom_boost);

	if (r_postfx_lut_debug_id.value > 0.f)
	{
		int debug_id = (int)Q_rint (r_postfx_lut_debug_id.value);
		postfx_lut_id = CLAMP (0, debug_id, PFX_LUT_COUNT - 1);
		postfx_lut_strength = 1.f;
	}

	if (r_postfx_lut.value <= 0.f || postfx_lut_strength <= 0.f || postfx_lut_size <= 0)
	{
		postfx_lut_id = PFX_LUT_NONE;
		postfx_lut_strength = 0.f;
	}

	godrays_enabled = (!drs_postfx_guard
		&& r_godrays.value > 0.f
		&& R_GodraysMediumEnabled ()
		&& R_Godrays_IsReady (cl.worldmodel, r_framecount));
	godrays_debug = (r_godrays_debug.value > 0.f) ? 1.f : 0.f;
	godrays_debug_source = CLAMP (0.f, r_godrays_debug_source.value, 2.f);
	if (drs_postfx_guard)
	{
		godrays_debug = 0.f;
		godrays_debug_source = 0.f;
	}
	godrays_debug_enabled = (godrays_debug > 0.f || godrays_debug_source > 0.f);
	godrays_preview = (godrays_enabled || (godrays_debug_enabled && R_Godrays_IsReady (cl.worldmodel, r_framecount)));
	if (!godrays_preview)
	{
		godrays_debug = 0.f;
		godrays_debug_source = 0.f;
	}
	godrays_texture = 0;
	godrays_mask = 0;
	godrays_source = 0;
	if (godrays_preview)
	{
		R_EnsureGodraysTexturesForFrame (true);
		godrays_texture = r_godrays_cached_shafts;
		godrays_mask = r_godrays_cached_mask;
		godrays_source = r_godrays_cached_source;
	}

	view_min_x = (glx + r_refdef.vrect.x) / (float)R_GetNativeRenderWidth ();
	view_min_y = (gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height) / (float)R_GetNativeRenderHeight ();
	view_max_x = view_min_x + r_refdef.vrect.width / (float)R_GetNativeRenderWidth ();
	view_max_y = view_min_y + r_refdef.vrect.height / (float)R_GetNativeRenderHeight ();
	inv_scale = 1.f / (float)q_max (1, R_GetSceneRenderScale ());
	scaled_scene = (R_GetSceneRenderScale () != 1);
	drs_postfx_guard = scaled_scene || (r_drs.value > 0.f);

	ssao_texture = GL_GenerateSSAOTexture (view_min_x, view_min_y, view_max_x, view_max_y);
	/* Keep SSAO intensity aligned with the cvar's intended tuning range.
	 * Clamping to 1.0 made the default 1.5 weaker than expected and could make
	 * user configs look like SSAO had no effect when toggled on. */
	ssao_intensity = R_SSAO_SanitizeValue (r_ssao_intensity.value, 1.5f, 0.f, 8.f);
	{
		int debug_cvar = (int)Q_rint (r_ssao_debug.value);
		int debug_mode_i = (debug_cvar > 0) ? CLAMP (1, debug_cvar, 14) : -1;
		ssao_debug_mode = (debug_mode_i > 0) ? (float)debug_mode_i : -1.f;
	}
	if (ssao_texture == 0)
		ssao_debug_mode = -1.f;
	if (ssao_texture == 0 || r_ssao.value <= 0.f)
		ssao_intensity = 0.f;
	ssao_fog_strength = CLAMP (0.f, r_ssao_fog_strength.value, 1.f);
	ssao_fog_power = q_max (0.01f, r_ssao_fog_power.value);

	msaa = scene_samples > 1;
	motion_strength = q_max (0.f, r_motionblur.value);
	if (!GL_ShouldApplyMotionBlur ())
		motion_strength = 0.f;
	if (drs_postfx_guard)
		motion_strength = 0.f;
	motion_shutter = q_max (0.f, r_motionblur_shutter.value);
	motion_effective_shutter = motion_strength * motion_shutter;
	if (motion_effective_shutter > 0.f && r_prev_frame_valid)
	{
		double frame_delta = cl.time - r_prev_frame_time;
		if (frame_delta > 0.0)
		{
			const double reference_delta = 1.0 / 60.0;
			float frame_scale = (float)(reference_delta / frame_delta);
			frame_scale = q_min (4.f, q_max (1.f, frame_scale));
			motion_effective_shutter *= frame_scale;
		}
	}
	motion_max_radius = q_max (0.f, r_motionblur_maxradiuspixels.value);
	motion_min_velocity = q_max (0.f, r_motionblur_minvelocity.value);
	motion_depth_threshold = q_max (0.f, r_motionblur_depththreshold.value);
	motion_max_samples = (int)Q_rint (r_motionblur_maxsamples.value);
	if (motion_max_samples < 0)
		motion_max_samples = 0;
	if (motion_max_samples > 64)
		motion_max_samples = 64;
	velocity_texture = 0;
	if (scene_velocity_tex)
	{
		velocity_texture = msaa ? resolved_scene_velocity_tex : scene_velocity_tex;
		if (framesetup.scene_fbo != scene_fbo)
		{
			// The scene FBO was not used this frame, so the velocity attachment still holds stale data.
			velocity_texture = 0;
		}
	}
	motion_enabled = (motion_effective_shutter > 0.f && motion_max_samples > 0 && velocity_texture != 0);
	R_DebugDRSNativeEffects (
		bloom_intensity_effective > 0.f,
		ssao_texture != 0 && ssao_intensity > 0.f,
		R_FogVol_ShouldAffectPostFX (),
		godrays_texture != 0,
		motion_enabled);

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
	glViewport (glx, gly, glwidth, glheight);
	{
		int debug_mode = (int)Q_rint (CLAMP (0.f, r_debug_colorspace.value, 4.f));
		qboolean linear_debug = (debug_mode == 2);
		qboolean srgb_output = GL_UseSRGBFramebuffer () && !linear_debug;
		GL_SetFramebufferSRGB (srgb_output);
	}

	variant = q_min ((int)softemu, 2);
	if (!glprogs.postprocess[variant])
	{
		GL_PostProcessFallback ();
		GL_EndGroup ();
		return;
	}
	GL_UseProgram (glprogs.postprocess[variant]);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, composite_color_tex);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_3D, gl_palette_lut);
	GL_BindNative (GL_TEXTURE3, GL_TEXTURE_2D, bloom_texture);
	GL_BindNative (GL_TEXTURE4, GL_TEXTURE_2D, velocity_texture);
	GL_BindNative (GL_TEXTURE5, GL_TEXTURE_2D, godrays_texture);
	GL_BindNative (GL_TEXTURE6, GL_TEXTURE_2D, godrays_mask);
	GL_BindNative (GL_TEXTURE7, GL_TEXTURE_2D, godrays_source);
	GL_BindNative (GL_TEXTURE8, GL_TEXTURE_2D, ssao_texture);
	GL_BindNative (GL_TEXTURE9, GL_TEXTURE_2D_ARRAY, R_PostFX_GetLUTTexture ());
	/* Bind medium scatter/transmittance texture at slot 10 for SSAO suppression
	 * in postprocess.frag. The current implementation sources FogVol composite via
	 * GL_GetMediumScatterSource(), preserving validity gating and stale-handle safety. */
	GL_PostProcess_SetMediumScatterUniforms (&r_ssao_fog_state);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, gl_palette_buffer[palidx], 0, 256 * sizeof (GLuint));
	{
		float post_contrast = CLAMP (0.8f, r_color_contrast.value, 1.2f);
		GL_Uniform4fFunc (0, 1.f, post_contrast, 1.f / (float)q_max (1, R_GetSceneRenderScale ()), dither);
	}
	{
		int debug_mode = (int)Q_rint (CLAMP (0.f, r_debug_colorspace.value, 4.f));
		qboolean linear_debug = (debug_mode == 2);
		qboolean output_srgb = (r_srgb_framebuffer.value <= 0.f) || !vid_framebuffer_srgb_capable;
		GLint colorspace_params_loc = GL_GetUniformLocationFunc ? GL_GetUniformLocationFunc (glprogs.postprocess[variant], "ColorSpaceParams") : -1;
		if (linear_debug)
			output_srgb = false;
		if (colorspace_params_loc >= 0)
			GL_Uniform4fFunc (colorspace_params_loc, (float)debug_mode, 0.f, output_srgb ? 1.f : 0.f, 0.f);
	}
	GL_Uniform3fFunc (5, bloom_intensity, exposure, tonemap_mode);
	GL_Uniform4fFunc (6, motion_enabled ? 1.f : 0.f, motion_effective_shutter, motion_min_velocity, motion_depth_threshold);
	GL_Uniform4fFunc (7, motion_max_radius, (float)motion_max_samples, velocity_texture ? 1.f : 0.f, 0.f);
	GL_Uniform4fFunc (8,
		postfx_vignette,
		q_max (0.f, r_vignette_radius_inner.value),
		q_max (0.f, r_vignette_radius_outer.value),
		q_max (0.001f, r_vignette_falloff.value));
	GL_Uniform4fFunc (9,
		q_min (1.f, q_max (0.f, r_vignette_color_r.value)),
		q_min (1.f, q_max (0.f, r_vignette_color_g.value)),
		q_min (1.f, q_max (0.f, r_vignette_color_b.value)),
		q_min (2.f, q_max (0.f, r_vignette_blend_mode.value)));
	GL_Uniform4fFunc (10,
	q_min (0.1f, q_max (0.f, r_vignette_noise.value)),
	0.f,
	0.f,
	0.f);
	GL_Uniform4fFunc (11, teleport_fade, teleport_blur, 0.f, 0.f);
	GL_Uniform1fFunc (12, saturation);
	GL_Uniform1fFunc (20, q_max (0.1f, r_color_midtone.value));





	GL_Uniform4fFunc (21, postfx_exposure_add, postfx_bloom_boost, postfx_emissive_boost, postfx_desat);
	GL_Uniform4fFunc (22,
		postfx_lut_strength,
		postfx_state.underwater_postfx_active ? postfx_state.underwater_grade_strength : 0.f,
		postfx_state.underwater_postfx_active ? postfx_state.underwater_fog_strength : 0.f,
		postfx_vignette_softness);
	GL_Uniform4fFunc (23, (float)postfx_lut_size, (float)postfx_lut_id, 0.f, 0.f);
	/* Postprocess SSAO damping uses the cached fogvol global density instead of
	 * the removed analytic GL fog UBO channel. */
	GL_Uniform4fFunc (24, fog_r, fog_g, fog_b, r_ssao_fog_state.density);
	{
		int quality = (int)Q_rint (r_post_damage_dv_quality.value);
		dv_quality = (float)CLAMP (0, quality, 2);
		dv_strength = q_max (0.f, r_post_damage_dv_strength.value);
		dv_max_px = q_max (0.f, r_post_damage_dv_px.value);
		dv_freq = q_max (0.f, r_post_damage_dv_freq.value);
		dv_debug = (r_post_damage_dv_debug.value > 0.f) ? 1.f : 0.f;
		if (r_postfx.value <= 0.f || r_post_damage_doublevision.value <= 0.f || dv_quality <= 0.f)
			postfx_damage_trauma = 0.f;
		dv_time = (float)cl.time;
		GL_Uniform4fFunc (25, postfx_damage_trauma, dv_strength, dv_max_px, dv_freq);
		GL_Uniform4fFunc (26, dv_time, dv_quality, dv_debug, 0.f);
	}
	{
		GLint godrays_params_loc = GL_GetUniformLocationFunc ? GL_GetUniformLocationFunc (glprogs.postprocess[variant], "GodraysParams") : -1;
		if (godrays_params_loc >= 0)
			GL_Uniform4fFunc (godrays_params_loc, godrays_texture ? 1.f : 0.f, godrays_debug, godrays_debug_source, 0.f);
	}
	{
		float upscale_nearest = (r_ssao_upscale_nearest.value > 0.f) ? 1.f : 0.f;
		GL_Uniform4fFunc (17, ssao_intensity, ssao_debug_mode, upscale_nearest, ssao_fog_strength);
		{
			int blur_radius = (int)Q_rint (r_ssao_blur_radius.value);
			float blur_sigma = q_max (0.01f, r_ssao_blur_sigma.value);
			float depth_threshold_scale = 0.02f;
			blur_radius = CLAMP (1, blur_radius, 4);
			GL_Uniform4fFunc (18, blur_sigma, (float)blur_radius, depth_threshold_scale, ssao_fog_power);
		}
	}
	dof_enabled = R_DoFEnabled () && !drs_postfx_guard;

	{
		GL_Uniform4fFunc (3, view_min_x, view_min_y, view_max_x, view_max_y);
		GL_Uniform4fFunc (4, 1.f, 1.f, 0.f, 0.f);
	}

	depth_texture = 0;
	{
		qboolean ssao_needs_depth = (ssao_texture != 0
			&& (r_ssao_halfres.value > 0.f || ssao_debug_mode == 8.f || ssao_fog_strength > 0.f));
		if (composite_depth_tex && (dof_enabled || (motion_enabled && motion_depth_threshold > 0.f) || ssao_needs_depth))
			depth_texture = composite_depth_tex;
	}
	GL_BindNative (GL_TEXTURE2, GL_TEXTURE_2D, depth_texture);

	if (dof_enabled)
	{
		dof_focus = q_max (0.f, r_dof_focus.value);
		dof_focus = R_GetDynamicDoFFocus (dof_focus);
		dof_range = q_max (0.f, r_dof_range.value);
		dof_strength = q_max (0.f, r_dof_strength.value);
		dof_znear = (view_znear > 0.f) ? view_znear : 0.5f;
		dof_zfar = (view_zfar > dof_znear) ? view_zfar : dof_znear + 1.f;
		GL_Uniform4fFunc (1, 1.f, dof_focus, dof_range, dof_strength);
		GL_Uniform4fFunc (2, dof_znear, dof_zfar, gl_clipcontrol_able ? 1.f : 0.f, 0.f);
	}
	else
	{
		r_dof_autofocus_initialized = false;
		dof_znear = (view_znear > 0.f) ? view_znear : 0.5f;
		dof_zfar = (view_zfar > dof_znear) ? view_zfar : dof_znear + 1.f;
		GL_Uniform4fFunc (1, 0.f, 0.f, 0.f, 0.f);
		GL_Uniform4fFunc (2, dof_znear, dof_zfar, gl_clipcontrol_able ? 1.f : 0.f, 0.f);
	}

	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}


/*
=================
R_CullBox -- johnfitz -- replaced with new function from lordhavoc

Returns true if the box is completely outside the frustum.

Uses the bounding-box center/extents formulation so the expensive corner
selection only happens once per box.
=================
*/
qboolean R_CullBox (vec3_t emins, vec3_t emaxs)
{
	vec3_t center;
	vec3_t extents;
	int i;

	center[0] = (emins[0] + emaxs[0]) * 0.5f;
	center[1] = (emins[1] + emaxs[1]) * 0.5f;
	center[2] = (emins[2] + emaxs[2]) * 0.5f;
	extents[0] = (emaxs[0] - emins[0]) * 0.5f;
	extents[1] = (emaxs[1] - emins[1]) * 0.5f;
	extents[2] = (emaxs[2] - emins[2]) * 0.5f;

	for (i = 0; i < 4; i++)
	{
		const mplane_t *plane = &frustum[i];
		const float *absnormal = frustum_absnormal[i];
		float signed_distance = DotProduct (plane->normal, center) - plane->dist;
		float radius = absnormal[0] * extents[0] + absnormal[1] * extents[1] + absnormal[2] * extents[2];

		if (signed_distance < -radius)
		return true;
	}

	return false;
}

/*
===============
R_GetEntityBounds -- johnfitz -- uses correct bounds based on rotation

PERF OPT: Avoid redundant pointer dereferences
===============
*/
void R_GetEntityBounds (const entity_t* e, vec3_t mins, vec3_t maxs)
{
	vec_t scalefactor;
	const vec_t* minbounds, * maxbounds;
	const vec3_t* origin = &e->origin;
	const vec3_t* angles = &e->angles;

	// PERF OPT: Cache model pointer
	const qmodel_t* model = e->model;

	if ((*angles)[0] || (*angles)[2]) //pitch or roll
	{
		minbounds = model->rmins;
		maxbounds = model->rmaxs;
	}
	else if ((*angles)[1]) //yaw
	{
		minbounds = model->ymins;
		maxbounds = model->ymaxs;
	}
	else //no rotation
	{
		minbounds = model->mins;
		maxbounds = model->maxs;
	}

	scalefactor = ENTSCALE_DECODE (e->scale);
	if (scalefactor != 1.0f)
	{
		// PERF OPT: Manual VectorMA for better optimization
		mins[0] = (*origin)[0] + minbounds[0] * scalefactor;
		mins[1] = (*origin)[1] + minbounds[1] * scalefactor;
		mins[2] = (*origin)[2] + minbounds[2] * scalefactor;
		maxs[0] = (*origin)[0] + maxbounds[0] * scalefactor;
		maxs[1] = (*origin)[1] + maxbounds[1] * scalefactor;
		maxs[2] = (*origin)[2] + maxbounds[2] * scalefactor;
	}
	else
	{
		// PERF OPT: Manual VectorAdd for better optimization
		mins[0] = (*origin)[0] + minbounds[0];
		mins[1] = (*origin)[1] + minbounds[1];
		mins[2] = (*origin)[2] + minbounds[2];
		maxs[0] = (*origin)[0] + maxbounds[0];
		maxs[1] = (*origin)[1] + maxbounds[1];
		maxs[2] = (*origin)[2] + maxbounds[2];
	}
}

/*
===============
R_CullModelForEntity -- johnfitz -- uses correct bounds based on rotation
===============
*/
qboolean R_CullModelForEntity (entity_t* e)
{
	vec3_t mins, maxs;

	R_GetEntityBounds (e, mins, maxs);

	return R_CullBox (mins, maxs);
}

/*
===============
R_EntityMatrix

PERF OPT: Calculate sin/cos only once, reuse for both branches
===============
*/
void R_EntityMatrix (float matrix[16], vec3_t origin, vec3_t angles, unsigned char scale)
{
	float scalefactor = ENTSCALE_DECODE (scale);
	float yaw = DEG2RAD (angles[YAW]);
	float pitch = angles[PITCH];
	float roll = angles[ROLL];

	// PERF OPT: Calculate sin/cos for yaw once
	float sy = sin (yaw);
	float cy = cos (yaw);

	if (pitch == 0.f && roll == 0.f)
	{
		// PERF OPT: Reuse pre-calculated sin/cos
		sy *= scalefactor;
		cy *= scalefactor;

		// First column
		matrix[0] = cy;
		matrix[1] = sy;
		matrix[2] = 0.f;
		matrix[3] = 0.f;

		// Second column
		matrix[4] = -sy;
		matrix[5] = cy;
		matrix[6] = 0.f;
		matrix[7] = 0.f;

		// Third column
		matrix[8] = 0.f;
		matrix[9] = 0.f;
		matrix[10] = scalefactor;
		matrix[11] = 0.f;
	}
	else
	{
		float sp, sr, cp, cr;
		pitch = DEG2RAD (pitch);
		roll = DEG2RAD (roll);
		// PERF OPT: sy and cy already calculated above
		sp = sin (pitch);
		sr = sin (roll);
		cp = cos (pitch);
		cr = cos (roll);

		// https://www.symbolab.com/solver/matrix-multiply-calculator FTW!

		// First column
		matrix[0] = scalefactor * cy * cp;
		matrix[1] = scalefactor * sy * cp;
		matrix[2] = scalefactor * sp;
		matrix[3] = 0.f;

		// Second column
		matrix[4] = scalefactor * (-cy * sp * sr - cr * sy);
		matrix[5] = scalefactor * (cr * cy - sy * sp * sr);
		matrix[6] = scalefactor * cp * sr;
		matrix[7] = 0.f;

		// Third column
		matrix[8] = scalefactor * (sy * sr - cr * cy * sp);
		matrix[9] = scalefactor * (-cy * sr - cr * sy * sp);
		matrix[10] = scalefactor * cr * cp;
		matrix[11] = 0.f;
	}

	// Fourth column
	matrix[12] = origin[0];
	matrix[13] = origin[1];
	matrix[14] = origin[2];
	matrix[15] = 1.f;
}

/*
=============
GL_PolygonOffset -- johnfitz

negative offset moves polygon closer to camera
=============
*/
void GL_PolygonOffset (int offset)
{
	if (gl_clipcontrol_able)
		offset = -offset;

	if (offset > 0)
	{
		glEnable (GL_POLYGON_OFFSET_FILL);
		glEnable (GL_POLYGON_OFFSET_LINE);
		glPolygonOffset (1, offset);
	}
	else if (offset < 0)
	{
		glEnable (GL_POLYGON_OFFSET_FILL);
		glEnable (GL_POLYGON_OFFSET_LINE);
		glPolygonOffset (-1, offset);
	}
	else
	{
		glDisable (GL_POLYGON_OFFSET_FILL);
		glDisable (GL_POLYGON_OFFSET_LINE);
	}
}

/*
=============
GL_DepthRange

Wrapper around glDepthRange that handles clip control/reversed Z differences
=============
*/
void GL_DepthRange (zrange_t range)
{
	switch (range)
	{
	default:
	case ZRANGE_FULL:
		glDepthRange (0.f, 1.f);
		break;

	case ZRANGE_VIEWMODEL:
		if (gl_clipcontrol_able)
			glDepthRange (0.7f, 1.f);
		else
			glDepthRange (0.f, 0.3f);
		break;

	case ZRANGE_NEAR:
		if (gl_clipcontrol_able)
			glDepthRange (1.f, 1.f);
		else
			glDepthRange (0.f, 0.f);
		break;
	}
}

/*
=============
R_GetAlphaMode
=============
*/
alphamode_t R_GetAlphaMode (void)
{
	if (r_oit.value)
		return ALPHAMODE_OIT;
	return r_alphasort.value ? ALPHAMODE_SORTED : ALPHAMODE_BASIC;
}

/*
=============
R_GetEffectiveAlphaMode
=============
*/
alphamode_t R_GetEffectiveAlphaMode (void)
{
	if (map_checks.value)
		return ALPHAMODE_BASIC;
	return R_GetAlphaMode ();
}

/*
=============
R_SetAlphaMode
=============
*/
void R_SetAlphaMode (alphamode_t mode)
{
	Cvar_SetValueQuick (&r_oit, mode == ALPHAMODE_OIT);
	if (mode != ALPHAMODE_OIT)
		Cvar_SetValueQuick (&r_alphasort, mode == ALPHAMODE_SORTED);
}


//==============================================================================
//
// SETUP FRAME
//
//==============================================================================

static uint32_t visedict_keys[MAX_VISEDICTS];
static uint16_t visedict_order[2][MAX_VISEDICTS];

/*
=============
R_SortEntities

PERF OPT: Optimized entity filtering and sorting
=============
*/
static void R_SortEntities (void)
{
	int i, j, pass;
	int bins[1 << (MODSORT_BITS / 2)];
	int typebins[mod_numtypes * 2];
	alphamode_t alphamode = R_GetEffectiveAlphaMode ();
	cl_numshadowedicts = 0;

	if (!r_drawentities.value)
		cl_numvisedicts = 0;

	// PERF OPT: Combined entity filtering - remove entities with no/invisible models
	// and apply brush culling in one pass
	for (i = 0, j = 0; i < cl_numvisedicts; i++)
	{
		entity_t* ent = cl_visedicts[i];

		// PERF OPT: Early-out conditions grouped together
		if (!ent->model || ent->alpha == ENTALPHA_ZERO)
			continue;
		if (cl_numshadowedicts < MAX_VISEDICTS)
			cl_shadow_visedicts[cl_numshadowedicts++] = ent;

		// PERF OPT: Only cull brush models (most common case first)
		if (ent->model->type == mod_brush)
		{
			if (R_CullModelForEntity (ent))
				continue;
		}

		cl_visedicts[j++] = ent;
	}
	cl_numvisedicts = j;

	memset (typebins, 0, sizeof (typebins));
	if (r_drawworld.value)
		typebins[mod_brush * 2 + 0]++; // count worldspawn

	// fill entity sort key array, initial order, and per-type counts
	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t* ent = cl_visedicts[i];
		qboolean translucent = !ENTALPHA_OPAQUE (ent->alpha);

		if (translucent && alphamode == ALPHAMODE_SORTED)
		{
			float dist, delta;
			vec3_t mins, maxs;

			R_GetEntityBounds (ent, mins, maxs);

			// PERF OPT: Unrolled distance calculation
			dist = 0.f;
			delta = CLAMP (mins[0], r_refdef.vieworg[0], maxs[0]) - r_refdef.vieworg[0];
			dist += delta * delta;
			delta = CLAMP (mins[1], r_refdef.vieworg[1], maxs[1]) - r_refdef.vieworg[1];
			dist += delta * delta;
			delta = CLAMP (mins[2], r_refdef.vieworg[2], maxs[2]) - r_refdef.vieworg[2];
			dist += delta * delta;

			dist = sqrt (dist);
			visedict_keys[i] = ~CLAMP (0, (int)dist, MODSORT_MASK);
		}
		else if (translucent && alphamode != ALPHAMODE_OIT)
		{
			// Note: -1 (0xfffff) for non-static entities (firstleaf=0),
			// so they are sorted after static ones
			visedict_keys[i] = ent->firstleaf - 1;
		}
		else
		{
			// PERF OPT: Branch prediction hint - alias is most common
			if (ent->model->type == mod_alias)
				visedict_keys[i] = ent->model->sortkey | (ent->skinnum & MODSORT_FRAMEMASK);
			else
				visedict_keys[i] = ent->model->sortkey | (ent->frame & MODSORT_FRAMEMASK);
		}

		if ((unsigned)ent->model->type >= (unsigned)mod_numtypes)
			Sys_Error ("Model '%s' has invalid type %d", ent->model->name, ent->model->type);
		typebins[ent->model->type * 2 + translucent]++;

		visedict_order[0][i] = i;
	}

	// convert typebin counts into offsets
	for (i = 0, j = 0; i < countof (typebins); i++)
	{
		int tmp = typebins[i];
		cl_modtype_ofs[i] = typebins[i] = j;
		j += tmp;
	}
	cl_modtype_ofs[i] = j;

	// LSD-first radix sort: 2 passes x MODSORT_BITS/2 bits
	for (pass = 0; pass < 2; pass++)
	{
		uint16_t* src = visedict_order[pass];
		uint16_t* dst = visedict_order[pass ^ 1];
		const int mask = countof (bins) - 1;
		int shift = pass * (MODSORT_BITS / 2);
		int sum;

		// count number of entries in each bin
		memset (bins, 0, sizeof (bins));
		for (i = 0; i < cl_numvisedicts; i++)
			bins[(visedict_keys[i] >> shift) & mask]++;

		// turn bin counts into offsets
		sum = 0;
		for (i = 0; i < countof (bins); i++)
		{
			int tmp = bins[i];
			bins[i] = sum;
			sum += tmp;
		}

		// reorder
		for (i = 0; i < cl_numvisedicts; i++)
			dst[bins[(visedict_keys[src[i]] >> shift) & mask]++] = src[i];
	}

	// write sorted list
	if (r_drawworld.value)
		cl_sorted_visedicts[typebins[mod_brush * 2 + 0]++] = &cl_entities[0]; // add the world as the first brush entity
	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t* ent = cl_visedicts[visedict_order[0][i]];
		qboolean translucent = !ENTALPHA_OPAQUE (ent->alpha);
		cl_sorted_visedicts[typebins[ent->model->type * 2 + translucent]++] = ent;
	}
}

int SignbitsForPlane (mplane_t* out)
{
	int	bits, j;

	// for fast box on planeside test

	bits = 0;
	for (j = 0; j < 3; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1 << j;
	}
	return bits;
}

/*
=============
GL_FrustumMatrix
=============
*/
static void GL_FrustumMatrix (float matrix[16], float fovx, float fovy, float n, float f)
{
	const float w = 1.0f / tanf (fovx * 0.5f);
	const float h = 1.0f / tanf (fovy * 0.5f);

	memset (matrix, 0, 16 * sizeof (float));

	if (gl_clipcontrol_able)
	{
		// reversed Z projection matrix with the coordinate system conversion baked in
		matrix[0 * 4 + 2] = -n / (f - n);
		matrix[0 * 4 + 3] = 1.f;
		matrix[1 * 4 + 0] = -w;
		matrix[2 * 4 + 1] = h;
		matrix[3 * 4 + 2] = f * n / (f - n);
	}
	else
	{
		// standard projection matrix with the coordinate system conversion baked in
		matrix[0 * 4 + 2] = (f + n) / (f - n);
		matrix[0 * 4 + 3] = 1.f;
		matrix[1 * 4 + 0] = -w;
		matrix[2 * 4 + 1] = h;
		matrix[3 * 4 + 2] = -2.f * f * n / (f - n);
	}
}

/*
===============
ExtractFrustumPlane

Extracts the normalized frustum plane from the given view-projection matrix
that corresponds to a value of 'ndcval' on the 'axis' axis in NDC space.
===============
*/
static void ExtractFrustumPlane (float mvp[16], int axis, float ndcval, qboolean flip, mplane_t* out)
{
	float scale;
	out->normal[0] = (mvp[0 * 4 + axis] - ndcval * mvp[0 * 4 + 3]);
	out->normal[1] = (mvp[1 * 4 + axis] - ndcval * mvp[1 * 4 + 3]);
	out->normal[2] = (mvp[2 * 4 + axis] - ndcval * mvp[2 * 4 + 3]);
	out->dist = -(mvp[3 * 4 + axis] - ndcval * mvp[3 * 4 + 3]);

	scale = (flip ? -1.f : 1.f) / sqrtf (DotProduct (out->normal, out->normal));
	out->normal[0] *= scale;
	out->normal[1] *= scale;
	out->normal[2] *= scale;
	out->dist *= scale;

	out->type = PLANE_ANYZ;
	out->signbits = SignbitsForPlane (out);
}

/*
===============
R_SetFrustum
===============
*/
void R_SetFrustum (void)
{
	static qboolean warned_invalid_farclip = false;
	float w, h, d;
	float znear, zfar;
	float logznear, logzfar;
	float logrange;
	float translation[16];
	float rotation[16];
	int i;

	// reduce near clip distance at high FOV's to avoid seeing through walls
	w = 1.f / tanf (DEG2RAD (r_fovx) * 0.5f);
	h = 1.f / tanf (DEG2RAD (r_fovy) * 0.5f);
	d = 12.f * q_min (w, h);
	znear = CLAMP (0.5f, d, 4.f);
	zfar = gl_farclip.value;
	if (zfar <= znear || zfar <= 0.f)
	{
		const float sanitized_zfar = q_max (znear + 1.f, 1.f);

		if (!warned_invalid_farclip)
		{
			Con_DPrintf ("gl_farclip %0.4f is invalid for znear %0.4f; clamping to %0.4f\n",
				zfar, znear, sanitized_zfar);
			warned_invalid_farclip = true;
		}

		zfar = sanitized_zfar;
	}

	view_znear = znear;
	view_zfar = zfar;

	GL_FrustumMatrix (r_matproj, DEG2RAD (r_fovx), DEG2RAD (r_fovy), znear, zfar);
	if (!Mat4_Inverse (r_matproj, r_matinvproj))
		memcpy (r_matinvproj, r_identity_mat4, sizeof (r_identity_mat4));

	// View matrix
	RotationMatrix (r_matview, DEG2RAD (-r_refdef.viewangles[ROLL]), 0);
	RotationMatrix (rotation, DEG2RAD (-r_refdef.viewangles[PITCH]), 1);
	MatrixMultiply (r_matview, rotation);
	RotationMatrix (rotation, DEG2RAD (-r_refdef.viewangles[YAW]), 2);
	MatrixMultiply (r_matview, rotation);

	TranslationMatrix (translation, -r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);
	MatrixMultiply (r_matview, translation);

	// View projection matrix
	memcpy (r_matviewproj, r_matproj, 16 * sizeof (float));
	MatrixMultiply (r_matviewproj, r_matview);

	ExtractFrustumPlane (r_matviewproj, 0, 1.f, true, &frustum[0]); // right
	ExtractFrustumPlane (r_matviewproj, 0, -1.f, false, &frustum[1]); // left
	ExtractFrustumPlane (r_matviewproj, 1, -1.f, false, &frustum[2]); // bottom
	ExtractFrustumPlane (r_matviewproj, 1, 1.f, true, &frustum[3]); // top

	for (i = 0; i < 4; i++)
	{
		frustum_absnormal[i][0] = fabsf (frustum[i].normal[0]);
		frustum_absnormal[i][1] = fabsf (frustum[i].normal[1]);
		frustum_absnormal[i][2] = fabsf (frustum[i].normal[2]);
	}

	logznear = log2f (znear);
	logzfar = log2f (zfar);
	logrange = logzfar - logznear;
	if (fabsf (logrange) < 1e-6f)
		logrange = (logrange < 0.f) ? -1e-6f : 1e-6f;

	memcpy (r_framedata.viewproj, r_matviewproj, 16 * sizeof (float));
	memcpy (r_framedata.view, r_matview, 16 * sizeof (float));
	r_framedata.zparams[0] = LIGHT_TILES_Z / logrange;
	r_framedata.zparams[1] = -r_framedata.zparams[0] * logznear;
}

/*
=============
GL_NeedsSceneEffects
=============
*/

static qboolean GL_NeedsPostprocess_Internal (qboolean include_fogvol)
{
	float saturation;
	qboolean godrays_medium;
	qboolean fogvol_requested = R_FogVol_IsEnabledForFrame ();

	saturation = CLAMP (0.9f, r_color_saturation.value, 1.2f);
	if (softemu || R_GetEffectiveAlphaMode () == ALPHAMODE_OIT || R_DoFEnabled ())
		return true;
	if (r_debug_colorspace.value > 0.f)
		return true;
	if (r_lighting_debug_view.value > 0.f)
		return true;
	if (r_tonemap.value > 0.f || r_bloom.value > 0.f || r_color_contrast.value != 1.f || saturation != 1.f || r_color_midtone.value != 1.f || GL_ShouldApplyMotionBlur ())
		return true;
	if (r_srgb_framebuffer.value <= 0.f)
		return true;
	if (r_ssao.value > 0.f)
		return true;
	godrays_medium = R_GodraysMediumEnabled ();
	if (r_godrays.value > 0.f && godrays_medium)
		return true;
	if (include_fogvol && fogvol_requested)
		return true;
	return false;
}

qboolean GL_NeedsSceneEffects (void)
{
	qboolean fogvol_active = R_FogVol_IsEnabledForFrame ();

	if (framebufs.scene.samples > 1 || water_warp || R_GetSceneRenderScale () != 1)
		return true;

	/* Bloom enabled: keep scene-effects path active for a full-frame bloom extract/composite pass. */
	if (r_bloom.value > 0.f || GL_PostFXBloomBoostActive ())
		return true;

        if (GL_ShouldApplyMotionBlur ())
		return true;

        if (R_DoFEnabled ())
		return true;

	if (fogvol_active)
		return true;

	return false;
}

/*
=============
GL_NeedsPostprocess
=============
*/
qboolean GL_NeedsPostprocess (void)
{
	return GL_NeedsPostprocess_Internal (true);
}

static void R_GetFramePlanDecisions (qboolean *out_needs_scene_effects, qboolean *out_needs_postprocess)
{
	RenderFramePlan plan;
	qboolean needs_scene_effects;
	qboolean needs_postprocess;

	if (R_FrameGraph_GetRenderFramePlan (&plan))
	{
		needs_scene_effects = plan.needs_scene_effects;
		needs_postprocess = plan.needs_postprocess;
	}
	else
	{
		needs_scene_effects = GL_NeedsSceneEffects ();
		needs_postprocess = GL_NeedsPostprocess ();
	}

	if (out_needs_scene_effects)
		*out_needs_scene_effects = needs_scene_effects;
	if (out_needs_postprocess)
		*out_needs_postprocess = needs_postprocess;
}

static int R_GetDesiredSceneSampleCount (void)
{
	int desired = Q_nextPow2 ((int)q_max (1.f, vid_fsaa.value));
	int max_samples = framebufs.max_samples > 0 ? framebufs.max_samples : 1;

	return CLAMP (1, desired, max_samples);
}

static void R_EnsureRenderTargetSampleState (void)
{
	int desired_samples = R_GetDesiredSceneSampleCount ();
	int current_samples = framebufs.scene.samples > 0 ? framebufs.scene.samples : 1;
	int desired_scene_w = R_GetSceneRenderWidth ();
	int desired_scene_h = R_GetSceneRenderHeight ();
	int current_scene_w = framebufs.scene.width > 0 ? framebufs.scene.width : desired_scene_w;
	int current_scene_h = framebufs.scene.height > 0 ? framebufs.scene.height : desired_scene_h;
	int desired_fog_w = (r_fogvol_halfres.value > 0.f) ? q_max (1, R_GetNativeRenderWidth () / 2) : R_GetNativeRenderWidth ();
	int desired_fog_h = (r_fogvol_halfres.value > 0.f) ? q_max (1, R_GetNativeRenderHeight () / 2) : R_GetNativeRenderHeight ();
	int current_fog_w = framebufs.fogvol.width > 0 ? framebufs.fogvol.width : desired_fog_w;
	int current_fog_h = framebufs.fogvol.height > 0 ? framebufs.fogvol.height : desired_fog_h;
	qboolean sample_changed = (current_samples != desired_samples);
	qboolean size_changed = (current_scene_w != desired_scene_w || current_scene_h != desired_scene_h);
	qboolean fog_size_changed = (current_fog_w != desired_fog_w || current_fog_h != desired_fog_h);

	if (!sample_changed && !size_changed && !fog_size_changed)
		return;

	Con_DPrintf ("Recreating render targets (scene %dx%d -> %dx%d, fog %dx%d -> %dx%d, samples %d -> %d)\n",
		current_scene_w, current_scene_h, desired_scene_w, desired_scene_h,
		current_fog_w, current_fog_h, desired_fog_w, desired_fog_h,
		current_samples, desired_samples);
	GL_DeleteFrameBuffers ();
	GL_CreateFrameBuffers ();
	R_InvalidateTemporalHistoryOnSceneResize ();
	R_FrameGraph_SetRenderFramePlan (NULL);
}

/*
====================================================================================================
COLOR-SPACE POLICY (HYBRID LINEAR)

1) Lighting/compositing happens in linear HDR (RGBA16F targets).
2) Albedo/diffuse textures are uploaded as sRGB. Non-color data (normals/depth/noise/LUTs/masks)
   stays linear/UNORM.
3) Lightmaps are controlled by r_lightmap_colorspace (srgb|linear). "srgb" assumes gamma-encoded
   bakes and decodes on sampling; "linear" bypasses conversion.
4) UI/HUD/2D is mixed AFTER tone mapping in LDR space (postprocess output).
5) Exactly one output transform: postprocess applies the Quake curve, then performs linear->sRGB
   conversion if the backbuffer is not sRGB-capable.
====================================================================================================
*/

/*
=============
R_SetupGL
=============
*/
void R_SetupGL (void)
{
	qboolean needs_scene_effects = false;
	qboolean needs_postprocess = false;
	int scene_width = R_GetSceneRenderWidth ();
	int scene_height = R_GetSceneRenderHeight ();

	R_GetFramePlanDecisions (&needs_scene_effects, &needs_postprocess);

	if (!needs_scene_effects)
	{
		GLuint target = needs_postprocess ? framebufs.composite.fbo : 0u;
		qboolean srgb_output = (target == 0u) && GL_UseSRGBFramebuffer ();

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, target);
		GL_SetFramebufferSRGB (srgb_output);
		framesetup.scene_fbo = framebufs.composite.fbo;
		framesetup.oit_fbo = framebufs.oit.fbo_composite;
		framesetup.composite_ready = (target == framebufs.composite.fbo);
		if (target)
		{
			glDrawBuffer (GL_COLOR_ATTACHMENT0);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
		}
		else
		{
			glDrawBuffer (GL_BACK);
			glReadBuffer (GL_BACK);
		}
		glViewport (glx + r_refdef.vrect.x, gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height, r_refdef.vrect.width, r_refdef.vrect.height);
	}
	else
	{
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.scene.fbo);
		GL_SetFramebufferSRGB (false);
		framesetup.scene_fbo = framebufs.scene.fbo;
		framesetup.oit_fbo = framebufs.oit.fbo_scene;
		framesetup.composite_ready = false;
		if (framebufs.scene.velocity_tex)
		{
			GLuint buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			GL_DrawBuffersFunc (2, buffers);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
		}
		else
		{
			glDrawBuffer (GL_COLOR_ATTACHMENT0);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
		}
		glViewport (0, 0, scene_width, scene_height);
	}
}

/*
=============
R_Clear -- johnfitz -- rewritten and gutted
=============
*/

void R_Clear (void)
{
	GLbitfield clearbits = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
	if (gl_clear.value)
		clearbits |= GL_COLOR_BUFFER_BIT;

	GL_SetState (glstate & ~GLS_NO_ZWRITE); // make sure depth writes are enabled
	glStencilMask (~0u);
	glClear (clearbits);
}

/*
===============
R_SetupScene -- johnfitz -- this is the stuff that needs to be done once per eye in stereo mode
===============
*/
void R_SetupScene (void)
{
	R_SetupGL ();
}

/*
===============
R_UploadFrameData
===============
*/
void R_UploadFrameData (void)
{
	GLuint	buf;
	GLbyte* ofs;
	size_t	size;

	size = sizeof (r_lightbuffer.lightstyles) + sizeof (r_lightbuffer.lights[0]) * q_max (r_framedata.numlights, 1); // avoid zero-length array
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &r_lightbuffer, size, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, buf, (GLintptr)ofs, size);

	GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 0, buf, (GLintptr)ofs, sizeof (r_framedata));
}

void R_GLStateDump (const char *tag)
{
	GLint program = 0;
	GLint active_texture = 0;
	GLint viewport[4] = {0};
	GLint scissor_box[4] = {0};
	GLint draw_fbo = 0;
	GLint read_fbo = 0;
	GLint prev_active_texture = 0;
	GLint ubo0 = 0, ubo1 = 0, ubo2 = 0;
	GLint tex2d_0 = 0, tex2d_1 = 0, tex2d_2 = 0;
	GLboolean blend = glIsEnabled (GL_BLEND);
	GLboolean depth_test = glIsEnabled (GL_DEPTH_TEST);
	GLboolean cull = glIsEnabled (GL_CULL_FACE);
	GLboolean scissor = glIsEnabled (GL_SCISSOR_TEST);
	GLboolean srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	GLboolean depth_mask = GL_TRUE;

	if (r_gl_state_validate.value <= 0.f)
		return;

	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &active_texture);
	glGetIntegerv (GL_VIEWPORT, viewport);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &depth_mask);
	prev_active_texture = active_texture;

	GL_GetIntegeri_vFunc (GL_UNIFORM_BUFFER_BINDING, 0, &ubo0);
	GL_GetIntegeri_vFunc (GL_UNIFORM_BUFFER_BINDING, 1, &ubo1);
	GL_GetIntegeri_vFunc (GL_UNIFORM_BUFFER_BINDING, 2, &ubo2);

	GL_ActiveTextureFunc (GL_TEXTURE0);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d_0);
	GL_ActiveTextureFunc (GL_TEXTURE1);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d_1);
	GL_ActiveTextureFunc (GL_TEXTURE2);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d_2);
	GL_ActiveTextureFunc (prev_active_texture);

	Con_Printf ("GL_STATE[%s] prog=%d acttex=%d fbo(draw/read)=%d/%d vp=(%d %d %d %d) sci=%d box=(%d %d %d %d) blend=%d depth=%d depthmask=%d cull=%d srgb=%d ubo(0/1/2)=%d/%d/%d tex2d(0/1/2)=%d/%d/%d\n",
		tag,
		program,
		active_texture - GL_TEXTURE0,
		draw_fbo,
		read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		blend,
		depth_test,
		depth_mask,
		cull,
		srgb,
		ubo0, ubo1, ubo2,
		tex2d_0, tex2d_1, tex2d_2);
}

/*
===============
R_SetupView -- johnfitz -- this is the stuff that needs to be done once per frame, even in stereo mode
===============
*/
void R_SetupView (void)
{
	static qboolean gpuframedata_layout_logged = false;

	R_Quality_Update ();
	R_UpdateDynamicResolutionScale ();
	R_EnsureRenderTargetSampleState ();
	framesetup.composite_ready = false;
	memset (r_framedata.fogdata, 0, sizeof (r_framedata.fogdata));
	memset (r_framedata.skyfogdata, 0, sizeof (r_framedata.skyfogdata));

	if (r_gl_state_validate.value > 0.f && !gpuframedata_layout_logged)
	{
		gpuframedata_layout_logged = true;
		Con_Printf ("gpuframedata layout: sizeof=%u vieworg=%u viewproj=%u time=%u dlight_params=%u fog=%u\n",
			(unsigned)sizeof (gpuframedata_t),
			(unsigned)offsetof (gpuframedata_t, eye),
			(unsigned)offsetof (gpuframedata_t, viewproj),
			(unsigned)(offsetof (gpuframedata_t, eye) + 3 * sizeof (float)),
			(unsigned)offsetof (gpuframedata_t, dlight_params),
			(unsigned)offsetof (gpuframedata_t, fogdata));
	}

	R_AnimateLight ();

	{
		int overbright_bits = CLAMP (0, (int)Q_rint (r_overbrightbits.value), 3);
		float overbright = (float)(1 << overbright_bits);

		if (overbright > 1.f)
		{
			overbright = R_Tonemap_TemperedOverbright (overbright);

			float console_vis = GL_ConsoleVisibility ();
			if (console_vis > 0.f)
			{
				float compensated = 1.f + (overbright - 1.f) * (1.f - console_vis);
				overbright = compensated;
			}
		}

        r_framedata.dither[2] = overbright;
        r_framedata.dither[3] = 0.f;
}

	r_framecount++;
        r_framedata.eye[0] = r_refdef.vieworg[0];
        r_framedata.eye[1] = r_refdef.vieworg[1];
        r_framedata.eye[2] = r_refdef.vieworg[2];
        r_framedata.eye[3] = cl.time;
        r_framedata.lightmap_params[0] = r_lightmap_colorspace_debug.value > 0.f ? 1.f : 0.f;
        r_framedata.lightmap_params[1] = r_tonemap.value > 0.f ? 1.f : 0.f;
        r_framedata.lightmap_params[2] = (r_lightingdir.value > 0.f && lightmap_dir_texture) ? 1.f : 0.f;
        r_framedata.lightmap_params[3] = r_lightstyle_framefrac;
        r_framedata.lightgrid_params[0] = R_LightgridEnabled () ? 1.f : 0.f;
        r_framedata.lightgrid_params[1] = (r_lightgrid_debug.value >= 2.f) ? 1.f : 0.f;
        r_framedata.lightgrid_params[2] = (r_skyvis.value > 0.f && R_SkyVis_Active ()) ? 1.f : 0.f;
        r_framedata.lightgrid_params[3] = (r_skyvis_debug.value >= 2.f) ? 1.f : 0.f;
        r_framedata.dlight_params[0] = 1.f;
        r_framedata.dlight_params[1] = 0.f;
        r_framedata.dlight_params[2] = 0.f;
        r_framedata.dlight_params[3] = 0.f;
        r_framedata.colorspace_params[0] = CLAMP (0.f, r_debug_colorspace.value, 4.f);
        r_framedata.colorspace_params[1] = 0.f;
        r_framedata.colorspace_params[2] = 0.f;
        r_framedata.colorspace_params[3] = CLAMP (0.f, r_lighting_debug_view.value, 9.f);
        r_framedata.shader_params[0] = r_material_debug.value;
        r_framedata.shader_params[1] = r_tcgen_debug.value;
        /* Keep legacy sun-visibility fill attenuation alive for non-sky surfaces.
         * SkyVis skylight scale is carried separately in shader_params[3]. */
        r_framedata.shader_params[2] = CLAMP (0.f, r_sun_visibility.value, 1.f);
        r_framedata.shader_params[3] = R_SkyVis_GetResolvedScale ();

	{
		const sun_t *sun = R_GetSun ();
		qboolean sun_enabled = (r_sun_light.value > 0.f) && R_WorldHasSun ();

		r_framedata.sun_dir_enabled[0] = sun->dir[0];
		r_framedata.sun_dir_enabled[1] = sun->dir[1];
		r_framedata.sun_dir_enabled[2] = sun->dir[2];
		r_framedata.sun_dir_enabled[3] = sun_enabled ? 1.f : 0.f;

		r_framedata.sun_color_intensity[0] = sun->color[0];
		r_framedata.sun_color_intensity[1] = sun->color[1];
		r_framedata.sun_color_intensity[2] = sun->color[2];
		r_framedata.sun_color_intensity[3] = sun->intensity;
	}

	{
		vec3_t skyvis_tint = {0.f, 0.f, 0.f};

		if (r_skyvis.value > 0.f && R_SkyVis_Active ())
			R_SkyVis_GetTint (skyvis_tint);

		r_framedata.skyvis_tint[0] = skyvis_tint[0];
		r_framedata.skyvis_tint[1] = skyvis_tint[1];
		r_framedata.skyvis_tint[2] = skyvis_tint[2];
		r_framedata.skyvis_tint[3] = R_SkyVis_GetResolvedCap ();
	}

	double prev_delta = cl.time - r_prev_frame_time;
	qboolean prev_valid = r_prev_frame_valid && prev_delta > 0.0;

	if (prev_valid)
	{
		memcpy (r_framedata.prev_viewproj, r_prev_matviewproj, sizeof (r_prev_matviewproj));
                r_framedata.prev_eye[0] = r_prev_vieworg[0];
                r_framedata.prev_eye[1] = r_prev_vieworg[1];
                r_framedata.prev_eye[2] = r_prev_vieworg[2];
                r_framedata.prev_eye[3] = (float)prev_delta;
		r_framedata.prev_frame_valid = 1;
	}
	else
	{
		memcpy (r_framedata.prev_viewproj, r_identity_mat4, sizeof (r_identity_mat4));
                r_framedata.prev_eye[0] = r_refdef.vieworg[0];
                r_framedata.prev_eye[1] = r_refdef.vieworg[1];
                r_framedata.prev_eye[2] = r_refdef.vieworg[2];
                r_framedata.prev_eye[3] = 0.f;
		r_framedata.prev_frame_valid = 0;
		r_prev_frame_valid = false;
	}

	if (softemu == SOFTEMU_COARSE)
	{
                r_framedata.dither[0] = NOISESCALE * r_dither.value * r_softemu_dither_screen.value;
                r_framedata.dither[1] = NOISESCALE * r_dither.value * r_softemu_dither_texture.value;

		// r_fullbright replaces the actual lightmap texture with a 2x2 50% grey one.
		// Since texture-space dithering is applied on a scale of 1/16 of a lightmap texel,
		// this would lead to massively overscaled dithering patterns, so we disable
		// texture-space dithering in this case.
                if (r_fullbright_cheatsafe)
                        r_framedata.dither[1] = 0.f;
	}
	else if (softemu == SOFTEMU_OFF)
	{
                r_framedata.dither[0] = r_dither.value * (1.f / 255.f);
                r_framedata.dither[1] = 0.f;
	}
	else // FINE (screen-space dithering applied during postprocessing), or BANDED (no dithering)
	{
                r_framedata.dither[0] = 0.f;
                r_framedata.dither[1] = 0.f;
	}

	Sky_SetupFrame ();

	// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, r_origin);
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

	// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	//johnfitz -- calculate r_fovx and r_fovy here
	r_fovx = r_refdef.fov_x;
	r_fovy = r_refdef.fov_y;
	water_warp = false;
	{
		int view_contents = r_viewleaf->contents;
		int contents = view_contents;
		qboolean submerged = R_IsUnderwaterContents (contents);
		qboolean forced = (cl.forceunderwater || M_ForcedUnderwater ());
		qboolean underwater_active = (submerged || forced);
		qboolean underwater_postfx_active = underwater_active;

		contents = R_ResolveUnderwaterContents (view_contents, forced, r_origin);
		submerged = R_IsUnderwaterContents (contents);
		underwater_active = (submerged || forced);

		V_SetContentsColor (contents);
		V_CalcBlend ();

		if (r_waterwarp.value && underwater_active)
		{
			double t = forced ? realtime : cl.time;

			if (r_waterwarp.value > 1.f)
			{
				// Legacy warp has priority over postfx underwater treatment when animated FOV warp is active.
				// variance is a percentage of width, where width = 2 * tan(fov / 2) otherwise the effect is too dramatic at high FOV and too subtle at low FOV. what a mess!
				r_fovx = atan (tan (DEG2RAD (r_refdef.fov_x) / 2) * (0.97 + sin (t * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
				r_fovy = atan (tan (DEG2RAD (r_refdef.fov_y) / 2) * (1.03 - sin (t * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
				underwater_postfx_active = false;
			}
			else
			{
				water_warp = true;
			}
		}

		// Postfx stack consumes deterministic underwater state here.
		// CL_PostFX_Frame aggregates it and GL_PostProcess applies the resulting uniforms/LUT selection.
		CL_PostFX_SetContents (contents, underwater_active, underwater_postfx_active);
	}
	//johnfitz

	R_SetFrustum ();

	R_MarkSurfaces (); //johnfitz -- create texture chains from PVS

	R_SortEntities ();


	R_PushDlights ();
	/* Optional per-pixel path consumes this shared list in later milestones.
	 * Legacy lighting/rendering remains unchanged. */
	R_PPdlights_CollectFrame ();

	//johnfitz -- cheat-protect some draw modes
	r_fullbright_cheatsafe = r_lightmap_cheatsafe = false;
	r_drawworld_cheatsafe = true;
	if (cl.maxclients == 1)
	{
		if (!r_drawworld.value) r_drawworld_cheatsafe = false;

		if (r_fullbright.value) r_fullbright_cheatsafe = true;
		else if (r_lightmap.value) r_lightmap_cheatsafe = true;
	}
	if (!cl.worldmodel->lightdata)
	{
		r_fullbright_cheatsafe = true;
		r_lightmap_cheatsafe = false;
	}
	//johnfitz
}

void R_StorePrevFrameState (void)
{
	if (!r_frame_rendered_this_update)
	{
		r_prev_frame_valid = false;
		return;
	}

	double prev_time = r_prev_frame_time;

	memcpy (r_prev_matviewproj, r_matviewproj, sizeof (r_prev_matviewproj));
	VectorCopy (r_refdef.vieworg, r_prev_vieworg);

	r_prev_frame_time = cl.time;
	r_prev_frame_valid = (cl.time > prev_time);
	r_frame_rendered_this_update = false;
}

qboolean R_PrevFrameValid (void)
{
        return r_prev_frame_valid;
}




//==============================================================================
//
// RENDER VIEW
//
//==============================================================================

/*
=============
R_GetVisEntities
=============
*/
entity_t** R_GetVisEntities (modtype_t type, qboolean translucent, int* outcount)
{
	entity_t** entlist = cl_sorted_visedicts;
	int* ofs = cl_modtype_ofs + type * 2 + (translucent ? 1 : 0);
	*outcount = ofs[1] - ofs[0];
	return entlist + ofs[0];
}

/*
=============
R_DrawWater
=============
*/
static void R_DrawWater (qboolean translucent)
{
        entity_t** entlist = cl_sorted_visedicts;
        int* ofs = cl_modtype_ofs + 2 * mod_brush;

	if (translucent)
	{
		// all entities can have translucent water
		R_DrawBrushModels_Water (entlist + ofs[0], ofs[2] - ofs[0], true);
	}
	else
	{
		// only opaque entities can have opaque water
		R_DrawBrushModels_Water (entlist + ofs[0], ofs[1] - ofs[0], false);
	}

}

/*
=============
R_DrawEntitiesOnList
=============
*/
void R_DrawEntitiesOnList (qboolean alphapass) //johnfitz -- added parameter
{
	int* ofs;
	entity_t** entlist = cl_sorted_visedicts;

	GL_BeginGroup (alphapass ? "Translucent entities" : "Opaque entities");

	ofs = cl_modtype_ofs + (alphapass ? 1 : 0);
	R_DrawBrushModels (entlist + ofs[2 * mod_brush], ofs[2 * mod_brush + 1] - ofs[2 * mod_brush]);
	R_DrawAliasModels (entlist + ofs[2 * mod_alias], ofs[2 * mod_alias + 1] - ofs[2 * mod_alias]);
	if (!alphapass)
		R_DrawSpriteModels (entlist + cl_modtype_ofs[2 * mod_sprite], cl_modtype_ofs[2 * mod_sprite + 2] - cl_modtype_ofs[2 * mod_sprite]);

	GL_EndGroup ();
}

/*
=============
R_IsViewModelVisible
=============
*/
static qboolean R_IsViewModelVisible (void)
{
	entity_t* e = &cl.viewent;
	if (!r_drawviewmodel.value || !r_drawentities.value || chase_active.value || scr_viewsize.value >= 130)
	return false;

	if (cl.items & IT_INVISIBILITY || cl.stats[STAT_HEALTH] <= 0)
	return false;

	if (!e->model)
	return false;

	//johnfitz -- this fixes a crash
	if (e->model->type != mod_alias)
	return false;

	return true;
}

/*
=============
R_DrawViewModel -- johnfitz -- gutted
=============
*/
void R_DrawViewModel (void)
{
	entity_t* e = &cl.viewent;
	GLenum restore_depth_func = gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL;

	if (!R_IsViewModelVisible ())
		return;

	GL_BeginGroup ("View model");
	GL_SetScissorEnabled (false);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	/* Viewmodel is rendered after postprocess to keep it out of blur/DoF/SSAO/bloom.
	 * Reinitialize depth so world depth cannot clip the weapon while preserving
	 * correct self-occlusion inside the model. */
	glDepthMask (GL_TRUE);
	if (gl_clipcontrol_able)
		glClearDepth (0.0);
	else
		glClearDepth (1.0);
	glClear (GL_DEPTH_BUFFER_BIT);
	glDepthFunc (restore_depth_func);

	// hack the depth range to prevent view model from poking into walls
	GL_DepthRange (ZRANGE_VIEWMODEL);
	R_DrawAliasModels (&e, 1);
	GL_DepthRange (ZRANGE_FULL);
	glDepthFunc (restore_depth_func);

	GL_EndGroup ();
}

typedef struct debugvert_s
{
	vec3_t		pos;
	uint32_t	color;
} debugvert_t;

// FIX #2: Added max limits to prevent buffer overflow
#define MAX_DEBUG_VERTS 4096
#define MAX_DEBUG_IDX   8192

static debugvert_t	debugverts[MAX_DEBUG_VERTS];
static uint16_t		debugidx[MAX_DEBUG_IDX];
static int			numdebugverts = 0;
static int			numdebugidx = 0;
static qboolean		debugztest = false;

/*
================
R_FlushDebugGeometry
================
*/
static void R_FlushDebugGeometry (void)
{
	if (numdebugverts && numdebugidx)
	{
		GLuint	buf;
		GLbyte* ofs;
		unsigned int state;

		GL_UseProgram (glprogs.debug3d);
		state = GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (2);
		if (!debugztest)
			state |= GLS_NO_ZTEST;
		GL_SetState (state);

		GL_Upload (GL_ARRAY_BUFFER, debugverts, sizeof (debugverts[0]) * numdebugverts, &buf, &ofs);
		GL_BindBuffer (GL_ARRAY_BUFFER, buf);
		GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (debugverts[0]), ofs + offsetof (debugvert_t, pos));
		GL_VertexAttribPointerFunc (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (debugverts[0]), ofs + offsetof (debugvert_t, color));

		GL_Upload (GL_ELEMENT_ARRAY_BUFFER, debugidx, sizeof (debugidx[0]) * numdebugidx, &buf, &ofs);
		GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
		glDrawElements (GL_LINES, numdebugidx, GL_UNSIGNED_SHORT, ofs);
	}

	numdebugverts = 0;
	numdebugidx = 0;
}

/*
================
R_SetDebugGeometryZTest
================
*/
static void R_SetDebugGeometryZTest (qboolean ztest)
{
	if (debugztest == ztest)
		return;
	R_FlushDebugGeometry ();
	debugztest = ztest;
}

/*
================
R_AddDebugGeometry

FIX #2: Added bounds checking to prevent buffer overflow
================
*/
static void R_AddDebugGeometry (const debugvert_t verts[], int numverts, const uint16_t idx[], int numidx)
{
	int i;

	// FIX #2: Validate input sizes before adding
	if (numverts <= 0 || numidx <= 0)
		return;

	if (numverts > MAX_DEBUG_VERTS || numidx > MAX_DEBUG_IDX)
	{
		Con_Warning ("R_AddDebugGeometry: geometry too large (%d verts, %d indices)\n", numverts, numidx);
		return;
	}

	if (numdebugverts + numverts > MAX_DEBUG_VERTS ||
		numdebugidx + numidx > MAX_DEBUG_IDX)
		R_FlushDebugGeometry ();

	for (i = 0; i < numidx; i++)
		debugidx[numdebugidx + i] = idx[i] + numdebugverts;
	numdebugidx += numidx;

	for (i = 0; i < numverts; i++)
		debugverts[numdebugverts + i] = verts[i];
	numdebugverts += numverts;
}

/*
================
R_EmitLine
================
*/
static void R_EmitLine (const vec3_t a, const vec3_t b, uint32_t color)
{
        debugvert_t verts[2];
        uint16_t idx[2];

	VectorCopy (a, verts[0].pos);
	VectorCopy (b, verts[1].pos);
	verts[0].color = color;
	verts[1].color = color;
	idx[0] = 0;
	idx[1] = 1;

	R_AddDebugGeometry (verts, 2, idx, 2);
}

/*
================
R_EmitWirePoint -- johnfitz -- draws a wireframe cross shape for point entities
================
*/
static void R_EmitWirePoint (const vec3_t origin, uint32_t color)
{
        const float Size = 8.f;
        int i;
        for (i = 0; i < 3; i++)
        {
                vec3_t a, b;
                VectorCopy (origin, a);
                VectorCopy (origin, b);
                a[i] -= Size;
                b[i] += Size;
                R_EmitLine (a, b, color);
        }
}

static uint32_t R_PackDebugColor (const vec3_t color)
{
	const int r = (int)CLAMP (0, Q_rint (color[0] * 255.f), 255);
	const int g = (int)CLAMP (0, Q_rint (color[1] * 255.f), 255);
	const int b = (int)CLAMP (0, Q_rint (color[2] * 255.f), 255);

	return (uint32_t)(0xff << 24 | r << 16 | g << 8 | b);
}

/*
================
R_EmitWireBox -- johnfitz -- draws one axis aligned bounding box
================
*/
static const uint16_t boxidx[12 * 2] = { 0,1, 0,2, 0,4, 1,3, 1,5, 2,3, 2,6, 3,7, 4,5, 4,6, 5,7, 6,7, };

static void R_EmitWireBox (const vec3_t mins, const vec3_t maxs, uint32_t color)
{
	int i;
	debugvert_t v[8];

	for (i = 0; i < 8; i++)
	{
		v[i].pos[0] = i & 1 ? mins[0] : maxs[0];
		v[i].pos[1] = i & 2 ? mins[1] : maxs[1];
		v[i].pos[2] = i & 4 ? mins[2] : maxs[2];
		v[i].color = color;
	}

        R_AddDebugGeometry (v, countof (v), boxidx, countof (boxidx));
}

void R_DebugDrawWireBox (const vec3_t mins, const vec3_t maxs, const vec3_t color, qboolean ztest)
{
	R_SetDebugGeometryZTest (ztest);
	R_EmitWireBox (mins, maxs, R_PackDebugColor (color));
}

void R_DebugFlushGeometry (void)
{
	R_FlushDebugGeometry ();
}

static void R_EmitDiamond (const vec3_t center, float radius, uint32_t color)
{
	debugvert_t v[6];
	uint16_t idx[] = {
		0, 2, 0, 3, 0, 4, 0, 5,
		1, 2, 1, 3, 1, 4, 1, 5,
		2, 4, 2, 5, 3, 4, 3, 5
	};

	VectorSet (v[0].pos, center[0] + radius, center[1], center[2]);
	VectorSet (v[1].pos, center[0] - radius, center[1], center[2]);
	VectorSet (v[2].pos, center[0], center[1] + radius, center[2]);
	VectorSet (v[3].pos, center[0], center[1] - radius, center[2]);
	VectorSet (v[4].pos, center[0], center[1], center[2] + radius);
	VectorSet (v[5].pos, center[0], center[1], center[2] - radius);

	for (size_t i = 0; i < countof (v); i++)
		v[i].color = color;

	R_AddDebugGeometry (v, countof (v), idx, countof (idx));
}

/*
================
R_EmitArrow
================
*/
static void R_EmitArrow (const vec3_t from, const vec3_t to, uint32_t color)
{
	float	frac, len;
	vec3_t	center, dir, side, tmp;

	R_EmitLine (from, to, color);

	VectorSubtract (to, from, dir);
	len = VectorNormalize (dir);
	if (len < 1e-2f)
	{
		VectorCopy (vup, dir);
		VectorCopy (vright, side);
	}
	else
	{
		VectorSubtract (from, r_origin, tmp);
		CrossProduct (dir, tmp, side);
		VectorNormalize (side);
	}

	frac = realtime - floor (realtime);
	VectorLerp (from, to, frac, center);

	VectorMA (center, 8.f, side, tmp);
	VectorMA (tmp, -8.f, dir, tmp);
	R_EmitLine (tmp, center, color);

	VectorMA (tmp, -16.f, side, tmp);
	R_EmitLine (tmp, center, color);
}

/*
================
R_EmitEdictLink
================
*/
static void R_EmitEdictLink (const edict_t* from, const edict_t* to, showbboxflags_t flags)
{
	vec3_t vec_from, vec_to;

	if (!flags)
		return;

	VectorCopy (from->v.origin, vec_from);
	if (!VectorCompare (from->v.mins, from->v.maxs))
	{
		VectorMA (vec_from, 0.5f, from->v.mins, vec_from);
		VectorMA (vec_from, 0.5f, from->v.maxs, vec_from);
	}

	VectorCopy (to->v.origin, vec_to);
	if (!VectorCompare (to->v.mins, to->v.maxs))
	{
		VectorMA (vec_to, 0.5f, to->v.mins, vec_to);
		VectorMA (vec_to, 0.5f, to->v.maxs, vec_to);
	}

	if (flags == SHOWBBOX_LINK_BOTH)
		R_EmitLine (vec_from, vec_to, 0x7f7f3f7f);
	else if (flags == SHOWBBOX_LINK_OUTGOING)
		R_EmitArrow (vec_from, vec_to, 0x7f7f3f3f);
	else if (flags == SHOWBBOX_LINK_INCOMING)
		R_EmitArrow (vec_to, vec_from, 0x7f3f3f7f);
}

/*
================
R_ShowBoundingBoxesFilter

r_showbboxes_filter artifact =trigger_secret #42

PERF OPT: Cache filter string lengths to avoid repeated strlen calls
================
*/
char r_showbboxes_filter_strings[MAXCMDLINE];
qboolean r_showbboxes_filter_byindex;

// PERF OPT: Cache for filter performance
static struct {
	qboolean valid;
	int num_filters;
	struct {
		const char* str;
		int len;
		qboolean is_exact;
		qboolean is_index;
	} filters[32]; // reasonable maximum
} filter_cache = { false, 0 };

static void R_UpdateFilterCache (void)
{
	const char* filter_p;
	int count = 0;

	filter_cache.valid = true;
	filter_cache.num_filters = 0;

	if (!r_showbboxes_filter_strings[0])
		return;

	for (filter_p = r_showbboxes_filter_strings; *filter_p && count < 32; filter_p += strlen (filter_p) + 1)
	{
		filter_cache.filters[count].str = filter_p;
		filter_cache.filters[count].len = strlen (filter_p);
		filter_cache.filters[count].is_exact = (*filter_p == '=');
		filter_cache.filters[count].is_index = (*filter_p == '#');
		count++;
	}

	filter_cache.num_filters = count;
}

static qboolean R_ShowBoundingBoxesFilter (edict_t* ed)
{
	char entnum[16] = "";
	const char* classname = NULL;
	int i;

	// PERF OPT: Early return if no filters
	if (!r_showbboxes_filter_strings[0])
		return true;

	// PERF OPT: Update cache if invalid
	if (!filter_cache.valid)
		R_UpdateFilterCache ();

	if (r_showbboxes_filter_byindex)
		q_snprintf (entnum, sizeof (entnum), "%d", NUM_FOR_EDICT (ed));

	if (ed->v.classname)
		classname = PR_GetString (ed->v.classname);

	// PERF OPT: Use cached filter data
	for (i = 0; i < filter_cache.num_filters; i++)
	{
		if (filter_cache.filters[i].is_index)
		{
			if (!strcmp (entnum, filter_cache.filters[i].str + 1))
		return true;
			continue;
		}

		if (!classname)
			continue;

		if (filter_cache.filters[i].is_exact)
		{
			if (!strcmp (classname, filter_cache.filters[i].str + 1))
		return true;
			continue;
		}

		if (strstr (classname, filter_cache.filters[i].str) != NULL)
		return true;
	}

	return false;
}

// PERF OPT: Invalidate cache when filter changes
void R_InvalidateFilterCache (void)
{
	filter_cache.valid = false;
}

static edict_t** bbox_edicts = NULL;		// all edicts shown by r_showbboxes & co
edict_t** bbox_linked = NULL;				// focused edict, followed by edicts linked from/to it

/*
================
R_AddHighlightedEntity
================
*/
static void R_AddHighlightedEntity (edict_t* ed, showbboxflags_t flags)
{
	if (ed->showbboxframe != r_framecount)
	{
		ed->showbboxframe = r_framecount;
		ed->showbboxflags = SHOWBBOX_LINK_NONE;
		VEC_PUSH (bbox_edicts, ed);
	}

	if (!(ed->showbboxflags & flags) && (int)r_showbboxes_links.value & flags)
	{
		VEC_PUSH (bbox_linked, ed);
		ed->showbboxflags |= flags;
	}
}

/*
================
R_ClearBoundingBoxes
================
*/
void R_ClearBoundingBoxes (void)
{
	VEC_CLEAR (bbox_edicts);
	VEC_CLEAR (bbox_linked);
}

/*
================
R_ShowBoundingBoxes -- johnfitz

draw bounding boxes -- the server-side boxes, not the renderer cullboxes
================
*/
static void R_ShowBoundingBoxes (void)
{
	extern		edict_t* sv_player;
	byte* pvs;
	vec3_t		mins, maxs;
	edict_t* ed, * focused;
	int			i, j, mode;
	uint32_t	color;
	qcvm_t* oldvm;	//in case we ever draw a scene from within csqc.
	float		dist, bestdist, extend;
	vec3_t		rcpdelta;
	const float	bbox_rcp_epsilon = 1e-6f;
	int			numentityfields;

	VEC_CLEAR (bbox_edicts);
	VEC_CLEAR (bbox_linked);
	focused = NULL;

	mode = abs ((int)r_showbboxes.value);
	if ((!mode && !r_showfields.value) || cl.maxclients > 1 || !r_drawentities.value || !sv.active)
		return;

	GL_BeginGroup ("Show bounding boxes");

	R_SetDebugGeometryZTest (false);

	oldvm = qcvm;
	PR_SwitchQCVM (NULL);
	PR_SwitchQCVM (&sv.qcvm);

	// Use PVS if r_showbboxes >= 2, or if r_showbboxes is 0 (which means r_showfields is active)
	if (mode >= 2 || mode == 0)
	{
		vec3_t org;
		VectorAdd (sv_player->v.origin, sv_player->v.view_ofs, org);
		pvs = SV_FatPVS (org, sv.worldmodel);
	}
	else
		pvs = NULL;

	// Compute ray reciprocal delta
	for (i = 0; i < 3; i++)
	{
		const float denom = gl_farclip.value * vpn[i];
		rcpdelta[i] = (fabsf (denom) > bbox_rcp_epsilon) ? (1.f / denom) : 0.f;
	}

	// Iterate over all server entities
	bestdist = FLT_MAX;
	for (i = 1, ed = NEXT_EDICT (qcvm->edicts); i < qcvm->num_edicts; i++, ed = NEXT_EDICT (ed))
	{
		if (ed == sv_player || ed->free)
			continue; // don't draw player's own bbox or freed edicts

		if (r_showbboxes_think.value && (ed->v.nextthink <= 0) == (r_showbboxes_think.value > 0))
			continue;

		if (r_showbboxes_health.value && (ed->v.health <= 0) == (r_showbboxes_health.value > 0))
			continue;

		// Compute bounding box (16 units wide for point entities)
		extend = VectorCompare (ed->v.mins, ed->v.maxs) ? 8.f : 0.f;
		for (j = 0; j < 3; j++)
		{
			mins[j] = ed->v.origin[j] + ed->v.mins[j] - extend;
			maxs[j] = ed->v.origin[j] + ed->v.maxs[j] + extend;
		}

		// Frustum culling
		if (R_CullBox (mins, maxs))
			continue;

		// Classname or edict num filter
		if (!R_ShowBoundingBoxesFilter (ed))
			continue;

		// PVS filter
		if (pvs)
		{
			qboolean inpvs =
				ed->num_leafs ?
				SV_EdictInPVS (ed, pvs) :
				SV_BoxInPVS (ed->v.absmin, ed->v.absmax, pvs, sv.worldmodel->nodes)
				;
			if (!inpvs)
				continue;
		}

		// Keep track of the closest bounding box intersecting the center ray
		// Note: if we're inside the box (dist == 0), we ignore this entity
		if (RayVsBox (r_origin, rcpdelta, mins, maxs, &dist) && dist > 0.f && dist < bestdist)
		{
			bestdist = dist;
			focused = ed;
		}

		// Add edict to list
		R_AddHighlightedEntity (ed, SHOWBBOX_LINK_NONE);
	}

	if (focused)
		VEC_PUSH (bbox_linked, focused);

	if (focused && r_showbboxes_links.value)
	{
		if (!qcvm || !qcvm->entityfieldofs)
			numentityfields = 0;
		else if (qcvm->numentityfields < 0 || qcvm->numentityfields > 16384)
			numentityfields = 0;
		else
			numentityfields = qcvm->numentityfields;

		// Find outgoing links (entity field references other than .chain)
		if (((int)r_showbboxes_links.value & SHOWBBOX_LINK_OUTGOING) && numentityfields)
		{
			for (i = 0; i < numentityfields; i++)
			{
				const int fieldofs = qcvm->entityfieldofs[i];
				eval_t* val;

				if (fieldofs < 0 || fieldofs > (int)sizeof (entvars_t) - (int)sizeof (eval_t))
					continue;
				val = (eval_t*)((char*)&focused->v + fieldofs);
				if (fieldofs == offsetof (entvars_t, chain) || !val->edict)
					continue;
				ed = PROG_TO_EDICT (val->edict);
				if (ed == focused || ed->free || ed == sv_player)
					continue;
				R_AddHighlightedEntity (ed, SHOWBBOX_LINK_OUTGOING);
			}
		}

		// Inspect all other edicts to find incoming links
		// (either entity field references or target/targetname matches)
		if (((int)r_showbboxes_links.value & SHOWBBOX_LINK_INCOMING) || r_showbboxes_targets.value)
		{
			const char* focus_target = PR_GetString (focused->v.target);
			const char* focus_targetname = PR_GetString (focused->v.targetname);

			for (i = 1, ed = NEXT_EDICT (qcvm->edicts); i < qcvm->num_edicts; i++, ed = NEXT_EDICT (ed))
			{
				if (ed == sv_player || ed->free || ed == focused)
					continue;

				// Check target/targetname matches
				if (r_showbboxes_targets.value && (*focus_target || *focus_targetname))
				{
					const char* target = PR_GetString (ed->v.target);
					const char* targetname = PR_GetString (ed->v.targetname);

					if (*focus_targetname && !strcmp (focus_targetname, target))
						R_AddHighlightedEntity (ed, SHOWBBOX_LINK_INCOMING);
					if (*focus_target && !strcmp (focus_target, targetname))
						R_AddHighlightedEntity (ed, SHOWBBOX_LINK_OUTGOING);
				}

				// Check for entity field references (other than .chain)
				if (((int)r_showbboxes_links.value & SHOWBBOX_LINK_INCOMING) && numentityfields)
				{
					for (j = 0; j < numentityfields; j++)
					{
						const int fieldofs = qcvm->entityfieldofs[j];
						eval_t* val;

						if (fieldofs < 0 || fieldofs > (int)sizeof (entvars_t) - (int)sizeof (eval_t))
							continue;
						val = (eval_t*)((char*)&ed->v + fieldofs);
						if (fieldofs == offsetof (entvars_t, chain) || !val->edict)
							continue;
						if (PROG_TO_EDICT (val->edict) == focused)
							R_AddHighlightedEntity (ed, SHOWBBOX_LINK_INCOMING);
					}
				}
			}
		}

		// Draw all links
		for (j = 0; j < (int)VEC_SIZE (bbox_linked); j++)
			R_EmitEdictLink (focused, bbox_linked[j], bbox_linked[j]->showbboxflags);
	}

	// Draw all the matching edicts
	for (i = 0; i < (int)VEC_SIZE (bbox_edicts); i++)
	{
		ed = bbox_edicts[i];

		if (ed == focused)
			color = 0xffffffff;
		else if (ed->showbboxflags)
			color = 0xaaaaaaaa;
		else if (r_showbboxes.value > 0.f)
		{
			int modelindex = (int)ed->v.modelindex;
			color = 0x7f800080;
			if (modelindex >= 0 && modelindex < MAX_MODELS && sv.models[modelindex])
			{
				switch (sv.models[modelindex]->type)
				{
				case mod_brush:  color = 0x7fff8080; break;
				case mod_alias:  color = 0x7f408080; break;
				case mod_sprite: color = 0x7f4040ff; break;
				default:
					break;
				}
			}
			if (ed->v.health > 0)
				color = 0x7f0000ff;
		}
		else if (r_showbboxes.value < 0.f)
			color = 0x7fffffff;
		else
			color = 0x5f7f7f7f;

		if (VectorCompare (ed->v.mins, ed->v.maxs))
		{
			//point entity
			R_EmitWirePoint (ed->v.origin, color);
		}
		else
		{
			//box entity
			VectorAdd (ed->v.mins, ed->v.origin, mins);
			VectorAdd (ed->v.maxs, ed->v.origin, maxs);
			R_EmitWireBox (mins, maxs, color);
		}
	}

	VEC_CLEAR (bbox_edicts);

	PR_SwitchQCVM (NULL);
	PR_SwitchQCVM (oldvm);

	R_FlushDebugGeometry ();

	Sbar_Changed (); //so we don't get dots collecting on the statusbar

	GL_EndGroup ();
}

/*
===============
R_ShowPointFile
===============
*/
static void R_ShowPointFile (void)
{
	size_t i;

	if (VEC_SIZE (r_pointfile) == 0)
		return;

	GL_BeginGroup ("Point file");
	R_SetDebugGeometryZTest (true);
	for (i = 1; i < VEC_SIZE (r_pointfile); i++)
		R_EmitArrow (r_pointfile[i - 1], r_pointfile[i], 0xff3f3f7f);
	R_FlushDebugGeometry ();
	GL_EndGroup ();
}

static const lightgrid_t *r_lightgrid_debug_reported_grid = NULL;
static qboolean r_lightgrid_debug_reported_loaded = false;
static char r_lightgrid_debug_reported_reason[64];

static void R_ShowLightgridDebug (void)
{
        const lightgrid_t *lg;

        if (r_lightgrid_debug.value <= 0.f)
        {
                r_lightgrid_debug_reported_grid = NULL;
                r_lightgrid_debug_reported_loaded = false;
                r_lightgrid_debug_reported_reason[0] = '\0';
                return;
        }

        lg = Lightgrid_Get ();
        if (!lg || !lg->octree)
        {
                const char *reason = "unknown";

                if (!lg)
                        reason = "no lightgrid is loaded";
                else if (!lg->octree)
                        reason = "lightgrid has no octree";

                if (!r_lightgrid_debug_reported_reason[0]
                        || q_strcasecmp (reason, r_lightgrid_debug_reported_reason))
                {
                        Con_Printf ("r_lightgrid_debug: %s\n", reason);
                        q_strlcpy (r_lightgrid_debug_reported_reason, reason, sizeof (r_lightgrid_debug_reported_reason));
                }

                r_lightgrid_debug_reported_grid = NULL;
                r_lightgrid_debug_reported_loaded = false;
                return;
        }

        if (lg != r_lightgrid_debug_reported_grid || !r_lightgrid_debug_reported_loaded)
        {
                Con_Printf ("r_lightgrid_debug: loaded from %s (%zu nodes, %zu leaves)\n",
                        Lightgrid_GetSource (),
                        lg->octree ? lg->octree->node_count : 0,
                        lg->octree ? lg->octree->leaf_count : 0);
                r_lightgrid_debug_reported_grid = lg;
                r_lightgrid_debug_reported_loaded = true;
                r_lightgrid_debug_reported_reason[0] = '\0';
        }
}

/*
===============
Collinear
===============
*/
static qboolean Collinear (const vec3_t a, const vec3_t b, const vec3_t c)
{
	return Distance (a, b) + Distance (b, c) < Distance (a, c) * 1.00001f;
}

/*
===============
R_ReadPointFile_f
===============
*/
void R_ReadPointFile_f (void)
{
	FILE* f;
	vec3_t		org;
	int			r, n;
	qboolean	leakmode;
	char		name[MAX_QPATH];

	VEC_CLEAR (r_pointfile);

	if (cls.state != ca_connected)
		return;			// need an active map.

	q_snprintf (name, sizeof (name), "maps/%s.pts", cl.mapname);
	leakmode = Cmd_Argc () >= 2 && !strcmp (Cmd_Argv (1), "leak");

	COM_FOpenFile (name, &f, NULL);
	if (!f)
	{
		Con_Printf ("couldn't open %s\n", name);
		return;
	}

	if (!leakmode)
		Con_Printf ("Reading %s...\n", name);
	org[0] = org[1] = org[2] = 0; // silence pesky compiler warnings

	for (r = 0; fscanf (f, "%f %f %f\n", &org[0], &org[1], &org[2]) == 3; r++)
	{
		Vec_Append ((void**)&r_pointfile, sizeof (r_pointfile[0]), &org, 1);
		n = (int)VEC_SIZE (r_pointfile);
		if (n >= 3 && Collinear (r_pointfile[n - 3], r_pointfile[n - 2], r_pointfile[n - 1]))
		{
			VectorCopy (r_pointfile[n - 1], r_pointfile[n - 2]);
			VEC_POP (r_pointfile);
		}
	}

	fclose (f);

	if (leakmode)
		Con_Warning ("map appears to have leaks!\n");
	else
		Con_Printf ("%i points read (%i significant)\n", r, (int)VEC_SIZE (r_pointfile));
}

/*
================
R_ShowTris -- johnfitz
================
*/
void R_ShowTris (void)
{
        int* ofs;
        entity_t** entlist = cl_sorted_visedicts;

        if (r_showtris.value < 1 || r_showtris.value > 2 || cl.maxclients > 1)
                return;

        GL_BeginGroup ("Show tris");

        R_UploadFrameData ();

	if (r_showtris.value == 1)
		GL_DepthRange (ZRANGE_NEAR);
	glPolygonMode (GL_FRONT_AND_BACK, GL_LINE);
	GL_PolygonOffset (OFFSET_SHOWTRIS);

	ofs = cl_modtype_ofs;
	R_DrawBrushModels_ShowTris (entlist + ofs[2 * mod_brush], ofs[2 * mod_brush + 2] - ofs[2 * mod_brush]);
	R_DrawAliasModels_ShowTris (entlist + ofs[2 * mod_alias], ofs[2 * mod_alias + 2] - ofs[2 * mod_alias]);
	R_DrawSpriteModels_ShowTris (entlist + ofs[2 * mod_sprite], ofs[2 * mod_sprite + 2] - ofs[2 * mod_sprite]);

	// viewmodel
	if (R_IsViewModelVisible ())
	{
		entity_t* e = &cl.viewent;

		if (r_showtris.value != 1.f)
			GL_DepthRange (ZRANGE_VIEWMODEL);

		R_DrawAliasModels_ShowTris (&e, 1);

		GL_DepthRange (ZRANGE_FULL);
	}

	R_DrawParticles_ShowTris ();

	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_PolygonOffset (OFFSET_NONE);
	if (r_showtris.value == 1)
		GL_DepthRange (ZRANGE_FULL);

	Sbar_Changed (); //so we don't get dots collecting on the statusbar

	GL_EndGroup ();
}

void R_RenderShadowMaps (void)
{
	R_Shadow_RenderMaps (cl_shadow_visedicts, cl_numshadowedicts);
}

/*
================
R_RenderScene
================
*/
void R_RenderScene (const RenderGraphResourceHandle *resources)
{
	(void)resources;
	R_SetupScene (); //johnfitz -- this does everything that should be done once per call to RenderScene
	R_Clear ();
	
	// Upload frame data after fog has been set up to ensure fog parameters
	// are available to all draw calls, even when light clustering is skipped.
	R_UploadFrameData ();
	S_ExtraUpdate (); // don't let sound get messed up if going slow
	R_DrawEntitiesOnList (false); //johnfitz -- false means this is the pass for nonalpha entities
	R_DrawDecals ();
	R_DrawDLightPass ();
	R_DrawParticles (false);
	Sky_DrawSky (); //johnfitz
	R_DrawWater (false);
	R_BeginTranslucency ();
	R_DrawWater (true);
	R_DrawEntitiesOnList (true); //johnfitz -- true means this is the pass for alpha entities
	R_DrawParticles (true);
	R_EndTranslucency ();
	R_ShowTris (); //johnfitz
	R_ShowBoundingBoxes (); //johnfitz
	R_ShowPointFile ();
	R_ShowLightgridDebug ();
}

/*
================
R_WarpScaleView

The r_scale cvar allows rendering the 3D view at 1/2, 1/3, or 1/4 resolution.
This function scales the reduced resolution 3D view back up to fill
r_refdef.vrect. This is for emulating a low-resolution pixellated look,
or possibly as a perforance boost on slow graphics cards.
================
*/
void R_WarpScaleView (const RenderGraphResourceHandle *resources)
{
	int srcx, srcy, srcw, srch;
	float smax, tmax;
	GLuint scene_fbo = (resources && resources->scene_fbo) ? resources->scene_fbo : framebufs.scene.fbo;
	GLuint scene_color_tex = (resources && resources->scene_color_tex) ? resources->scene_color_tex : framebufs.scene.color_tex;
	GLuint scene_velocity_tex = (resources && resources->scene_velocity_tex) ? resources->scene_velocity_tex : framebufs.scene.velocity_tex;
	GLuint resolved_scene_fbo = (resources && resources->resolved_scene_fbo) ? resources->resolved_scene_fbo : framebufs.resolved_scene.fbo;
	GLuint resolved_scene_color_tex = (resources && resources->resolved_scene_color_tex) ? resources->resolved_scene_color_tex : framebufs.resolved_scene.color_tex;
	GLuint resolved_scene_velocity_tex = (resources && resources->resolved_scene_velocity_tex) ? resources->resolved_scene_velocity_tex : framebufs.resolved_scene.velocity_tex;
	GLuint composite_fbo = (resources && resources->composite_fbo) ? resources->composite_fbo : framebufs.composite.fbo;
	int scene_samples = (resources && resources->scene_samples > 0) ? resources->scene_samples : framebufs.scene.samples;
	qboolean msaa = scene_samples > 1;
	qboolean needwarpscale;
	qboolean need_depth_resolve;
	qboolean force_blit_upscale;
	qboolean needs_scene_effects = false;
	qboolean needs_postprocess = false;
	GLuint fbodest;
	double t;

	R_GetFramePlanDecisions (&needs_scene_effects, &needs_postprocess);
	if (!needs_scene_effects)
		return;

	srcx = glx + r_refdef.vrect.x;
	srcy = gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height;
	srcw = R_GetSceneRenderWidth ();
	srch = R_GetSceneRenderHeight ();

	force_blit_upscale = (R_GetSceneRenderScale () != 1) || (r_drs.value > 0.f);
	needwarpscale = water_warp && !force_blit_upscale;
	fbodest = needs_postprocess ? composite_fbo : 0;
	need_depth_resolve = (fbodest == composite_fbo)
		&& (R_DoFEnabled () || r_ssao.value > 0.f || r_ssao_debug.value > 0.f || R_FogVol_ShouldAffectPostFX ()
			|| (R_Godrays_IsReady (cl.worldmodel, r_framecount) && (r_godrays.value > 0.f || r_godrays_debug.value > 0.f || r_godrays_debug_source.value > 0.f)));

	if (msaa)
	{
		GL_BeginGroup ("MSAA resolve");

		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, scene_fbo);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, resolved_scene_fbo);

		glReadBuffer (GL_COLOR_ATTACHMENT0);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		GL_BlitFramebufferFunc (0, 0, srcw, srch, 0, 0, srcw, srch, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		if (scene_velocity_tex && resolved_scene_velocity_tex)
		{
			glReadBuffer (GL_COLOR_ATTACHMENT1);
			glDrawBuffer (GL_COLOR_ATTACHMENT1);
			GL_BlitFramebufferFunc (0, 0, srcw, srch, 0, 0, srcw, srch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		GL_EndGroup ();

		if (!needwarpscale)
		{
			int dstw = force_blit_upscale ? r_refdef.vrect.width : srcw;
			int dsth = force_blit_upscale ? r_refdef.vrect.height : srch;
			GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, resolved_scene_fbo);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
			GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, fbodest);
			if (fbodest)
				glDrawBuffer (GL_COLOR_ATTACHMENT0);
			else
				glDrawBuffer (GL_BACK);
			{
				GLbitfield mask = GL_COLOR_BUFFER_BIT;
				if (need_depth_resolve)
					mask |= GL_DEPTH_BUFFER_BIT;
				GL_BlitFramebufferFunc (0, 0, srcw, srch, srcx, srcy, srcx + dstw, srcy + dsth, mask, GL_NEAREST);
			}
		}
	}

	if (need_depth_resolve)
	{
		int dstw = (R_GetSceneRenderScale () != 1) ? r_refdef.vrect.width : srcw;
		int dsth = (R_GetSceneRenderScale () != 1) ? r_refdef.vrect.height : srch;

		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, scene_fbo);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, composite_fbo);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		GL_BlitFramebufferFunc (0, 0, srcw, srch, srcx, srcy, srcx + dstw, srcy + dsth, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	}

	if (!msaa && !needwarpscale)
	{
		int dstw = force_blit_upscale ? r_refdef.vrect.width : srcw;
		int dsth = force_blit_upscale ? r_refdef.vrect.height : srch;
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, scene_fbo);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, fbodest);
		if (fbodest)
			glDrawBuffer (GL_COLOR_ATTACHMENT0);
		else
			glDrawBuffer (GL_BACK);
		{
			GLbitfield mask = GL_COLOR_BUFFER_BIT;
			if (need_depth_resolve)
			{
				mask |= GL_DEPTH_BUFFER_BIT;
				need_depth_resolve = false;
			}
			GL_BlitFramebufferFunc (0, 0, srcw, srch,
				srcx, srcy, srcx + dstw, srcy + dsth,
				mask, GL_NEAREST);
		}
	}

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, fbodest);
	if (fbodest)
	{
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
	}
	else
	{
		glDrawBuffer (GL_BACK);
		glReadBuffer (GL_BACK);
	}
	glViewport (srcx, srcy, r_refdef.vrect.width, r_refdef.vrect.height);

	if (!needwarpscale)
	{
		if (fbodest == composite_fbo)
			framesetup.composite_ready = true;
		return;
	}

	GL_BeginGroup ("Warp/scale view");

	/* Canonical scene-size path renders directly into size-matched scene textures,
	 * so warp/upscale should sample the full source texture range. */
	smax = 1.f;
	tmax = 1.f;

	GL_UseProgram (glprogs.warpscale[water_warp]);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));

	t = M_ForcedUnderwater () ? realtime : cl.time;
	GL_Uniform4fFunc (0, smax, tmax, water_warp ? 1.f / 256.f : 0.f, (float)t);
	// View blends are applied after postprocess/UI to avoid AO/tone-map affecting overlays.
	GL_Uniform4fFunc (1, 0.f, 0.f, 0.f, 0.f);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, msaa ? resolved_scene_color_tex : scene_color_tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, water_warp && msaa ? GL_LINEAR : GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, water_warp && msaa ? GL_LINEAR : GL_NEAREST);

	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();

	if (fbodest == composite_fbo)
		framesetup.composite_ready = true;
}

static void R_FG_ExecFogVolPrepare (RenderPassContext *ctx)
{
	(void)ctx;
	R_PrepareFogVolInputs ();
}

static void R_FG_ExecSetupView (RenderPassContext *ctx)
{
	(void)ctx;
	R_SetupView ();
}

static const RenderPassDesc s_setup_view_framegraph_pass = {
	"Setup view",
	RENDER_RES_NONE,
	RENDER_RES_NONE,
	1u << 0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	NULL,
	R_FG_ExecSetupView,
	FG_PASS_STAGE_SETUP,
	FG_PASS_STATS_SETUP
};

static qboolean R_FG_PassWhenShadowEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_shadowmaps;
}

static void R_FG_ExecShadowMaps (RenderPassContext *ctx)
{
	(void)ctx;
	R_RenderShadowMaps ();
}

static const RenderPassDesc s_shadowmaps_framegraph_pass = {
	"Shadow maps",
	RENDER_RES_NONE,
	RENDER_RES_SHADOW_SUN_DEPTH,
	0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	R_FG_PassWhenShadowEnabled,
	R_FG_ExecShadowMaps,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_SHADOW
};

static void R_FG_ExecScene (RenderPassContext *ctx)
{
	R_RenderScene (ctx ? ctx->resources : NULL);
}

static const RenderPassDesc s_scene_framegraph_pass = {
	"Render scene",
	RENDER_RES_DECALS | RENDER_RES_SHADOW_SUN_DEPTH,
	RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_VELOCITY,
	0,
	FG_PASS_OUTPUT_AUTO_SCENE,
	FG_PASS_VIEWPORT_VIEW_RECT_SCALED,
	NULL,
	R_FG_ExecScene,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_SCENE
};

static void R_FG_ExecWarpResolve (RenderPassContext *ctx)
{
	R_WarpScaleView (ctx ? ctx->resources : NULL);
}

static const RenderPassDesc s_warp_resolve_framegraph_pass = {
	"Warp/resolve",
	RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH,
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
	1u << 0,
	FG_PASS_OUTPUT_AUTO_WARP,
	FG_PASS_VIEWPORT_VIEW_RECT,
	NULL,
	R_FG_ExecWarpResolve,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_WARP
};

static const RenderPassDesc s_fogvol_prepare_framegraph_pass = {
	"Prepare fogvol inputs",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
	RENDER_RES_FOGVOL_INPUTS,
	0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	NULL,
	R_FG_ExecFogVolPrepare
};

static qboolean R_FG_HasResources (const RenderPassContext *ctx, unsigned required)
{
	unsigned available = RENDER_RES_NONE;

	if (!ctx || !ctx->resources)
		return false;

	if (ctx->resources->scene_fbo)
		available |= RENDER_RES_SCENE_COLOR;
	if (ctx->resources->scene_depth_tex)
		available |= RENDER_RES_SCENE_DEPTH;
	if (ctx->resources->composite_fbo)
		available |= RENDER_RES_COMPOSITE_COLOR;
	if (ctx->resources->shadow_sun_depth_tex)
		available |= RENDER_RES_SHADOW_SUN_DEPTH;
	if (ctx->resources->fogvol_history_tex)
		available |= RENDER_RES_FOGVOL_HISTORY;
	if (ctx->resources->velocity_tex)
		available |= RENDER_RES_VELOCITY;

	return (available & required) == required;
}

static qboolean R_FG_PassWhenFogVolEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_fogvol
		&& R_FG_HasResources (ctx, RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_FOGVOL_HISTORY);
}

static qboolean R_FG_FogVolNeedsSunShadow (const RenderPassContext *ctx)
{
	return ctx
		&& ctx->frame_plan
		&& ctx->frame_plan->run_shadowmaps
		&& ctx->resources
		&& ctx->resources->shadow_sun_depth_tex
		&& (r_fogvol_shadow.value > 0.f)
		&& (r_fogvol_froxel_sun.value > 0.f);
}

static qboolean R_FG_PassWhenFogVolWithShadow (const RenderPassContext *ctx)
{
	return R_FG_PassWhenFogVolEnabled (ctx)
		&& R_FG_FogVolNeedsSunShadow (ctx);
}

static qboolean R_FG_PassWhenFogVolNoShadow (const RenderPassContext *ctx)
{
	return R_FG_PassWhenFogVolEnabled (ctx)
		&& !R_FG_FogVolNeedsSunShadow (ctx);
}

static void R_FG_ExecFogVol (RenderPassContext *ctx)
{
	(void)ctx;
	r_fogvol_update_called++;
	R_FogVol_BuildList ();
	r_fogvol_draw_called++;
	R_FogVol_Render ();
}

static void R_FG_ExecFogVolNoShadow (RenderPassContext *ctx)
{
	if (r_framegraph_debug.value > 0.f)
	{
		if (ctx && ctx->resources && ctx->resources->shadow_sun_depth_tex == 0)
			Con_DPrintf ("FrameGraph: fogvol using no-shadow resource contract (shadow depth optional)\n");
		else if (ctx && ctx->resources && ctx->resources->shadow_sun_depth_tex != 0)
			Con_DPrintf ("FrameGraph: fogvol selected no-shadow contract while shadow depth exists (feature toggle path)\n");
	}

	R_FG_ExecFogVol (ctx);
}

static const RenderPassDesc s_fogvol_framegraph_pass_shadow = {
	"Render fog volumes (shadowed)",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_SHADOW_SUN_DEPTH | RENDER_RES_FOGVOL_HISTORY | RENDER_RES_VELOCITY | RENDER_RES_FOGVOL_INPUTS,
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_FOGVOL_HISTORY,
	1u << 0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	R_FG_PassWhenFogVolWithShadow,
	R_FG_ExecFogVol,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_FOG
};

static const RenderPassDesc s_fogvol_framegraph_pass_noshadow = {
	"Render fog volumes (no-shadow)",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_FOGVOL_HISTORY | RENDER_RES_VELOCITY | RENDER_RES_FOGVOL_INPUTS,
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_FOGVOL_HISTORY,
	1u << 0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	R_FG_PassWhenFogVolNoShadow,
	R_FG_ExecFogVolNoShadow,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_FOG
};

static void R_FG_ExecSSAOFogHandoff (RenderPassContext *ctx)
{
	(void)ctx;
	/* Capture both global fog parameters and fogvol composite validity so
	 * postprocess SSAO suppression uses deterministic source selection. */
	R_SSAO_CaptureFogState (&r_framedata, &r_ssao_fog_state);
}

static const RenderPassDesc s_ssao_fog_handoff_framegraph_pass = {
	"Capture fog handoff",
	/* Captures CPU-side global fog + fogvol availability into SSAO handoff data. */
	RENDER_RES_NONE,
	RENDER_RES_SSAO_FOG_STATE,
	0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	NULL,
	R_FG_ExecSSAOFogHandoff
};

static qboolean R_FG_PassWhenPostprocessEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_postprocess
		&& R_FG_HasResources (ctx, RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH);
}

static void R_FG_ExecPostprocess (RenderPassContext *ctx)
{
	GL_PostProcess (ctx ? ctx->resources : NULL);
}

static const RenderPassDesc s_postprocess_framegraph_pass = {
	"Postprocess",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_SSAO_FOG_STATE,
	RENDER_RES_COMPOSITE_COLOR,
	1u << 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_VIEWPORT_FULL_WINDOW,
	R_FG_PassWhenPostprocessEnabled,
	R_FG_ExecPostprocess,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_POST
};

static qboolean R_FG_PassWhenViewmodelEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_viewmodel;
}

static void R_FG_ExecViewmodel (RenderPassContext *ctx)
{
	(void)ctx;
	R_DrawViewModel ();
}

static const RenderPassDesc s_viewmodel_framegraph_pass = {
	"Draw viewmodel",
	RENDER_RES_COMPOSITE_COLOR,
	RENDER_RES_COMPOSITE_COLOR,
	1u << 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_VIEWPORT_VIEW_RECT,
	R_FG_PassWhenViewmodelEnabled,
	R_FG_ExecViewmodel,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_OVERLAY
};

static qboolean R_FG_PassWhenPolyblendEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_polyblend;
}

static void R_FG_ExecPolyblend (RenderPassContext *ctx)
{
	(void)ctx;
	V_PolyBlend ();
}

static const RenderPassDesc s_polyblend_framegraph_pass = {
	"Polyblend",
	RENDER_RES_COMPOSITE_COLOR,
	RENDER_RES_COMPOSITE_COLOR,
	1u << 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_VIEWPORT_VIEW_RECT,
	R_FG_PassWhenPolyblendEnabled,
	R_FG_ExecPolyblend,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_OVERLAY
};

static qboolean R_FG_PassWhenStorePrevEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_store_prev;
}

static void R_FG_ExecStorePrev (RenderPassContext *ctx)
{
	(void)ctx;
	r_frame_rendered_this_update = true;
	R_StorePrevFrameState ();
}

static const RenderPassDesc s_storeprev_framegraph_pass = {
	"Store previous frame",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
	RENDER_RES_NONE,
	1u << 0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	R_FG_PassWhenStorePrevEnabled,
	R_FG_ExecStorePrev
};

void R_RegisterFrameGraphPasses (void)
{
	(void)R_FrameGraph_AddPass (&s_setup_view_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_shadowmaps_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_scene_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_warp_resolve_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_fogvol_prepare_framegraph_pass);
	R_Decals_RegisterFrameGraphPasses ();
	(void)R_FrameGraph_AddPass (&s_fogvol_framegraph_pass_shadow);
	(void)R_FrameGraph_AddPass (&s_fogvol_framegraph_pass_noshadow);
	(void)R_FrameGraph_AddPass (&s_ssao_fog_handoff_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_postprocess_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_viewmodel_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_polyblend_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_storeprev_framegraph_pass);
}

/*
================
R_RenderView
================
*/
void R_RenderView (void)
{
	double	time1, time2;

	if (r_norefresh.value)
		return;

	if (!cl.worldmodel)
		Sys_Error ("R_RenderView: NULL worldmodel");

	time1 = 0; /* avoid compiler warning */
	if (r_speeds.value)
	{
		glFinish ();
		time1 = Sys_DoubleTime ();

		//johnfitz -- rendering statistics
		rs_brushpolys = rs_aliaspolys = rs_skypolys =
			rs_dynamiclightmaps = rs_aliaspasses = rs_skypasses = rs_brushpasses = 0;
	}
        else if (gl_finish.value)
                glFinish ();

	R_InvalidateGodraysFrameCache ();
	R_FrameGraph_RenderView ();
	if (r_gl_state_validate.value > 0.f && r_framegraph_debug.value > 0.f)
	{
		Con_DPrintf ("fogvol_update_called=%d r_fogvol=%.1f\n", r_fogvol_update_called, r_fogvol.value);
		Con_DPrintf ("fogvol_draw_called=%d r_fogvol=%.1f\n", r_fogvol_draw_called, r_fogvol.value);
	}

	//johnfitz -- modified r_speeds output
	time2 = Sys_DoubleTime ();
	if (r_pos.value)
		Con_Printf ("x %i y %i z %i (pitch %i yaw %i roll %i)\n",
			(int)cl_entities[cl.viewentity].origin[0],
			(int)cl_entities[cl.viewentity].origin[1],
			(int)cl_entities[cl.viewentity].origin[2],
			(int)cl.viewangles[PITCH],
			(int)cl.viewangles[YAW],
			(int)cl.viewangles[ROLL]);
	else if (r_speeds.value == 2)
		Con_Printf ("%3i ms  %4i/%4i wpoly %4i/%4i epoly %3i lmap %4i/%4i sky %1.1f mtex\n",
			(int)((time2 - time1) * 1000),
			rs_brushpolys,
			rs_brushpasses,
			rs_aliaspolys,
			rs_aliaspasses,
			rs_dynamiclightmaps,
			rs_skypolys,
			rs_skypasses,
			TexMgr_FrameUsage ());
	else if (r_speeds.value)
		Con_Printf ("%3i ms  %4i wpoly %4i epoly %3i lmap\n",
			(int)((time2 - time1) * 1000),
			rs_brushpolys,
			rs_aliaspolys,
			rs_dynamiclightmaps);
	//johnfitz
}
