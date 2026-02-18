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
#include "client/cl_postfx.h"
#include "renderer/r_postfx.h"
#include "renderer/r_decals.h"
#include "renderer/r_envlight.h"
#include "renderer/r_dlight_pool.h"
#include "opengl/gl_lightgrid.h"
#include "mat_shader.h"
#include <float.h>
#include <math.h>

#define NOISESCALE     (1.0f / 127.0f)

extern gltexture_t *lightmap_dir_texture;

qboolean	r_cache_thrash;		// compatability

gpuframedata_t r_framedata;

float r_autoexposure_debug_exposure = 1.f;
float r_autoexposure_debug_luminance = 0.f;

vec3_t* r_pointfile;

int			r_visframecount;	// bumped when going to a new PVS
int			r_framecount;		// used for dlight push checking

static entity_t* cl_sorted_visedicts[MAX_VISEDICTS + 1];
static int cl_modtype_ofs[mod_numtypes * 2 + 1];

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
static qboolean r_dlight_buffered_frame = false;

extern cvar_t r_clustered_lighting;

static qboolean R_DlightsAdditivePassEnabled (void)
{
	return (r_clustered_lighting.value <= 0.f);
}

static void R_DlightLogf (const char *pass, const char *fmt, ...)
{
	(void)pass;
	(void)fmt;
}


typedef struct godrays_stabilization_s
{
	qboolean	valid;
	vec2_t		smoothed_pos;
	double		last_time;
	vec3_t		last_viewangles;
} godrays_stabilization_t;

static godrays_stabilization_t r_godrays_stabilization;

sun_state_t	r_sun = {
	false,
	{ 0.f, 0.f, -1.f },
	{ 1.f, 1.f, 1.f },
	1.f,
	{ 0.f, 0.f, 8192.f },
	SUN_SOURCE_NONE,
	{ 0.f, 0.f, -1.f }
};

typedef struct atmosphere_settings_s
{
	int mode;
	qboolean enabled_fog;
	qboolean enabled_shafts;
	qboolean enabled_volumetrics;
	qboolean shadow_enable;
	float density;
	float anisotropy;
	float steps;
	float history_weight;
	float debug_mode;
	float debug_slice;
	float noise_scale;
	float noise_drift;
	float height_base;
} atmosphere_settings_t;

#define MAX_FOG_VOLUMES 64

typedef enum fog_volume_shape_e
{
	FOG_VOLUME_BOX = 0,
	FOG_VOLUME_SPHERE = 1
} fog_volume_shape_t;

typedef enum fog_volume_blend_e
{
	FOG_VOLUME_BLEND_ADD = 0,
	FOG_VOLUME_BLEND_OVERRIDE = 1
} fog_volume_blend_t;

typedef struct fog_volume_s
{
	qboolean used;
	fog_volume_shape_t shape;
	fog_volume_blend_t blend;
	vec3_t mins;
	vec3_t maxs;
	vec3_t origin;
	float radius;
	float density;
	vec3_t color;
	float height_falloff;
	float noise_amount;
	float noise_scale;
	float anisotropy;
	int flags;
} fog_volume_t;

static fog_volume_t r_fog_volumes[MAX_FOG_VOLUMES];
static int r_fog_volume_count;

typedef struct packed_fog_volume_s
{
	vec4_t data0;	/* xyz center, w radius */
	vec4_t data1;	/* xyz extents, w density */
	vec4_t data2;	/* rgb color, w height_falloff */
	vec4_t data3;	/* x noise amount, y noise scale, z anisotropy, w shape */
	vec4_t data4;	/* x blend, y flags */
} packed_fog_volume_t;

static packed_fog_volume_t r_fog_volume_gpu[MAX_FOG_VOLUMES];
static GLuint r_fog_volume_ssbo;

typedef struct atmosphere_runtime_s
{
	atmosphere_settings_t settings;
	qboolean settings_valid;
	qboolean fog_enabled_for_scene;
	qboolean froxel_enabled;
	GLuint godrays_texture;
	GLuint godrays_mask;
	qboolean godrays_ready;
	int logged_frame;
} atmosphere_runtime_t;

static atmosphere_runtime_t r_atmosphere;

typedef struct framesetup_s
{
        GLuint          scene_fbo;
        GLuint          oit_fbo;
        qboolean        composite_ready;
} framesetup_t;

static framesetup_t framesetup;

static const float r_identity_mat4[16] = {
		1.f, 0.f, 0.f, 0.f,
		0.f, 1.f, 0.f, 0.f,
		0.f, 0.f, 1.f, 0.f,
		0.f, 0.f, 0.f, 1.f
};

extern cvar_t r_state_debug;
extern cvar_t r_state_debug_filter;
extern cvar_t r_gl_verify_program;

static void GL_LogErrorIfDeveloper (const char *label)
{
	GLenum err = glGetError ();
	if (err != GL_NO_ERROR)
		Con_DPrintf ("GL error after %s: 0x%04X\n", label, err);
}

static qboolean GL_StateDebugMatchesFilter (const char *label)
{
	const char *filter = r_state_debug_filter.string;

	if (!filter || !*filter)
		return true;

	return (q_strcasestr (label, filter) != NULL);
}

static void GL_DumpRenderState (const char *label)
{
	GLint viewport[4], scissor_box[4];
	GLint draw_fbo, read_fbo, program, vao;
	GLint blend_src_rgb, blend_dst_rgb, depth_func;
	GLboolean depth_mask;
	GLboolean color_mask[4];
	qboolean scissor, depth, blend, cull;

	if (r_state_debug.value <= 0.f)
		return;
	if (!GL_StateDebugMatchesFilter (label))
		return;

	glGetIntegerv (GL_VIEWPORT, viewport);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vao);
	scissor = glIsEnabled (GL_SCISSOR_TEST);
	depth = glIsEnabled (GL_DEPTH_TEST);
	blend = glIsEnabled (GL_BLEND);
	cull = glIsEnabled (GL_CULL_FACE);
	glGetIntegerv (GL_BLEND_SRC_RGB, &blend_src_rgb);
	glGetIntegerv (GL_BLEND_DST_RGB, &blend_dst_rgb);
	glGetIntegerv (GL_DEPTH_FUNC, &depth_func);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &depth_mask);
	glGetBooleanv (GL_COLOR_WRITEMASK, color_mask);

	Con_Printf (
		"STATEDBG frame=%d pass=%s vp=(%d %d %d %d) scissor=%d box=(%d %d %d %d) "
		"draw_fbo=%d read_fbo=%d prog=%d(%s) vao=%d depth=%d blend=%d cull=%d blendfunc=(%d,%d) depthfunc=0x%04x depthmask=%d colormask=(%d%d%d%d)\n",
		r_framecount,
		label,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		draw_fbo, read_fbo, program, GL_GetProgramDebugName ((GLuint)program), vao,
		depth, blend, cull,
		blend_src_rgb, blend_dst_rgb,
		(unsigned)depth_func,
		depth_mask ? 1 : 0,
		color_mask[0] ? 1 : 0, color_mask[1] ? 1 : 0, color_mask[2] ? 1 : 0, color_mask[3] ? 1 : 0);
}


static void R_LogWorldDlightState (const char *marker)
{
	GLint draw_fbo = 0;
	GLint draw_buf[4] = { 0, 0, 0, 0 };
	GLint attachment[4] = { 0, 0, 0, 0 };
	GLint attachment_type[4] = { 0, 0, 0, 0 };
	GLint program = 0;
	GLint depth_func = 0;
	GLboolean color_mask[4] = { 0, 0, 0, 0 };
	GLboolean depth_mask = GL_FALSE;
	qboolean depth = false;
	qboolean blend = false;
	GLint blend_src_rgb = 0, blend_dst_rgb = 0;

	if (r_state_debug.value <= 0.f)
	{
		if (q_strcasecmp (marker, "WORLD_DLIGHT_BEGIN") || r_dlight_debug_visualize.value < 2.f)
			return;
	}
	else if (!GL_StateDebugMatchesFilter (marker))
	{
		return;
	}

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_DRAW_BUFFER0, &draw_buf[0]);
	glGetIntegerv (GL_DRAW_BUFFER1, &draw_buf[1]);
	glGetIntegerv (GL_DRAW_BUFFER2, &draw_buf[2]);
	glGetIntegerv (GL_DRAW_BUFFER3, &draw_buf[3]);
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetBooleanv (GL_COLOR_WRITEMASK, color_mask);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &depth_mask);
	depth = glIsEnabled (GL_DEPTH_TEST);
	blend = glIsEnabled (GL_BLEND);
	glGetIntegerv (GL_DEPTH_FUNC, &depth_func);
	glGetIntegerv (GL_BLEND_SRC_RGB, &blend_src_rgb);
	glGetIntegerv (GL_BLEND_DST_RGB, &blend_dst_rgb);

	if (draw_fbo != 0)
	{
		int i;
		for (i = 0; i < 4; ++i)
		{
			GLenum attach = GL_COLOR_ATTACHMENT0 + i;
			GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, attach, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attachment[i]);
			GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, attach, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachment_type[i]);
		}
	}

	Con_Printf (
		"STATEDBG frame=%d pass=%s draw_fbo=%d drawbufs=(0x%X,0x%X,0x%X,0x%X) att=(%d/%d,%d/%d,%d/%d,%d/%d) colormask=(%d%d%d%d) "
		"depthtest=%d depthfunc=0x%04X depthmask=%d blend=%d blendfunc=(%d,%d) prog=%d(%s) numlights=%u\n",
		r_framecount,
		marker,
		draw_fbo,
		draw_buf[0], draw_buf[1], draw_buf[2], draw_buf[3],
		attachment[0], attachment_type[0], attachment[1], attachment_type[1], attachment[2], attachment_type[2], attachment[3], attachment_type[3],
		color_mask[0] ? 1 : 0, color_mask[1] ? 1 : 0, color_mask[2] ? 1 : 0, color_mask[3] ? 1 : 0,
		depth ? 1 : 0, depth_func, depth_mask ? 1 : 0,
		blend ? 1 : 0, blend_src_rgb, blend_dst_rgb,
		program, GL_GetProgramDebugName ((GLuint)program),
		r_framedata.numlights);

	if (!q_strcasecmp (marker, "WORLD_DLIGHT_BEGIN"))
	{
		dlight_filter_debug_t filter_stats;
		dlight_debug_entry_t entries[4];
		DLightPool_GetFilterDebug (&filter_stats, entries);
		Con_Printf ("STATEDBG dlight_filter total_active=%d lifetime_radius=%d world=%d pvs=%d frustum=%d budget=%d reject_lifetime=%d reject_world=%d reject_pvs=%d reject_frustum=%d reject_budget=%d\n",
			filter_stats.total_active,
			filter_stats.pass_lifetime_radius,
			filter_stats.pass_world_flag,
			filter_stats.pass_pvs,
			filter_stats.pass_frustum,
			filter_stats.pass_budget,
			filter_stats.rejected_lifetime_radius,
			filter_stats.rejected_world_flag,
			filter_stats.rejected_pvs,
			filter_stats.rejected_frustum,
			filter_stats.rejected_budget);
		for (int i = 0; i < 4; i++)
		{
			const dlight_debug_entry_t *entry = &entries[i];
			if (!entry->captured)
				continue;
			Con_Printf ("STATEDBG dlight[%d] id=%d org=(%.1f %.1f %.1f) radius=%.1f base=%.1f color=(%.2f %.2f %.2f) die=%.3f flags=0x%X bbox=(%.1f %.1f %.1f)-(%.1f %.1f %.1f) leaf=%d has_leaf=%d in_pvs=%d in_frustum=%d reason=%s\n",
				i,
				entry->id,
				entry->origin[0], entry->origin[1], entry->origin[2],
				entry->radius, entry->baseradius,
				entry->color[0], entry->color[1], entry->color[2],
				entry->die, entry->flags,
				entry->mins[0], entry->mins[1], entry->mins[2],
				entry->maxs[0], entry->maxs[1], entry->maxs[2],
				entry->leaf_index, entry->has_leaf ? 1 : 0,
				entry->in_pvs ? 1 : 0, entry->in_frustum ? 1 : 0,
				DLightPool_RejectReasonName (entry->reason));
		}
	}
}

void R_ResetViewportAndScissorFullscreen (const char *label)
{
	glDisable (GL_SCISSOR_TEST);
	glViewport (glx, gly, glwidth, glheight);
	glScissor (glx, gly, glwidth, glheight);
	GL_DumpRenderState (label);
}

static void R_VerifyProgramCache (const char *label)
{
	GLint gl_program = 0;
	GLuint cached_program;

	if (r_gl_verify_program.value <= 0.f)
		return;

	glGetIntegerv (GL_CURRENT_PROGRAM, &gl_program);
	cached_program = GL_GetCurrentProgramCached ();
	if ((GLuint)gl_program == cached_program)
		return;

	Con_DWarning ("R_VerifyProgramCache(%s): cache desync cached=%u(%s) gl_current=%d(%s)\n",
		label,
		(unsigned)cached_program,
		GL_GetProgramDebugName (cached_program),
		(int)gl_program,
		GL_GetProgramDebugName ((GLuint)gl_program));
}

void R_StateDebugMark (const char *label)
{
	GL_DumpRenderState (label);
	R_VerifyProgramCache (label);
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

static float GL_TemperedOverbright (float overbright)
{
	if (overbright <= 1.f)
		return 1.f;

	/*
	* Compress the raw 2^n scaling so the boost favours highlights over the
	* rest of the scene.  A sub-linear exponent keeps very bright areas hot
	* while easing off on mid and dark tones to avoid washing out the image.
	*/
	const float exponent = 0.75f;
	float tempered = powf (overbright, exponent);

	return q_max (tempered, 1.f);
}

static qboolean MatrixInverse4x4(const float m[16], float out[16])
{
    float inv[16];
    float det;

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
            m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15]
            - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15]
            + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14]
            - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15]
            - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15]
            + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15]
            - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14]
            + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15]
            + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15]
            - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15]
            + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14]
            - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
            - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
            + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
            - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
            + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-8f)
	return false;

    det = 1.f / det;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] * det;
    return true;
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
cvar_t	r_state_debug = { "r_state_debug", "0", CVAR_NONE };
cvar_t	r_state_debug_filter = { "r_state_debug_filter", "", CVAR_NONE };
cvar_t	r_debug_colorspace = { "r_debug_colorspace", "0", CVAR_NONE };
cvar_t	r_color_midtone = { "r_color_midtone", "1.0", CVAR_ARCHIVE };
cvar_t	r_color_contrast = { "r_color_contrast", "1.0", CVAR_ARCHIVE };
cvar_t	r_color_saturation = { "r_color_saturation", "1.05", CVAR_ARCHIVE };
cvar_t	r_lightmap_colorspace = { "r_lightmap_colorspace", "srgb", CVAR_ARCHIVE };
cvar_t	r_lightmap_colorspace_debug = { "r_lightmap_colorspace_debug", "0", CVAR_NONE };
cvar_t	r_wateralpha = { "r_wateralpha","1",CVAR_ARCHIVE };
cvar_t	r_litwater = { "r_litwater","1",CVAR_NONE };
cvar_t	r_dynamic = { "r_dynamic","1",CVAR_ARCHIVE };
cvar_t  r_dlight_enable = { "r_dlight_enable", "1", CVAR_ARCHIVE };
cvar_t  r_dlight_scale = { "r_dlight_scale", "1.0", CVAR_ARCHIVE };
cvar_t  r_dlight_radius_scale = { "r_dlight_radius_scale", "1.0", CVAR_ARCHIVE };
cvar_t  r_dlight_debug_visualize = { "r_dlight_debug_visualize", "0", CVAR_NONE };
cvar_t  r_clustered_lighting = { "r_clustered_lighting", "0", CVAR_ARCHIVE };
cvar_t  r_clustered_tilesize = { "r_clustered_tilesize", "16", CVAR_ARCHIVE };
cvar_t  r_clustered_zslices = { "r_clustered_zslices", "24", CVAR_ARCHIVE };
cvar_t  r_clustered_maxindices = { "r_clustered_maxindices", "262144", CVAR_ARCHIVE };
cvar_t  r_clustered_debug = { "r_clustered_debug", "0", CVAR_NONE };
cvar_t  r_clustered_log = { "r_clustered_log", "1", CVAR_NONE };
cvar_t	r_shadows = { "r_shadows", "0", CVAR_ARCHIVE };
cvar_t	r_shadow_sun = { "r_shadow_sun", "1", CVAR_ARCHIVE };
cvar_t	r_shadowmap_size = { "r_shadowmap_size", "2048", CVAR_ARCHIVE };
cvar_t	r_shadow_bias = { "r_shadow_bias", "0.001", CVAR_ARCHIVE };
cvar_t	r_shadow_normalbias = { "r_shadow_normalbias", "1.0", CVAR_ARCHIVE };
cvar_t	r_shadow_bias_mdl = { "r_shadow_bias_mdl", "0.001", CVAR_ARCHIVE };
cvar_t	r_shadow_normalbias_mdl = { "r_shadow_normalbias_mdl", "1.0", CVAR_ARCHIVE };
cvar_t	r_shadow_pcf = { "r_shadow_pcf", "1", CVAR_ARCHIVE };
cvar_t	r_shadow_pcf_taps = { "r_shadow_pcf_taps", "4", CVAR_ARCHIVE };
cvar_t	r_shadow_debug = { "r_shadow_debug", "0", CVAR_NONE };
cvar_t	r_shadow_csm_debug = { "r_shadow_csm_debug", "0", CVAR_NONE };
cvar_t	r_shadow_sun_dir = { "r_shadow_sun_dir", "0.3 0.5 -1.0", CVAR_ARCHIVE };
cvar_t	r_shadow_sun_color = { "r_shadow_sun_color", "1 1 1", CVAR_ARCHIVE };
cvar_t	r_shadow_sun_intensity = { "r_shadow_sun_intensity", "1.0", CVAR_ARCHIVE };
cvar_t	r_sun_fallback = { "r_sun_fallback", "1", CVAR_ARCHIVE };
cvar_t	r_sun_force_fallback = { "r_sun_force_fallback", "0", CVAR_ARCHIVE };
cvar_t	r_sun_allow_no_sun = { "r_sun_allow_no_sun", "0", CVAR_ARCHIVE };
cvar_t	r_sun_distance = { "r_sun_distance", "10000", CVAR_ARCHIVE };
cvar_t	r_shadow_twosided_mdl = { "r_shadow_twosided_mdl", "0", CVAR_ARCHIVE };
cvar_t	r_shadow_dlights = { "r_shadow_dlights", "0", CVAR_ARCHIVE };
cvar_t	r_shadow_dlight_max = { "r_shadow_dlight_max", "2", CVAR_ARCHIVE };
cvar_t	r_shadow_dlight_size = { "r_shadow_dlight_size", "256", CVAR_ARCHIVE };
cvar_t	r_shadow_dlight_distance = { "r_shadow_dlight_distance", "1024", CVAR_ARCHIVE };
cvar_t	r_shadow_dlight_bias = { "r_shadow_dlight_bias", "0.0025", CVAR_ARCHIVE };
cvar_t	r_shadow_dlight_pcf_taps = { "r_shadow_dlight_pcf_taps", "4", CVAR_ARCHIVE };
cvar_t	r_shadow_lightgrid = { "r_shadow_lightgrid", "0", CVAR_ARCHIVE };
cvar_t	r_shadow_lightgrid_mode = { "r_shadow_lightgrid_mode", "1", CVAR_ARCHIVE };
cvar_t	r_shadow_log = { "r_shadow_log", "0", CVAR_NONE };
cvar_t	r_shadow_log_rate = { "r_shadow_log_rate", "60", CVAR_NONE };
cvar_t	r_shadow_log_gl = { "r_shadow_log_gl", "0", CVAR_NONE };
cvar_t	r_shadow_log_dump = { "r_shadow_log_dump", "0", CVAR_NONE };
cvar_t	r_shadow_log_file = { "r_shadow_log_file", "0", CVAR_NONE };
cvar_t	r_shadow_validate = { "r_shadow_validate", "0", CVAR_NONE };
cvar_t	r_shadow_coord_debug = { "r_shadow_coord_debug", "0", CVAR_NONE };
cvar_t	r_shadow_comparefunc = { "r_shadow_comparefunc", "0", CVAR_ARCHIVE };
cvar_t	r_gl_verify_program = { "r_gl_verify_program", "0", CVAR_NONE };
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
cvar_t	r_ae_min_scene_luma = { "r_ae_min_scene_luma", "0.02", CVAR_ARCHIVE };
cvar_t	r_ae_min_exposure = { "r_ae_min_exposure", "0.25", CVAR_ARCHIVE };
cvar_t	r_ae_max_exposure = { "r_ae_max_exposure", "8.0", CVAR_ARCHIVE };
cvar_t	r_exposure_bias = { "r_exposure_bias", "1.0", CVAR_ARCHIVE };
cvar_t	r_exposure_min = { "r_exposure_min", "0.85", CVAR_ARCHIVE };
cvar_t	r_exposure_max = { "r_exposure_max", "1.15", CVAR_ARCHIVE };
cvar_t	r_exposure_speed_up = { "r_exposure_speed_up", "0.6", CVAR_ARCHIVE };
cvar_t	r_exposure_speed_down = { "r_exposure_speed_down", "0.3", CVAR_ARCHIVE };
cvar_t	r_exposure_lock = { "r_exposure_lock", "0", CVAR_ARCHIVE };
cvar_t	r_exposure_debug = { "r_exposure_debug", "0", CVAR_NONE };
cvar_t	r_tonemap_black_lift = { "r_tonemap_black_lift", "0.0", CVAR_ARCHIVE };
cvar_t	r_tonemap_black_lift_strength = { "r_tonemap_black_lift_strength", "1.0", CVAR_ARCHIVE };
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
// r_ssao_debug modes: 0 off, 1 raw AO, 2 AO*fog, 3 fog factor, 4 depth raw, 5 view-space Z, 6 view-space position, 7 normals, 8 noise, 9 sample hit ratio, 10 AO raw, 11 blur debug, 12 AO mask, 13 fog transmittance, 14 fog-damped AO.
cvar_t	r_ssao_debug = { "r_ssao_debug", "0", CVAR_NONE };
cvar_t	r_ssao_debug_far = { "r_ssao_debug_far", "4096", CVAR_NONE };
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
cvar_t	r_ssao_fog_power = { "r_ssao_fog_power", "1.5", CVAR_ARCHIVE };
cvar_t	r_ssao_max_distance = { "r_ssao_max_distance", "1024", CVAR_ARCHIVE };

// Unified AO controls: r_ao_method 0=off, 1=ASSAO-like, 2=GTAO-like.
cvar_t	r_ao_method = { "r_ao_method", "1", CVAR_ARCHIVE };
cvar_t	r_ao_quality = { "r_ao_quality", "1", CVAR_ARCHIVE };
cvar_t	r_ao_debug = { "r_ao_debug", "0", CVAR_NONE };
cvar_t	r_ao_radius = { "r_ao_radius", "24", CVAR_ARCHIVE };
cvar_t	r_ao_power = { "r_ao_power", "1.5", CVAR_ARCHIVE };
cvar_t	r_ao_intensity = { "r_ao_intensity", "1.0", CVAR_ARCHIVE };
cvar_t	r_ao_halfres = { "r_ao_halfres", "1", CVAR_ARCHIVE };
cvar_t	r_ao_applymode = { "r_ao_applymode", "0", CVAR_ARCHIVE };
cvar_t	r_ao_bentnormals = { "r_ao_bentnormals", "0", CVAR_ARCHIVE };
cvar_t	r_ao_temporal = { "r_ao_temporal", "0", CVAR_ARCHIVE };
cvar_t	r_ao_timing = { "r_ao_timing", "0", CVAR_NONE };

/* Unified AO controls (authoritative). Legacy r_ssao/r_ao cvars are compatibility aliases. */
cvar_t	s_ao = { "s_ao", "1", CVAR_ARCHIVE };
cvar_t	s_ao_mode = { "s_ao_mode", "2", CVAR_ARCHIVE }; /* 0=off, 1=ssao(legacy), 2=assao, 3=gtao */
cvar_t	s_ao_quality = { "s_ao_quality", "1", CVAR_ARCHIVE };
cvar_t	s_ao_radius = { "s_ao_radius", "24", CVAR_ARCHIVE };
cvar_t	s_ao_strength = { "s_ao_strength", "1.0", CVAR_ARCHIVE };
cvar_t	s_ao_power = { "s_ao_power", "1.5", CVAR_ARCHIVE };
cvar_t	s_ao_blur = { "s_ao_blur", "1", CVAR_ARCHIVE };
cvar_t	s_ao_halfres = { "s_ao_halfres", "1", CVAR_ARCHIVE };
cvar_t	s_ao_debug = { "s_ao_debug", "0", CVAR_NONE }; /* 0=off,1=ao_only,2=inputs,3=depth_lin,4=normals,5=edges,6=history */
cvar_t	s_ao_temporal = { "s_ao_temporal", "0", CVAR_ARCHIVE };

/* Legacy compatibility aliases; real envlight controls live in renderer/r_envlight.c. */
cvar_t	r_reflection_probes = { "r_reflection_probes", "0", CVAR_ARCHIVE };
cvar_t	r_reflection_probe_debug = { "r_reflection_probe_debug", "0", CVAR_NONE };
cvar_t	r_lightgrid_directional = { "r_lightgrid_directional", "1", CVAR_ARCHIVE };
cvar_t	r_lighting_debug = { "r_lighting_debug", "0", CVAR_NONE };

cvar_t	r_godrays = { "r_godrays", "0", CVAR_ARCHIVE };
cvar_t	r_godrays_quality = { "r_godrays_quality", "1", CVAR_ARCHIVE };
cvar_t	r_godrays_debug = { "r_godrays_debug", "0", CVAR_NONE };
cvar_t	r_sun_debug = { "r_sun_debug", "0", CVAR_NONE };
/* Unified fog controls. Froxel fog is the only volumetric fog backend. */
cvar_t	r_fog_enable = { "r_fog_enable", "1", CVAR_ARCHIVE };
cvar_t	r_fog_density = { "r_fog_density", "1.0", CVAR_ARCHIVE };
cvar_t	r_fog_quality = { "r_fog_quality", "1", CVAR_ARCHIVE };
cvar_t	r_fog_temporal = { "r_fog_temporal", "1", CVAR_ARCHIVE };
cvar_t	r_fog_history_weight = { "r_fog_history_weight", "0.85", CVAR_ARCHIVE };
cvar_t	r_fog_jitter = { "r_fog_jitter", "1", CVAR_ARCHIVE };
cvar_t	r_fog_anisotropy = { "r_fog_anisotropy", "0.2", CVAR_ARCHIVE };
cvar_t	r_fog_g = { "r_fog_g", "0.76", CVAR_ARCHIVE };
cvar_t	r_fog_albedo = { "r_fog_albedo", "0.92", CVAR_ARCHIVE };
cvar_t	r_fog_sun_strength = { "r_fog_sun_strength", "1.0", CVAR_ARCHIVE };
cvar_t	r_fog_height_falloff = { "r_fog_height_falloff", "0.0025", CVAR_ARCHIVE };
cvar_t	r_fog_height_base = { "r_fog_height_base", "0", CVAR_ARCHIVE };
cvar_t	r_fog_noise_strength = { "r_fog_noise_strength", "0.15", CVAR_ARCHIVE };
cvar_t	r_fog_noise_scale = { "r_fog_noise_scale", "0.06", CVAR_ARCHIVE };
cvar_t	r_fog_noise_drift = { "r_fog_noise_drift", "0.15", CVAR_ARCHIVE };
cvar_t	r_fog_volume_max_active = { "r_fog_volume_max_active", "16", CVAR_ARCHIVE };
cvar_t	r_fog_debug = { "r_fog_debug", "0", CVAR_NONE };
cvar_t	r_fog_debug_sun = { "r_fog_debug_sun", "0", CVAR_NONE };
cvar_t	r_fog_validate = { "r_fog_validate", "0", CVAR_NONE };
cvar_t	r_fog_debug_density = { "r_fog_debug_density", "0", CVAR_NONE };
cvar_t	r_fog_debug_transmittance = { "r_fog_debug_transmittance", "0", CVAR_NONE };
cvar_t	r_fog_debug_scattering = { "r_fog_debug_scattering", "0", CVAR_NONE };
cvar_t	r_fog_debug_light_injection = { "r_fog_debug_light_injection", "0", CVAR_NONE };
cvar_t	r_fog_debug_step_length = { "r_fog_debug_step_length", "0", CVAR_NONE };
cvar_t	r_fog_debug_mode = { "r_fog_debug_mode", "0", CVAR_NONE };
cvar_t	r_fog_debug_slice = { "r_fog_debug_slice", "0", CVAR_NONE };
cvar_t	r_fog_debug_showgrid = { "r_fog_debug_showgrid", "0", CVAR_NONE };
cvar_t	r_fog_debug_composite_stage = { "r_fog_debug_composite_stage", "0", CVAR_NONE };
cvar_t	r_fog_debug_volume_bounds = { "r_fog_debug_volume_bounds", "0", CVAR_NONE };

cvar_t	r_fog_log = { "r_fog_log", "0", CVAR_NONE };
cvar_t	r_fog_froxel_res = { "r_fog_froxel_res", "1", CVAR_ARCHIVE };
cvar_t	r_fog_zslices = { "r_fog_zslices", "64", CVAR_ARCHIVE };

cvar_t	r_vignette = { "r_vignette", "0.15", CVAR_ARCHIVE };
cvar_t	r_vignette_radius_inner = { "r_vignette_radius_inner", "0.8", CVAR_ARCHIVE };
cvar_t	r_vignette_radius_outer = { "r_vignette_radius_outer", "2.0", CVAR_ARCHIVE };
cvar_t	r_vignette_falloff = { "r_vignette_falloff", "2.0", CVAR_ARCHIVE };
cvar_t	r_vignette_color_r = { "r_vignette_color_r", "0.0", CVAR_ARCHIVE };
cvar_t	r_vignette_color_g = { "r_vignette_color_g", "0.0", CVAR_ARCHIVE };
cvar_t	r_vignette_color_b = { "r_vignette_color_b", "0.0", CVAR_ARCHIVE };
cvar_t	r_vignette_blend_mode = { "r_vignette_blend_mode", "0", CVAR_ARCHIVE };
cvar_t	r_vignette_noise = { "r_vignette_noise", "0.0", CVAR_ARCHIVE };
cvar_t	r_screendarken = { "r_screendarken", "0", CVAR_ARCHIVE };
cvar_t	r_screendarken_depth = { "r_screendarken_depth", "0.4", CVAR_ARCHIVE };
cvar_t	r_teleportfx = { "r_teleportfx", "1", CVAR_ARCHIVE };
cvar_t	r_teleportfx_time = { "r_teleportfx_time", "0.35", CVAR_ARCHIVE };
cvar_t	r_filmgrain = { "r_filmgrain", "0", CVAR_ARCHIVE };
cvar_t	r_filmgrain_amount = { "r_filmgrain_amount", "0.08", CVAR_ARCHIVE };
cvar_t	r_filmgrain_size = { "r_filmgrain_size", "1.0", CVAR_ARCHIVE };
cvar_t	r_filmgrain_speed = { "r_filmgrain_speed", "1.0", CVAR_ARCHIVE };
cvar_t	r_filmgrain_color = { "r_filmgrain_color", "0", CVAR_ARCHIVE };
cvar_t	r_filmgrain_luma_weight = { "r_filmgrain_luma_weight", "0.6", CVAR_ARCHIVE };
cvar_t	r_filmgrain_blend = { "r_filmgrain_blend", "0.6", CVAR_ARCHIVE };
cvar_t	r_filmgrain_seed = { "r_filmgrain_seed", "0", CVAR_ARCHIVE };
cvar_t	r_filmgrain_affect_ui = { "r_filmgrain_affect_ui", "0", CVAR_ARCHIVE };
cvar_t	r_filmgrain_debug = { "r_filmgrain_debug", "0", CVAR_NONE };

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
cvar_t	r_model_halflambert = { "r_model_halflambert", "0", CVAR_ARCHIVE };
cvar_t	r_rim = { "r_rim", "1", CVAR_ARCHIVE };
cvar_t	r_rim_strength = { "r_rim_strength", "0.12", CVAR_ARCHIVE };
cvar_t	r_rim_power = { "r_rim_power", "5.0", CVAR_ARCHIVE };
cvar_t	r_rim_staticScale = { "r_rim_staticScale", "1.0", CVAR_ARCHIVE };
cvar_t	r_rim_dynScale = { "r_rim_dynScale", "1.0", CVAR_ARCHIVE };
cvar_t	r_rim_ambScale = { "r_rim_ambScale", "0.15", CVAR_ARCHIVE };
cvar_t	r_rim_gateK = { "r_rim_gateK", "1.0", CVAR_ARCHIVE };
cvar_t	r_rim_gateBias = { "r_rim_gateBias", "0.0", CVAR_ARCHIVE };
cvar_t	r_rim_colorScale = { "r_rim_colorScale", "1.0", CVAR_ARCHIVE };
cvar_t	r_rim_clampDirect = { "r_rim_clampDirect", "1.0", CVAR_ARCHIVE };
cvar_t	r_rim_clampAmb = { "r_rim_clampAmb", "0.2", CVAR_ARCHIVE };
cvar_t	r_rim_viewmodel = { "r_rim_viewmodel", "0.35", CVAR_ARCHIVE };
cvar_t	r_rim_debug = { "r_rim_debug", "0", CVAR_NONE };
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
cvar_t	r_noshadow_list = { "r_noshadow_list", "progs/flame2.mdl,progs/flame.mdl,progs/bolt1.mdl,progs/bolt2.mdl,progs/bolt3.mdl,progs/laser.mdl", CVAR_NONE };

extern void R_Clustered_BindForShading (void);
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

static float view_znear;
static float view_zfar;

static qboolean R_DoFEnabled (void)
{
	return r_dof.value > 0.f && r_dof_strength.value > 0.f;
}

static qboolean r_dof_autofocus_initialized = false;
static float r_dof_autofocus_value = 0.f;

static float R_GetDynamicDoFFocus (float fallback)
{
	trace_t trace;
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
static GLuint GL_CreateFBOAttachment (GLenum format, int samples, GLenum filter, const char* name)
{
	GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
	GLuint texnum;
	qboolean is_depth_format = (format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8 || format == GL_DEPTH_COMPONENT24 || format == GL_DEPTH_COMPONENT32F);

	glGenTextures (1, &texnum);
	GL_BindNative (GL_TEXTURE0, target, texnum);
	GL_ObjectLabelFunc (GL_TEXTURE, texnum, -1, name);
	if (samples > 1)
	{
		GL_TexStorage2DMultisampleFunc (target, samples, format, vid.width, vid.height, GL_FALSE);
	}
	else
	{
		GL_TexStorage2DFunc (target, 1, format, vid.width, vid.height);
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

/*
=============
GL_CreateFrameBuffers
=============
*/
void GL_CreateFrameBuffers (void)
{
	GLenum color_format = GL_RGBA16F;
	GLenum depth_format = GL_DEPTH24_STENCIL8;

	/* query MSAA limits */
	glGetIntegerv (GL_MAX_COLOR_TEXTURE_SAMPLES, &framebufs.max_color_tex_samples);
	glGetIntegerv (GL_MAX_DEPTH_TEXTURE_SAMPLES, &framebufs.max_depth_tex_samples);
	framebufs.max_samples = q_min (framebufs.max_color_tex_samples, framebufs.max_depth_tex_samples);

	/* main framebuffer (color + depth + stencil) */
	framebufs.composite.color_tex = GL_CreateFBOAttachment (color_format, 1, GL_NEAREST, "composite colors");
	framebufs.composite.depth_stencil_tex = GL_CreateFBOAttachment (depth_format, 1, GL_NEAREST, "composite depth/stencil");
	framebufs.composite.fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D,
		framebufs.composite.color_tex,
		framebufs.composite.depth_stencil_tex,
		framebufs.composite.depth_stencil_tex,
		"composite fbo"
	);
	{
		int froxel_res = CLAMP (0, (int)Q_rint (r_fog_froxel_res.value), 3);
		int downsample = (froxel_res <= 0) ? 4 : (froxel_res == 1 ? 2 : (froxel_res == 2 ? 1 : 1));
		int zslices = CLAMP (8, (int)Q_rint (r_fog_zslices.value), 128);
		framebufs.atmos_froxel.width = q_max (1, (vid.width + downsample - 1) / downsample);
		framebufs.atmos_froxel.height = q_max (1, (vid.height + downsample - 1) / downsample);
		framebufs.atmos_froxel.depth = zslices;
		framebufs.atmos_froxel.history_index = 0;
		r_prev_frame_valid = false;
		glGenTextures (1, &framebufs.atmos_froxel.scatter_tex);
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, framebufs.atmos_froxel.scatter_tex);
		GL_ObjectLabelFunc (GL_TEXTURE, framebufs.atmos_froxel.scatter_tex, -1, "atmos froxel scatter");
		GL_TexStorage3DFunc (GL_TEXTURE_3D, 1, GL_RGBA16F, framebufs.atmos_froxel.width, framebufs.atmos_froxel.height, framebufs.atmos_froxel.depth);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		glGenTextures (1, &framebufs.atmos_froxel.transmittance_tex);
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, framebufs.atmos_froxel.transmittance_tex);
		GL_ObjectLabelFunc (GL_TEXTURE, framebufs.atmos_froxel.transmittance_tex, -1, "atmos froxel transmittance");
		GL_TexStorage3DFunc (GL_TEXTURE_3D, 1, GL_RG16F, framebufs.atmos_froxel.width, framebufs.atmos_froxel.height, framebufs.atmos_froxel.depth);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		for (int i = 0; i < 2; ++i)
		{
			glGenTextures (1, &framebufs.atmos_froxel.scatter_history_tex[i]);
			GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, framebufs.atmos_froxel.scatter_history_tex[i]);
			GL_ObjectLabelFunc (GL_TEXTURE, framebufs.atmos_froxel.scatter_history_tex[i], -1, i == 0 ? "atmos froxel history 0" : "atmos froxel history 1");
			GL_TexStorage3DFunc (GL_TEXTURE_3D, 1, GL_RGBA16F, framebufs.atmos_froxel.width, framebufs.atmos_froxel.height, framebufs.atmos_froxel.depth);
			glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		}

		glGenTextures (1, &framebufs.atmos_froxel.scatter_resolved_tex);
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, framebufs.atmos_froxel.scatter_resolved_tex);
		GL_ObjectLabelFunc (GL_TEXTURE, framebufs.atmos_froxel.scatter_resolved_tex, -1, "atmos froxel resolved");
		GL_TexStorage3DFunc (GL_TEXTURE_3D, 1, GL_RGBA16F, framebufs.atmos_froxel.width, framebufs.atmos_froxel.height, framebufs.atmos_froxel.depth);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	}

	framebufs.autoexposure.width = 16;
	framebufs.autoexposure.height = 16;
	framebufs.autoexposure.tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.autoexposure.width, framebufs.autoexposure.height,
		GL_LINEAR, "autoexposure downscale");
	framebufs.autoexposure.fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.autoexposure.tex, 0, 0, "autoexposure fbo");

	framebufs.bloom.width = q_max (1, vid.width / 2);
	framebufs.bloom.height = q_max (1, vid.height / 2);
	framebufs.bloom.extract_tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.bloom.width, framebufs.bloom.height, GL_LINEAR, "bloom extract");
	framebufs.bloom.pingpong_tex[0] = GL_CreateTexture2D (GL_RGBA16F, framebufs.bloom.width, framebufs.bloom.height, GL_LINEAR, "bloom blur 0");
	framebufs.bloom.pingpong_tex[1] = GL_CreateTexture2D (GL_RGBA16F, framebufs.bloom.width, framebufs.bloom.height, GL_LINEAR, "bloom blur 1");
	framebufs.bloom.extract_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.bloom.extract_tex, 0, 0, "bloom extract fbo");
	framebufs.bloom.pingpong_fbo[0] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.bloom.pingpong_tex[0], 0, 0, "bloom blur fbo 0");
	framebufs.bloom.pingpong_fbo[1] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.bloom.pingpong_tex[1], 0, 0, "bloom blur fbo 1");

	framebufs.godrays.width = q_max (1, vid.width / 2);
	framebufs.godrays.height = q_max (1, vid.height / 2);
	framebufs.godrays.mask_tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.godrays.width, framebufs.godrays.height, GL_LINEAR, "godrays mask");
	framebufs.godrays.shafts_tex = GL_CreateTexture2D (GL_RGBA16F, framebufs.godrays.width, framebufs.godrays.height, GL_LINEAR, "godrays shafts");
	framebufs.godrays.mask_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.godrays.mask_tex, 0, 0, "godrays mask fbo");
	framebufs.godrays.shafts_fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.godrays.shafts_tex, 0, 0, "godrays shafts fbo");

	framebufs.ssao.width[0] = vid.width;
	framebufs.ssao.height[0] = vid.height;
	framebufs.ssao.width[1] = q_max (1, vid.width / 2);
	framebufs.ssao.height[1] = q_max (1, vid.height / 2);
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
		framebufs.ssao.raw_tex[i] = GL_CreateTexture2D (GL_R16F, width, height, GL_NEAREST, va ("ao raw %s", suffix));
		framebufs.ssao.final_tex[i] = GL_CreateTexture2D (GL_R16F, width, height, GL_NEAREST, va ("ao final %s", suffix));
		framebufs.ssao.edges_tex[i] = GL_CreateTexture2D (GL_RG8, width, height, GL_NEAREST, va ("ao edges %s", suffix));
		framebufs.ssao.bentnormal_tex[i] = GL_CreateTexture2D (GL_RG16F, width, height, GL_NEAREST, va ("ao bent %s", suffix));
		framebufs.ssao.ao_fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.ssao.ao_tex[i], 0, 0, va ("ssao fbo %s", suffix));
		framebufs.ssao.blur_fbo[i] = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.ssao.blur_tex[i], 0, 0, va ("ssao blur fbo %s", suffix));
	}
	{
		int maxdim = q_max (vid.width, vid.height);
		int mips = 1;
		while ((1 << mips) <= maxdim)
			++mips;
		framebufs.ssao.depth_mips = mips;
		glGenTextures (1, &framebufs.ssao.depth_prefilter_tex);
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.ssao.depth_prefilter_tex);
		GL_ObjectLabelFunc (GL_TEXTURE, framebufs.ssao.depth_prefilter_tex, -1, "ao depth prefilter");
		GL_TexStorage2DFunc (GL_TEXTURE_2D, mips, GL_R16F, vid.width, vid.height);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	/* scene framebuffer (color + depth + stencil, potentially multisampled) */
	framebufs.scene.samples = Q_nextPow2 ((int)q_max (1.f, vid_fsaa.value));
	framebufs.scene.samples = CLAMP (1, framebufs.scene.samples, framebufs.max_samples);

	framebufs.scene.color_tex = GL_CreateFBOAttachment (color_format, framebufs.scene.samples, GL_NEAREST, "scene colors");
	framebufs.scene.velocity_tex = GL_CreateFBOAttachment (GL_RGBA16F, framebufs.scene.samples, GL_NEAREST, "scene velocity");
	framebufs.scene.depth_stencil_tex = GL_CreateFBOAttachment (depth_format, framebufs.scene.samples, GL_NEAREST, "scene depth/stencil");
	{
		GLuint colors[2] = { framebufs.scene.color_tex, framebufs.scene.velocity_tex };
		framebufs.scene.fbo = GL_CreateFBO (framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D,
			colors, 2,
			framebufs.scene.depth_stencil_tex,
			framebufs.scene.depth_stencil_tex,
			"scene fbo"
		);
	}

	framebufs.dlight.tex = 0;
	framebufs.dlight.fbo = 0;
	if (framebufs.scene.samples == 1)
	{
		framebufs.dlight.tex = GL_CreateFBOAttachment (GL_RGBA16F, 1, GL_LINEAR, "dlight buffer");
		framebufs.dlight.fbo = GL_CreateSimpleFBO (GL_TEXTURE_2D, framebufs.dlight.tex,
			framebufs.scene.depth_stencil_tex,
			framebufs.scene.depth_stencil_tex,
			"dlight fbo"
		);
	}

	/* weighted blended order-independent transparency (accum + revealage, potentially multisampled */
	framebufs.oit.accum_tex = GL_CreateFBOAttachment (GL_RGBA16F, framebufs.scene.samples, GL_NEAREST, "oit accum");
	framebufs.oit.revealage_tex = GL_CreateFBOAttachment (GL_R8, framebufs.scene.samples, GL_NEAREST, "oit revealage");

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
	framebufs.resolved_scene.color_tex = GL_CreateFBOAttachment (color_format, 1, GL_NEAREST, "resolved scene colors");
	framebufs.resolved_scene.velocity_tex = GL_CreateFBOAttachment (GL_RGBA16F, 1, GL_NEAREST, "resolved scene velocity");
		{
			GLuint colors[2] = { framebufs.resolved_scene.color_tex, framebufs.resolved_scene.velocity_tex };
			framebufs.resolved_scene.fbo = GL_CreateFBO (GL_TEXTURE_2D, colors, 2, 0, 0, "resolved scene fbo");
		}
	}
	else
	{
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
	GL_DeleteFramebuffersFunc (1, &framebufs.resolved_scene.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.oit.fbo_composite);
	GL_DeleteFramebuffersFunc (1, &framebufs.oit.fbo_scene);
	GL_DeleteFramebuffersFunc (1, &framebufs.scene.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.dlight.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.composite.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.autoexposure.fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.bloom.extract_fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.bloom.pingpong_fbo[0]);
	GL_DeleteFramebuffersFunc (1, &framebufs.bloom.pingpong_fbo[1]);
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
	GL_DeleteNativeTexture (framebufs.atmos_froxel.scatter_tex);
	GL_DeleteNativeTexture (framebufs.atmos_froxel.transmittance_tex);
	GL_DeleteNativeTexture (framebufs.atmos_froxel.scatter_history_tex[0]);
	GL_DeleteNativeTexture (framebufs.atmos_froxel.scatter_history_tex[1]);
	GL_DeleteNativeTexture (framebufs.atmos_froxel.scatter_resolved_tex);
	if (r_fog_volume_ssbo)
	{
		GL_DeleteBuffersFunc (1, &r_fog_volume_ssbo);
		r_fog_volume_ssbo = 0;
	}
	GL_DeleteNativeTexture (framebufs.oit.revealage_tex);
	GL_DeleteNativeTexture (framebufs.oit.accum_tex);
	GL_DeleteNativeTexture (framebufs.scene.depth_stencil_tex);
	GL_DeleteNativeTexture (framebufs.scene.color_tex);
	GL_DeleteNativeTexture (framebufs.scene.velocity_tex);
	GL_DeleteNativeTexture (framebufs.dlight.tex);
	GL_DeleteNativeTexture (framebufs.autoexposure.tex);
	GL_DeleteNativeTexture (framebufs.bloom.pingpong_tex[0]);
	GL_DeleteNativeTexture (framebufs.bloom.pingpong_tex[1]);
	GL_DeleteNativeTexture (framebufs.bloom.extract_tex);
	GL_DeleteNativeTexture (framebufs.godrays.mask_tex);
	GL_DeleteNativeTexture (framebufs.godrays.shafts_tex);
	GL_DeleteNativeTexture (framebufs.ssao.noise_tex);
	for (int i = 0; i < 2; ++i)
	{
		GL_DeleteNativeTexture (framebufs.ssao.ao_tex[i]);
		GL_DeleteNativeTexture (framebufs.ssao.blur_tex[i]);
		GL_DeleteNativeTexture (framebufs.ssao.raw_tex[i]);
		GL_DeleteNativeTexture (framebufs.ssao.final_tex[i]);
		GL_DeleteNativeTexture (framebufs.ssao.edges_tex[i]);
		GL_DeleteNativeTexture (framebufs.ssao.bentnormal_tex[i]);
	}
	GL_DeleteNativeTexture (framebufs.ssao.depth_prefilter_tex);
	GL_DeleteNativeTexture (framebufs.composite.depth_stencil_tex);
	GL_DeleteNativeTexture (framebufs.composite.color_tex);

	memset (&framebufs, 0, sizeof (framebufs));
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
	qboolean msaa = framebufs.scene.samples > 1;
	GLuint velocity_texture = 0;
	if (framebufs.scene.velocity_tex)
	{
		velocity_texture = msaa ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
		if (framesetup.scene_fbo != framebufs.scene.fbo)
			velocity_texture = 0;
	}
	float mask_enabled = velocity_texture ? 1.f : 0.f;

	GL_BeginGroup ("Bloom extract");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.bloom.extract_fbo);
	glViewport (0, 0, width, height);
	GL_UseProgram (glprogs.bloom_extract);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.color_tex);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, velocity_texture);
	GL_Uniform4fFunc (0, threshold, mask_enabled, 0.f, 0.f);
	GL_Uniform4fFunc (1, (float)vid.width, (float)vid.height,
		(float)vid.width / (float)width,
		(float)vid.height / (float)height);
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
	GL_Uniform4fFunc (1, (float)vid.width, (float)vid.height,
		(float)vid.width / (float)width,
		(float)vid.height / (float)height);
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
		ao_tex, ao_internal_format, ao_width, ao_height, vid.width, vid.height, normal_width, normal_height);
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

static void R_CompositeDlightBuffer (void)
{
	if (!r_dlight_buffered_frame)
		return;
	if (!framebufs.dlight.tex || !glprogs.dlight_composite)
		return;

	float scale = (r_dlight_debug_visualize.value > 0.f) ? 1.f : q_max (0.f, r_dlight_scale.value);
	if (scale <= 0.f)
		return;

	GL_BeginGroup ("Dlight composite");
	GL_UseProgram (glprogs.dlight_composite);
	if (r_dlight_debug_visualize.value > 0.f)
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	else
		GL_SetState (GLS_BLEND_ADD | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.dlight.tex);
	GL_Uniform1fFunc (0, scale);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	r_dlight_buffered_frame = false;
	GL_EndGroup ();
}

static float R_SanitizeSSAOValue (float value, float fallback, float minval, float maxval)
{
#if defined(_MSC_VER)
	if (_finite (value) == 0)
		return fallback;
#else
	if (!isfinite (value))
		return fallback;
#endif

	if (value < minval)
		return minval;
	if (value > maxval)
		return maxval;
	return value;
}

typedef struct ao_quality_preset_s
{
	int assao_samples;
	int gtao_slices;
	int gtao_steps;
	float denoise_sigma;
	int denoise_radius;
	qboolean halfres;
} ao_quality_preset_t;

static ao_quality_preset_t R_GetAOQualityPreset (int quality)
{
	static const ao_quality_preset_t presets[4] = {
		{ 8, 2, 4, 1.0f, 1, true },
		{ 12, 3, 5, 1.5f, 2, true },
		{ 16, 4, 6, 2.0f, 2, false },
		{ 24, 6, 8, 2.5f, 3, false },
	};
	return presets[CLAMP (0, quality, 3)];
}

static void R_DispatchAOComputeCommon (GLuint program, int width, int height)
{
	GL_UseProgram (program);
	GL_DispatchComputeFunc ((GLuint)((width + 7) / 8), (GLuint)((height + 7) / 8), 1);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

static GLuint GL_GenerateLegacySSAOTexture (float view_min_x, float view_min_y, float view_max_x, float view_max_y)
{
	if ((r_ssao.value <= 0.f || r_ssao_intensity.value <= 0.f) && r_ssao_debug.value <= 0.f)
		return 0;
	if (!glprogs.ssao || !framebufs.composite.depth_stencil_tex || !framebufs.ssao.noise_tex)
		return 0;
	if (framebufs.ssao.ao_fbo[0] == 0 || framebufs.ssao.ao_fbo[1] == 0)
		return 0;

	int samples = (int)Q_rint (r_ssao_samples.value);
	samples = CLAMP (4, samples, SSAO_MAX_SAMPLES);
	float radius = R_SanitizeSSAOValue (r_ssao_radius.value, 24.f, 0.01f, 8192.f);
	float bias = R_SanitizeSSAOValue (r_ssao_bias.value, 0.02f, 0.f, 1.f) * radius;
	float power = R_SanitizeSSAOValue (r_ssao_power.value, 1.f, 0.01f, 8.f);
	float min_ao = CLAMP (0.f, r_ssao_min.value, 1.f);
	qboolean use_halfres = (r_ssao_halfres.value > 0.f && r_ssao_force_fullres.value <= 0.f);
	int index = use_halfres ? 1 : 0;
	int width = framebufs.ssao.width[index];
	int height = framebufs.ssao.height[index];
	if (width <= 0 || height <= 0)
		return 0;

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
	float noise_scale = R_SanitizeSSAOValue (r_ssao_noise_scale.value, 1.f, 0.1f, 64.f);
	noise_mode = CLAMP (0, noise_mode, 2);
	if (noise_enabled <= 0.5f)
		noise_mode = 0;
	else if (noise_mode == 0)
		noise_mode = 1;
	int normal_source = (int)Q_rint (r_ssao_normalsource.value);
	normal_source = CLAMP (0, normal_source, 1);
	float max_distance = R_SanitizeSSAOValue (r_ssao_max_distance.value, 1024.f, 1.f, 65536.f);
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
	{
		GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_DPrintf ("SSAO FBO incomplete (0x%X)\n", status);
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
		1.f / (float)vid.width,
		1.f / (float)vid.height,
		(float)vid.width,
		(float)vid.height);
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
	GL_Uniform4fFunc (14, max_distance, 0.f, 0.f, 0.f);
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
			1.f / (float)vid.width,
			1.f / (float)vid.height,
			(float)vid.width,
			(float)vid.height);
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


static GLuint R_RenderAO (float view_min_x, float view_min_y, float view_max_x, float view_max_y)
{
	int mode = CLAMP (0, (int)Q_rint (s_ao_mode.value), 3);
	int method;
	int debug_mode = CLAMP (0, (int)Q_rint (r_ao_debug.value), 6);
	int quality = CLAMP (0, (int)Q_rint (r_ao_quality.value), 3);
	ao_quality_preset_t preset = R_GetAOQualityPreset (quality);

	if (s_ao.value <= 0.f)
		mode = 0;
	if (mode == 1)
		return GL_GenerateLegacySSAOTexture (view_min_x, view_min_y, view_max_x, view_max_y);
	method = CLAMP (0, mode - 1, 2);

	qboolean use_halfres = (r_ao_halfres.value > 0.f) ? true : preset.halfres;
	int index = use_halfres ? 1 : 0;
	int width = framebufs.ssao.width[index];
	int height = framebufs.ssao.height[index];
	float radius = R_SanitizeSSAOValue (r_ao_radius.value, 24.f, 0.01f, 8192.f);
	float intensity = R_SanitizeSSAOValue (r_ao_intensity.value, 1.f, 0.f, 4.f);
	float power = R_SanitizeSSAOValue (r_ao_power.value, 1.5f, 0.01f, 8.f);
	qboolean want_bent = (method == 2 && r_ao_bentnormals.value > 0.f);

	if (method <= 0 || (intensity <= 0.f && debug_mode == 0))
		return 0;
	if (!framebufs.composite.depth_stencil_tex)
		return 0;
	if (method == 1 && !glprogs.ao_assao_main)
		return 0;
	if (method == 2 && (!glprogs.ao_gtao_prefilter || !glprogs.ao_gtao_main || !glprogs.ao_denoise))
		return 0;
	if (!glprogs.ao_denoise)
		return 0;

	/*
	Integration Plan
	- gl_rmain.c: GL_CreateFrameBuffers / GL_DeleteFrameBuffers allocate and release AO compute textures.
	- gl_shaders.c: loads ao_assao_main/ao_gtao_prefilter/ao_gtao_main/ao_denoise/ao_bent_pack compute programs.
	- gl_rmain.c: GL_GenerateSSAOTexture routes r_ao_method to ASSAO-like or GTAO-like compute passes.
	- shaders/postprocess.frag: applies AO in linear color with r_ao_power and debug routing.
	*/
	GL_BeginGroup (method == 2 ? "GTAO" : "ASSAO");
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
	if (method == 2)
	{
		GL_UseProgram (glprogs.ao_gtao_prefilter);
		GL_Uniform4fFunc (0, view_znear, view_zfar, gl_clipcontrol_able ? 1.f : 0.f, 0.f);
		GL_Uniform4fFunc (1, 1.f / (float)vid.width, 1.f / (float)vid.height, (float)vid.width, (float)vid.height);
		GL_BindImageTextureFunc (0, framebufs.ssao.depth_prefilter_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
		R_DispatchAOComputeCommon (glprogs.ao_gtao_prefilter, vid.width, vid.height);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, framebufs.ssao.depth_prefilter_tex);
		GL_GenerateMipmapFunc (GL_TEXTURE_2D);
		GL_MemoryBarrierFunc (GL_TEXTURE_FETCH_BARRIER_BIT);
	}

	GL_UseProgram (method == 2 ? glprogs.ao_gtao_main : glprogs.ao_assao_main);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matinvproj);
	GL_Uniform4fFunc (1, 1.f / (float)vid.width, 1.f / (float)vid.height, (float)vid.width, (float)vid.height);
	GL_Uniform4fFunc (2, 1.f / (float)width, 1.f / (float)height, (float)width, (float)height);
	GL_Uniform4fFunc (3, radius, intensity, power, (float)r_framecount);
	GL_Uniform4fFunc (4, view_min_x, view_min_y, view_max_x, view_max_y);
	GL_Uniform4fFunc (5, view_znear, view_zfar, gl_clipcontrol_able ? 1.f : 0.f, 0.f);
	GL_Uniform4fFunc (6, (float)preset.assao_samples, (float)preset.gtao_slices, (float)preset.gtao_steps, want_bent ? 1.f : 0.f);
	if (method == 2)
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, framebufs.ssao.depth_prefilter_tex);
	else
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
	GL_BindImageTextureFunc (0, framebufs.ssao.raw_tex[index], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
	GL_BindImageTextureFunc (1, framebufs.ssao.edges_tex[index], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG8);
	if (want_bent)
		GL_BindImageTextureFunc (2, framebufs.ssao.bentnormal_tex[index], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
	R_DispatchAOComputeCommon (method == 2 ? glprogs.ao_gtao_main : glprogs.ao_assao_main, width, height);

	GL_UseProgram (glprogs.ao_denoise);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.ssao.raw_tex[index]);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
	GL_BindNative (GL_TEXTURE2, GL_TEXTURE_2D, framebufs.ssao.edges_tex[index]);
	GL_Uniform4fFunc (0, 1.f / (float)width, 1.f / (float)height, preset.denoise_sigma, (float)preset.denoise_radius);
	GL_Uniform4fFunc (1, 1.f / (float)vid.width, 1.f / (float)vid.height, (float)vid.width, (float)vid.height);
	GL_Uniform4fFunc (2, view_znear, view_zfar, gl_clipcontrol_able ? 1.f : 0.f, 0.f);
	GL_Uniform4fFunc (3, view_min_x, view_min_y, view_max_x, view_max_y);
	GL_BindImageTextureFunc (0, framebufs.ssao.final_tex[index], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
	R_DispatchAOComputeCommon (glprogs.ao_denoise, width, height);

	GL_EndGroup ();

	if (debug_mode == 1)
		return framebufs.ssao.raw_tex[index];
	if (debug_mode == 3)
		return framebufs.ssao.edges_tex[index];
	if (debug_mode == 4)
		return framebufs.ssao.depth_prefilter_tex;
	if (debug_mode == 5 && want_bent)
	{
		if (glprogs.ao_bent_pack)
		{
			GL_UseProgram (glprogs.ao_bent_pack);
			GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.ssao.bentnormal_tex[index]);
			GL_BindImageTextureFunc (0, framebufs.ssao.final_tex[index], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
			R_DispatchAOComputeCommon (glprogs.ao_bent_pack, width, height);
		}
		return framebufs.ssao.final_tex[index];
	}
	return framebufs.ssao.final_tex[index];
}

static qboolean r_godrays_rt_logged = false;
static qboolean r_ao_deprecated_warned = false;
static int r_ao_last_logged_mode = -1;

static void R_AOHandleDeprecatedCvars (void)
{
	int s_mode = CLAMP (0, (int)Q_rint (s_ao_mode.value), 3);
	if (s_ao.value <= 0.f)
		s_mode = 0;

	if (r_ssao.value <= 0.f && (s_ao.value > 0.f || s_mode > 0))
	{
		if (!r_ao_deprecated_warned)
		{
			Con_Printf ("r_ssao is deprecated; use s_ao/s_ao_mode.\n");
			r_ao_deprecated_warned = true;
		}
		Cvar_SetValueQuick (&s_ao, 0.f);
		Cvar_SetValueQuick (&s_ao_mode, 0.f);
		s_mode = 0;
	}
	else if (r_ssao.value > 0.f && s_mode == 0)
	{
		if (!r_ao_deprecated_warned)
		{
			Con_Printf ("r_ssao is deprecated; use s_ao/s_ao_mode.\n");
			r_ao_deprecated_warned = true;
		}
		Cvar_SetValueQuick (&s_ao, 1.f);
		Cvar_SetValueQuick (&s_ao_mode, 1.f);
		s_mode = 1;
	}

	/* Unified AO parameters override legacy AO controls. */
	Cvar_SetValueQuick (&r_ao_method, (float)((s_mode >= 2) ? (s_mode - 1) : 0));
	Cvar_SetValueQuick (&r_ao_quality, CLAMP (0.f, s_ao_quality.value, 3.f));
	Cvar_SetValueQuick (&r_ao_radius, R_SanitizeSSAOValue (s_ao_radius.value, 24.f, 0.01f, 8192.f));
	Cvar_SetValueQuick (&r_ao_intensity, CLAMP (0.f, s_ao_strength.value, 4.f));
	Cvar_SetValueQuick (&r_ao_power, R_SanitizeSSAOValue (s_ao_power.value, 1.5f, 0.01f, 8.f));
	Cvar_SetValueQuick (&r_ao_halfres, (s_ao_halfres.value > 0.f) ? 1.f : 0.f);
	Cvar_SetValueQuick (&r_ao_temporal, (s_ao_temporal.value > 0.f) ? 1.f : 0.f);
	Cvar_SetValueQuick (&r_ao_debug, CLAMP (0.f, s_ao_debug.value, 6.f));

	/* Legacy SSAO path mirrors unified controls when selected. */
	if (s_mode == 1)
	{
		Cvar_SetValueQuick (&r_ssao, (s_ao.value > 0.f) ? 1.f : 0.f);
		Cvar_SetValueQuick (&r_ssao_radius, r_ao_radius.value);
		Cvar_SetValueQuick (&r_ssao_intensity, r_ao_intensity.value);
		Cvar_SetValueQuick (&r_ssao_power, r_ao_power.value);
		Cvar_SetValueQuick (&r_ssao_blur, (s_ao_blur.value > 0.f) ? 1.f : 0.f);
		Cvar_SetValueQuick (&r_ssao_halfres, r_ao_halfres.value);
		Cvar_SetValueQuick (&r_ssao_debug, CLAMP (0.f, s_ao_debug.value, 14.f));
	}

	if (r_ao_last_logged_mode != s_mode)
	{
		Con_DPrintf ("AO mode switch: s_ao=%d mode=%d quality=%d radius=%.2f strength=%.2f halfres=%d reversedZ=%d\n",
			(s_ao.value > 0.f) ? 1 : 0, s_mode, (int)Q_rint (r_ao_quality.value),
			r_ao_radius.value, r_ao_intensity.value, (r_ao_halfres.value > 0.f) ? 1 : 0, gl_clipcontrol_able ? 1 : 0);
		r_ao_last_logged_mode = s_mode;
	}
}


void R_ResetGodraysStabilization (void)
{
	r_godrays_stabilization.valid = false;
}


static int R_GodraysQualityScaleDivisor (void)
{
	int q = CLAMP (0, (int)Q_rint (r_godrays_quality.value), 2);
	if (q <= 0)
		return 4;
	if (q == 1)
		return 2;
	return 1;
}

static qboolean R_GodraysReady (void)
{
	if (!glprogs.godrays_mask || !glprogs.godrays)
		return false;
	if (!framebufs.composite.depth_stencil_tex)
		return false;
	if (!framebufs.godrays.mask_fbo || !framebufs.godrays.shafts_fbo)
		return false;
	if (!framebufs.godrays.mask_tex || !framebufs.godrays.shafts_tex)
		return false;
	if (framebufs.godrays.width <= 0 || framebufs.godrays.height <= 0)
		return false;

	return true;
}

static qboolean R_ComputeGodraysSunScreenPos (float *out_x, float *out_y, qboolean *out_visible)
{
	vec3_t sun_dir, sun_point;
	vec4_t clip;
	float inv_w;

	if (!out_x || !out_y || !out_visible)
		return false;
	if (!r_sun.enabled || r_sun.intensity <= 0.f)
		return false;

	VectorCopy (r_framedata.shadow_sun_dir, sun_dir);
	if (DotProduct (sun_dir, sun_dir) <= 1e-6f)
		VectorSet (sun_dir, 0.f, 0.f, -1.f);
	VectorNormalize (sun_dir);

	VectorCopy (r_sun.virtual_origin, sun_point);
	clip[0] = r_matviewproj[0] * sun_point[0] + r_matviewproj[4] * sun_point[1] + r_matviewproj[8] * sun_point[2] + r_matviewproj[12];
	clip[1] = r_matviewproj[1] * sun_point[0] + r_matviewproj[5] * sun_point[1] + r_matviewproj[9] * sun_point[2] + r_matviewproj[13];
	clip[2] = r_matviewproj[2] * sun_point[0] + r_matviewproj[6] * sun_point[1] + r_matviewproj[10] * sun_point[2] + r_matviewproj[14];
	clip[3] = r_matviewproj[3] * sun_point[0] + r_matviewproj[7] * sun_point[1] + r_matviewproj[11] * sun_point[2] + r_matviewproj[15];

	if (!isfinite (clip[3]) || fabsf (clip[3]) < 1e-6f)
		return false;

	inv_w = 1.f / clip[3];
	*out_x = CLAMP (0.f, clip[0] * inv_w * 0.5f + 0.5f, 1.f);
	*out_y = CLAMP (0.f, clip[1] * inv_w * 0.5f + 0.5f, 1.f);
	*out_visible = (clip[3] > 0.f && clip[2] >= -clip[3]);
	return true;
}

static GLuint GL_GenerateGodraysTexture (GLuint *out_mask)
{
	int width = framebufs.godrays.width;
	int height = framebufs.godrays.height;
	float sun_x = 0.5f, sun_y = 0.5f;
	qboolean sun_visible = false;
	int quality_div;
	int samples;
	float density, weight, decay, exposure;

	if (out_mask)
		*out_mask = 0;
	if (!R_GodraysReady () || width <= 0 || height <= 0)
		return 0;
	if (!glprogs.godrays_mask || !glprogs.godrays || !framebufs.composite.depth_stencil_tex)
		return 0;

	quality_div = R_GodraysQualityScaleDivisor ();
	R_ComputeGodraysSunScreenPos (&sun_x, &sun_y, &sun_visible);
	if (!sun_visible && r_godrays_debug.value <= 0.f)
		return 0;

	if (r_godrays_debug.value > 0.f && !r_godrays_rt_logged)
	{
		Con_Printf ("Godrays RT: %dx%d shafts=%dx%d depthTex=%u reversedZ=%d\n",
			vid.width, vid.height, width, height, framebufs.composite.depth_stencil_tex, gl_clipcontrol_able ? 1 : 0);
		r_godrays_rt_logged = true;
	}

	samples = (quality_div == 1) ? 64 : (quality_div == 2 ? 40 : 24);
	density = (quality_div == 1) ? 0.95f : (quality_div == 2 ? 0.9f : 0.8f);
	weight = 0.02f;
	decay = 0.97f;
	exposure = 1.0f;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.mask_fbo);
	glViewport (0, 0, width, height);
	{ const float zero[4] = {0,0,0,0}; GL_ClearBufferfvFunc (GL_COLOR, 0, zero); }
	GL_UseProgram (glprogs.godrays_mask);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.depth_stencil_tex);
	GL_Uniform4fFunc (0, sun_x, sun_y, gl_clipcontrol_able ? 1.f : 0.f, CLAMP (0.f, r_godrays_debug.value, 5.f));
	GL_Uniform4fFunc (1, (float)vid.width, (float)vid.height, (float)vid.width / (float)width, (float)vid.height / (float)height);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.godrays.shafts_fbo);
	glViewport (0, 0, width, height);
	{ const float zero[4] = {0,0,0,0}; GL_ClearBufferfvFunc (GL_COLOR, 0, zero); }
	GL_UseProgram (glprogs.godrays);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.godrays.mask_tex);
	GL_Uniform4fFunc (0, sun_x, sun_y, density, weight);
	GL_Uniform4fFunc (1, decay, exposure, 1.f, (float)samples);
	GL_Uniform4fFunc (2, r_sun.color[0], r_sun.color[1], r_sun.color[2], r_sun.intensity);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	if (out_mask)
		*out_mask = framebufs.godrays.mask_tex;
	return framebufs.godrays.shafts_tex;
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

	pixels = (byte *)malloc (bufsize);
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

	free (pixels);
}

static void GL_SetFilmgrainUniforms (float amount, qboolean allow_debug)
{
	float amount_clamped = CLAMP (0.f, amount, 1.f);
	float size = CLAMP (0.5f, r_filmgrain_size.value, 4.f);
	float speed = q_max (0.f, r_filmgrain_speed.value);
	float luma_weight = CLAMP (0.f, r_filmgrain_luma_weight.value, 1.f);
	float blend = CLAMP (0.f, r_filmgrain_blend.value, 1.f);
	float color = r_filmgrain_color.value > 0.f ? 1.f : 0.f;
	float debug = (allow_debug && r_filmgrain_debug.value > 0.f) ? 1.f : 0.f;
	float seed = r_filmgrain_seed.value;
	float frame = (r_filmgrain_seed.value != 0.f) ? (float)r_framecount : (float)cl.time;

	GL_Uniform4fFunc (13, amount_clamped, size, speed, luma_weight);
	GL_Uniform4fFunc (14, blend, color, debug, seed);
	GL_Uniform4fFunc (15, frame, 0.f, 0.f, 0.f);
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

static qboolean GL_SampleAutoExposureLuminance (float *out_luminance)
{
	const int width = framebufs.autoexposure.width;
	const int height = framebufs.autoexposure.height;
	const int pixel_count = width * height;
	float pixels[16 * 16 * 4];
	float luminance_samples[16 * 16];

	if (framebufs.composite.fbo == 0 || framebufs.autoexposure.fbo == 0)
		return false;
	if (width <= 0 || height <= 0 || pixel_count > (int)countof (luminance_samples))
		return false;

	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.composite.fbo);
	GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.autoexposure.fbo);
	GL_BlitFramebufferFunc (0, 0, vid.width, vid.height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.autoexposure.fbo);
	glReadBuffer (GL_COLOR_ATTACHMENT0);
	glReadPixels (0, 0, width, height, GL_RGBA, GL_FLOAT, pixels);
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
		const float exposure_middle_gray = 0.18f;
		const float bias = q_max (0.f, r_exposure_bias.value);
		const float min_exposure = q_min (r_exposure_min.value, r_exposure_max.value);
		const float max_exposure = q_max (r_exposure_min.value, r_exposure_max.value);
		const float hard_min_exposure = q_max (0.f, q_min (r_ae_min_exposure.value, r_ae_max_exposure.value));
		const float hard_max_exposure = q_max (hard_min_exposure, q_max (r_ae_min_exposure.value, r_ae_max_exposure.value));
		float target = exposure_middle_gray * bias / scene_luminance;
		float speed_up = q_max (0.f, r_exposure_speed_up.value);
		float speed_down = q_max (0.f, r_exposure_speed_down.value);
		float adaptation_speed = (target > current_exposure) ? speed_up : speed_down;
		float delta = (float)(cl.time - last_time);
		float change;
		float max_delta;

		if (delta < 0.f)
			delta = 0.f;

		last_time = cl.time;

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


static qboolean R_ParseFogVector3 (const char *value, vec3_t out)
{
	float x, y, z;
	if (!value || sscanf (value, "%f %f %f", &x, &y, &z) != 3)
		return false;
	out[0] = x; out[1] = y; out[2] = z;
	return true;
}

static void R_FogVolume_Default (fog_volume_t *v)
{
	memset (v, 0, sizeof (*v));
	v->shape = FOG_VOLUME_BOX;
	v->blend = FOG_VOLUME_BLEND_ADD;
	v->density = 1.f;
	v->color[0] = v->color[1] = v->color[2] = 1.f;
	v->noise_scale = 0.06f;
}

void R_Fog_ParseVolumes (void)
{
	const char *data;
	r_fog_volume_count = 0;
	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;
	data = cl.worldmodel->entities;
	data = COM_Parse (data);
	while (data && com_token[0])
	{
		fog_volume_t vol;
		char classname[64] = "";
		if (com_token[0] != '{')
			break;
		R_FogVolume_Default (&vol);
		while (1)
		{
			char key[64], value[1024];
			data = COM_Parse (data);
			if (!data || !com_token[0])
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			if (key[0] == '_')
				memmove (key, key + 1, strlen (key));
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));
			if (!strcmp (key, "classname"))
				q_strlcpy (classname, value, sizeof (classname));
			else if (!strcmp (key, "origin"))
				R_ParseFogVector3 (value, vol.origin);
			else if (!strcmp (key, "mins"))
				R_ParseFogVector3 (value, vol.mins);
			else if (!strcmp (key, "maxs"))
				R_ParseFogVector3 (value, vol.maxs);
			else if (!strcmp (key, "radius"))
				vol.radius = atof (value);
			else if (!strcmp (key, "shape"))
				vol.shape = (!q_strcasecmp (value, "sphere")) ? FOG_VOLUME_SPHERE : FOG_VOLUME_BOX;
			else if (!strcmp (key, "blend"))
				vol.blend = (!q_strcasecmp (value, "override")) ? FOG_VOLUME_BLEND_OVERRIDE : FOG_VOLUME_BLEND_ADD;
			else if (!strcmp (key, "density"))
				vol.density = atof (value);
			else if (!strcmp (key, "color") || !strcmp (key, "albedo"))
				R_ParseFogVector3 (value, vol.color);
			else if (!strcmp (key, "height_falloff"))
				vol.height_falloff = atof (value);
			else if (!strcmp (key, "noise_amount"))
				vol.noise_amount = atof (value);
			else if (!strcmp (key, "noise_scale"))
				vol.noise_scale = atof (value);
			else if (!strcmp (key, "anisotropy") || !strcmp (key, "g"))
				vol.anisotropy = atof (value);
			else if (!strcmp (key, "flags"))
				vol.flags = atoi (value);
		}
		if ((!q_strcasecmp (classname, "fog_volume") || !q_strcasecmp (classname, "env_fog_volume"))
			&& r_fog_volume_count < MAX_FOG_VOLUMES)
		{
			if (vol.shape == FOG_VOLUME_BOX && (VectorCompare (vol.maxs, vec3_origin) || VectorCompare (vol.mins, vec3_origin)))
			{
				VectorAdd (vol.origin, vol.mins, vol.mins);
				VectorAdd (vol.origin, vol.maxs, vol.maxs);
			}
			else if (vol.shape == FOG_VOLUME_BOX)
			{
				VectorSet (vol.mins, vol.origin[0] - 64.f, vol.origin[1] - 64.f, vol.origin[2] - 64.f);
				VectorSet (vol.maxs, vol.origin[0] + 64.f, vol.origin[1] + 64.f, vol.origin[2] + 64.f);
			}
			if (vol.shape == FOG_VOLUME_SPHERE && vol.radius <= 0.f)
				vol.radius = 128.f;
			vol.used = true;
			r_fog_volumes[r_fog_volume_count++] = vol;
		}
		data = COM_Parse (data);
	}
}

static atmosphere_settings_t Atmosphere_ReadSettings (void);
static void R_Atmosphere_LogSettings (const atmosphere_settings_t *settings, const char *stage);

static float Atmosphere_Halton (int index, int base)
{
	float f = 1.f;
	float r = 0.f;
	while (index > 0)
	{
		f /= (float)base;
		r += f * (float)(index % base);
		index /= base;
	}
	return r;
}

static void Atmosphere_Froxel_InitResources (void) { (void)0; }


static void Atmosphere_Froxel_UploadVolumeBuffer (int *out_count)
{
	int i, active;
	if (out_count)
		*out_count = 0;
	if (!r_fog_volume_ssbo)
		GL_GenBuffersFunc (1, &r_fog_volume_ssbo);
	active = CLAMP (0, (int)Q_rint (r_fog_volume_max_active.value), MAX_FOG_VOLUMES);
	active = q_min (active, r_fog_volume_count);
	for (i = 0; i < active; ++i)
	{
		fog_volume_t *v = &r_fog_volumes[i];
		packed_fog_volume_t *dst = &r_fog_volume_gpu[i];
		vec3_t center;
		vec3_t ext;
		if (v->shape == FOG_VOLUME_BOX)
		{
			center[0] = (v->mins[0] + v->maxs[0]) * 0.5f;
			center[1] = (v->mins[1] + v->maxs[1]) * 0.5f;
			center[2] = (v->mins[2] + v->maxs[2]) * 0.5f;
			ext[0] = q_max (1.f, (v->maxs[0] - v->mins[0]) * 0.5f);
			ext[1] = q_max (1.f, (v->maxs[1] - v->mins[1]) * 0.5f);
			ext[2] = q_max (1.f, (v->maxs[2] - v->mins[2]) * 0.5f);
		}
		else
		{
			VectorCopy (v->origin, center);
			VectorSet (ext, v->radius, v->radius, v->radius);
		}
		dst->data0[0] = center[0]; dst->data0[1] = center[1]; dst->data0[2] = center[2]; dst->data0[3] = q_max (v->radius, 1.f);
		dst->data1[0] = ext[0]; dst->data1[1] = ext[1]; dst->data1[2] = ext[2]; dst->data1[3] = q_max (v->density, 0.f);
		dst->data2[0] = CLAMP (0.f, v->color[0], 8.f); dst->data2[1] = CLAMP (0.f, v->color[1], 8.f); dst->data2[2] = CLAMP (0.f, v->color[2], 8.f); dst->data2[3] = q_max (v->height_falloff, 0.f);
		dst->data3[0] = CLAMP (0.f, v->noise_amount, 2.f); dst->data3[1] = q_max (0.0001f, v->noise_scale); dst->data3[2] = CLAMP (-0.95f, v->anisotropy, 0.95f); dst->data3[3] = (float)v->shape;
		dst->data4[0] = (float)v->blend; dst->data4[1] = (float)v->flags;
	}
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fog_volume_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (packed_fog_volume_t) * q_max (1, active), active > 0 ? r_fog_volume_gpu : NULL, GL_STREAM_DRAW);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 7, r_fog_volume_ssbo, 0, sizeof (packed_fog_volume_t) * q_max (1, active));
	if (out_count)
		*out_count = active;
}

static void Atmosphere_Froxel_BuildVolume (const atmosphere_settings_t *settings, vec2_t jitter)
{
	int gx, gy, gz;
	int active_volumes = 0;
	if (!settings || !glprogs.atmos_froxel_build)
		return;
	gx = (framebufs.atmos_froxel.width + 3) / 4;
	gy = (framebufs.atmos_froxel.height + 3) / 4;
	gz = (framebufs.atmos_froxel.depth + 3) / 4;
	GL_BeginGroup ("Atmosphere: Froxel Build");
	GL_UseProgram (glprogs.atmos_froxel_build);
	GL_BindImageTextureFunc (0, framebufs.atmos_froxel.scatter_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	GL_BindImageTextureFunc (1, framebufs.atmos_froxel.transmittance_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG16F);
	Atmosphere_Froxel_UploadVolumeBuffer (&active_volumes);
	GL_Uniform4fFunc (0, (float)framebufs.atmos_froxel.width, (float)framebufs.atmos_froxel.height, (float)framebufs.atmos_froxel.depth, 0.f);
	GL_Uniform4fFunc (1, settings->density, q_max (0.00001f, r_fog_height_falloff.value), CLAMP (0.f, r_fog_albedo.value, 1.f), settings->anisotropy);
	GL_Uniform4fFunc (2, Fog_GetColor()[0], Fog_GetColor()[1], Fog_GetColor()[2], settings->noise_scale);
	GL_Uniform4fFunc (3, jitter[0], jitter[1], settings->noise_drift, CLAMP (0.f, r_fog_noise_strength.value, 2.f));
	GL_Uniform4fFunc (4, r_matproj[0], r_matproj[5], q_max (view_znear, 0.01f), q_max (view_zfar, 1.f));
	GL_Uniform4fFunc (5, (float)active_volumes, r_fog_volume_max_active.value, 0.f, 0.f);
	GL_Uniform4fFunc (6, settings->height_base, settings->debug_slice, settings->debug_mode, 0.f);
	GL_DispatchComputeFunc (gx, gy, gz);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
	GL_EndGroup ();
}

static void Atmosphere_Froxel_LightIntegrate (const atmosphere_settings_t *settings)
{
	int gx, gy, gz;
	int debug_mode = 0;
	if (!settings || !glprogs.atmos_froxel_integrate)
		return;
	gx = (framebufs.atmos_froxel.width + 3) / 4;
	gy = (framebufs.atmos_froxel.height + 3) / 4;
	gz = (framebufs.atmos_froxel.depth + 3) / 4;
	GL_BeginGroup ("Atmosphere: Froxel LightIntegrate");
	GL_UseProgram (glprogs.atmos_froxel_integrate);
	R_Clustered_BindForShading ();
	R_Shadow_BindShadowMap (GL_TEXTURE6);
	GL_BindImageTextureFunc (0, framebufs.atmos_froxel.scatter_tex, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
	GL_BindImageTextureFunc (1, framebufs.atmos_froxel.transmittance_tex, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RG16F);
	debug_mode = (r_fog_debug.value > 0.f) ? CLAMP (0, (int)Q_rint (r_fog_debug_mode.value), 7) : 0;
	if (debug_mode <= 0)
	{
		if (r_fog_debug_density.value > 0.f)
			debug_mode = 1;
		else if (r_fog_debug_transmittance.value > 0.f)
			debug_mode = 2;
		else if (r_fog_debug_scattering.value > 0.f)
			debug_mode = 3;
		else if (r_fog_debug_light_injection.value > 0.f)
			debug_mode = 4;
		else if (r_fog_debug_step_length.value > 0.f)
			debug_mode = 5;
		else if (r_fog_debug_showgrid.value > 0.f)
			debug_mode = 6;
	}
	GL_Uniform4fFunc (0, (float)framebufs.atmos_froxel.width, (float)framebufs.atmos_froxel.height, (float)framebufs.atmos_froxel.depth, q_max (1.f, settings->steps));
	GL_Uniform4fFunc (1, settings->anisotropy, q_max (0.f, r_fog_sun_strength.value), settings->shadow_enable ? 1.f : 0.f, 0.f);
	GL_Uniform4fFunc (2, (float)debug_mode, settings->density, q_max (view_zfar, 1.f), 0.f);
	GL_Uniform4fFunc (3, r_matproj[0], r_matproj[5], q_max (view_znear, 0.01f), q_max (view_zfar, 1.f));
	GL_Uniform4fFunc (4, r_sun.color[0], r_sun.color[1], r_sun.color[2], 0.f);
	if (r_fog_debug_sun.value > 0.f)
	{
		Con_Printf ("FogSun: dir=(%.3f %.3f %.3f) color=(%.3f %.3f %.3f) intensity=%.3f anisotropy=%.3f sunStrength=%.3f\n",
			r_sun.direction[0], r_sun.direction[1], r_sun.direction[2],
			r_sun.color[0], r_sun.color[1], r_sun.color[2], r_sun.intensity,
			settings->anisotropy, q_max (0.f, r_fog_sun_strength.value));
	}
	GL_DispatchComputeFunc (gx, gy, gz);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
	GL_EndGroup ();
}

static void Atmosphere_Froxel_TemporalResolve (const atmosphere_settings_t *settings)
{
	int gx, gy, gz;
	int src = framebufs.atmos_froxel.history_index;
	int dst = 1 - src;
	if (!settings || !glprogs.atmos_froxel_temporal)
		return;
	if (r_fog_temporal.value <= 0.f)
		return;
	gx = (framebufs.atmos_froxel.width + 3) / 4;
	gy = (framebufs.atmos_froxel.height + 3) / 4;
	gz = (framebufs.atmos_froxel.depth + 3) / 4;
	GL_BeginGroup ("Atmosphere: Froxel Temporal");
	GL_UseProgram (glprogs.atmos_froxel_temporal);
	GL_BindNative (GL_TEXTURE10, GL_TEXTURE_3D, framebufs.atmos_froxel.scatter_history_tex[src]);
	GL_BindImageTextureFunc (0, framebufs.atmos_froxel.scatter_tex, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
	GL_BindImageTextureFunc (1, framebufs.atmos_froxel.scatter_resolved_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	GL_BindImageTextureFunc (2, framebufs.atmos_froxel.scatter_history_tex[dst], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	GL_Uniform4fFunc (0, (float)framebufs.atmos_froxel.width, (float)framebufs.atmos_froxel.height, (float)framebufs.atmos_froxel.depth, 0.f);
	GL_Uniform4fFunc (1, CLAMP (0.f, settings->history_weight, 0.99f), r_prev_frame_valid ? 1.f : 0.f, settings->history_weight, 0.f);
	GL_DispatchComputeFunc (gx, gy, gz);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
	framebufs.atmos_froxel.history_index = dst;
	GL_EndGroup ();
}

static atmosphere_settings_t Atmosphere_ReadSettings (void)
{
	atmosphere_settings_t settings;
	float fog_density = fabsf (Fog_GetDensity ()) * q_max (0.f, r_fog_density.value);
	qboolean shafts_requested;
	int quality;

	memset (&settings, 0, sizeof (settings));
	quality = CLAMP (0, (int)Q_rint (r_fog_quality.value), 3);
	shafts_requested = (r_godrays.value > 0.f && R_GodraysReady ());

	settings.mode = 1;
	settings.enabled_fog = (r_fog_enable.value > 0.f && fog_density > 0.f);
	settings.enabled_shafts = shafts_requested;
	settings.enabled_volumetrics = settings.enabled_fog;
	settings.shadow_enable = (r_shadows.value > 0.f && r_shadow_sun.value > 0.f);
	settings.density = fog_density;
	{
		float legacy_g = r_fog_anisotropy.value;
		float g = r_fog_g.value;
		if (fabsf (g - 0.76f) < 0.0001f && fabsf (legacy_g - 0.2f) > 0.0001f)
			g = legacy_g;
		settings.anisotropy = CLAMP (-0.9f, g, 0.9f);
	}
	settings.steps = (float)(16 + quality * 16);
	settings.history_weight = CLAMP (0.f, r_fog_history_weight.value, 1.f);
	settings.debug_mode = (r_fog_debug.value > 0.f) ? CLAMP (0.f, r_fog_debug_mode.value, 7.f) : 0.f;
	settings.debug_slice = CLAMP (0.f, r_fog_debug_slice.value, (float)q_max (framebufs.atmos_froxel.depth - 1, 0));
	settings.noise_scale = q_max (0.0001f, r_fog_noise_scale.value);
	settings.noise_drift = q_max (0.f, r_fog_noise_drift.value);
	settings.height_base = r_fog_height_base.value;
	settings.enabled_shafts = false;
	return settings;
}


static void R_Atmosphere_LogSettings (const atmosphere_settings_t *settings, const char *stage)
{
	if (r_fog_log.value <= 0.f || !settings)
		return;
	Con_Printf ("atmos[%s] mode=%d fog=%d shafts=%d vol=%d froxel=%d shadow=%d density=%.5f anisotropy=%.5f steps=%.1f history=%.3f debug=%.0f\n",
		stage ? stage : "?",
		settings->mode,
		settings->enabled_fog ? 1 : 0,
		settings->enabled_shafts ? 1 : 0,
		settings->enabled_volumetrics ? 1 : 0,
		r_atmosphere.froxel_enabled ? 1 : 0,
		settings->shadow_enable ? 1 : 0,
		settings->density,
		settings->anisotropy,
		settings->steps,
		settings->history_weight,
		settings->debug_mode);
	if (r_atmosphere.froxel_enabled)
		Con_Printf ("atmos[froxel] dims=%dx%dx%d formats=scatter:RGBA16F trans:RG16F\n", framebufs.atmos_froxel.width, framebufs.atmos_froxel.height, framebufs.atmos_froxel.depth);
}
static void R_Fog_BeginFrame (void)
{
	r_atmosphere.settings = Atmosphere_ReadSettings ();
	r_atmosphere.settings_valid = true;
	r_atmosphere.godrays_texture = 0;
	r_atmosphere.godrays_mask = 0;
	r_atmosphere.godrays_ready = false;
	r_atmosphere.fog_enabled_for_scene = false;
	r_atmosphere.froxel_enabled = r_atmosphere.settings.enabled_volumetrics;

	if (r_atmosphere.settings.enabled_fog && !r_atmosphere.settings.enabled_volumetrics)
	{
		Fog_EnableGFog ();
		r_atmosphere.fog_enabled_for_scene = true;
	}
	else
	{
		Fog_DisableGFog ();
	}
	R_Atmosphere_LogSettings (&r_atmosphere.settings, "begin");
}

static void R_Fog_Render (void)
{
	/*
	 * Volumetric fog is compute-driven, but still explicitly pins baseline GL
	 * state so this pass never depends on whatever transparent/world pass ran
	 * before it.  STATEDBG markers around POST_FOG_* then reflect deterministic
	 * state instead of leaks from previous draws.
	 */
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	glDisable (GL_SCISSOR_TEST);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	if (!r_atmosphere.settings_valid)
		r_atmosphere.settings = Atmosphere_ReadSettings ();

	if (r_atmosphere.settings.enabled_volumetrics)
	{
		vec2_t jitter;
		Atmosphere_Froxel_InitResources ();
		if (r_fog_jitter.value > 0.f)
		{
			jitter[0] = Atmosphere_Halton (r_framecount & 1023, 2) - 0.5f;
			jitter[1] = Atmosphere_Halton (r_framecount & 1023, 3) - 0.5f;
		}
		else
		{
			jitter[0] = 0.f;
			jitter[1] = 0.f;
		}
		Atmosphere_Froxel_BuildVolume (&r_atmosphere.settings, jitter);
		if (r_fog_debug_volume_bounds.value > 0.f)
		{
			int i;
			vec3_t color = {0.2f, 0.8f, 1.0f};
			for (i = 0; i < r_fog_volume_count; ++i)
			{
				fog_volume_t *v = &r_fog_volumes[i];
				vec3_t mins, maxs;
				if (v->shape == FOG_VOLUME_SPHERE)
				{
					VectorSet (mins, v->origin[0] - v->radius, v->origin[1] - v->radius, v->origin[2] - v->radius);
					VectorSet (maxs, v->origin[0] + v->radius, v->origin[1] + v->radius, v->origin[2] + v->radius);
				}
				else
				{
					VectorCopy (v->mins, mins);
					VectorCopy (v->maxs, maxs);
				}
				R_DebugDrawWireBox (mins, maxs, color, true);
			}
		}
		Atmosphere_Froxel_LightIntegrate (&r_atmosphere.settings);
		Atmosphere_Froxel_TemporalResolve (&r_atmosphere.settings);
		r_atmosphere.settings.enabled_shafts = false;
	}

	/* Keep post-fog stage hand-off explicit as well. */
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
}

static void R_Fog_Composite (void)
{
	/* Composite setup must be explicit: no inherited alpha blending. */
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	glDisable (GL_SCISSOR_TEST);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	static int last_composite_log_frame = -1000000;
	if (r_fog_debug_composite_stage.value > 0.f && r_framecount - last_composite_log_frame >= 60)
	{
		Con_Printf ("fog composite stage: before post tonemap/bloom (linear HDR) frame=%d\n", r_framecount);
		last_composite_log_frame = r_framecount;
	}
	if (r_godrays.value > 0.f || r_godrays_debug.value > 0.f)
	{
		r_atmosphere.godrays_texture = GL_GenerateGodraysTexture (&r_atmosphere.godrays_mask);
		r_atmosphere.godrays_ready = (r_atmosphere.godrays_texture != 0 || r_godrays_debug.value > 0.f);
	}

	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
}

static void R_Fog_EndFrame (void)
{
	if (r_atmosphere.fog_enabled_for_scene)
		Fog_DisableGFog ();
	R_Atmosphere_LogSettings (&r_atmosphere.settings, "final");
}

static void R_Atmosphere_Render (qboolean after_scene)
{
	if (!after_scene)
	{
		R_StateDebugMark ("POST_FOG_BEGIN_BEFORE");
		R_Fog_BeginFrame ();
		R_StateDebugMark ("POST_FOG_BEGIN_AFTER");
		return;
	}
	R_StateDebugMark ("POST_FOG_RENDER_BEFORE");
	R_Fog_Render ();
	R_StateDebugMark ("POST_FOG_RENDER_AFTER");
	R_StateDebugMark ("POST_FOG_COMPOSITE_BEFORE");
	R_Fog_Composite ();
	R_StateDebugMark ("POST_FOG_COMPOSITE_AFTER");
	R_Fog_EndFrame ();
	R_StateDebugMark ("POST_FOG_END_AFTER");
}

void GL_PostProcess (void)
{
	int palidx, variant;
	float dither;
	qboolean dof_enabled;
	float dof_focus, dof_range, dof_strength;
	float dof_znear, dof_zfar;
	qboolean motion_enabled;
	qboolean msaa;
	GLuint velocity_texture;
	GLuint depth_texture;
	GLuint godrays_texture;
	GLuint godrays_mask;
	GLuint ssao_texture;
	float motion_strength;
	float motion_shutter;
	float motion_effective_shutter;
	float motion_max_radius;
	float motion_min_velocity;
	float motion_depth_threshold;
	int motion_max_samples;
	qboolean screen_darken_enabled;
	float screen_darken_strength;
	float screen_darken_depth;
	float teleport_fade;
	float teleport_blur;
	qboolean godrays_enabled;
	float godrays_debug;
	float ssao_intensity;
	float ssao_debug_mode;
	float ssao_fog_strength;
	float ssao_fog_power;
	float view_min_x;
	float view_min_y;
	float view_max_x;
	float view_max_y;
	float inv_scale;
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
	r_color_saturation.value = CLAMP (0.9f, r_color_saturation.value, 1.2f);
	if (!GL_NeedsPostprocess ())
		return;
	if (framebufs.composite.fbo == 0 || framebufs.composite.color_tex == 0)
		return;
	if (!framesetup.composite_ready)
	{
		GL_BeginGroup ("Postprocess backbuffer copy");
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, 0);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
		glReadBuffer (GL_BACK);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		GL_BlitFramebufferFunc (0, 0, vid.width, vid.height, 0, 0, vid.width, vid.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
		glDrawBuffer (GL_BACK);
		glReadBuffer (GL_BACK);
		framesetup.composite_ready = true;
		GL_EndGroup ();
	}

	GL_BeginGroup ("Postprocess");

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
	float bloom_intensity_effective = bloom_intensity;
	float exposure = q_max (0.f, r_tonemap_exposure.value);
	float tonemap_mode = q_max (0.f, r_tonemap.value);
	screen_darken_strength = q_min (1.f, q_max (0.f, r_screendarken.value));
	screen_darken_depth = q_max (0.f, r_screendarken_depth.value);
	screen_darken_enabled = (screen_darken_strength > 0.f);
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
	if (r_postfx_bloom_mode.value > 0.f)
		bloom_intensity_effective = q_max (bloom_intensity, postfx_bloom_boost);
	else
		bloom_intensity_effective = bloom_intensity + postfx_bloom_boost;
	bloom_intensity_effective = q_max (0.f, bloom_intensity_effective);

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

	if (!r_atmosphere.settings_valid)
		r_atmosphere.settings = Atmosphere_ReadSettings ();
	godrays_enabled = (r_godrays.value > 0.f && r_atmosphere.godrays_texture != 0);
	godrays_debug = CLAMP (0.f, r_godrays_debug.value, 5.f);
	if (!r_atmosphere.godrays_ready)
		godrays_debug = 0.f;
	godrays_texture = r_atmosphere.godrays_texture;
	godrays_mask = r_atmosphere.godrays_mask;

	view_min_x = (glx + r_refdef.vrect.x) / (float)vid.width;
	view_min_y = (gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height) / (float)vid.height;
	view_max_x = view_min_x + r_refdef.vrect.width / (float)vid.width;
	view_max_y = view_min_y + r_refdef.vrect.height / (float)vid.height;
	inv_scale = r_refdef.scale > 0 ? 1.f / (float)r_refdef.scale : 1.f;

	R_AOHandleDeprecatedCvars ();
	ssao_texture = R_RenderAO (view_min_x, view_min_y, view_max_x, view_max_y);
	ssao_intensity = R_SanitizeSSAOValue (r_ao_intensity.value, 0.f, 0.f, 1.f);
	{
		int debug_cvar = (int)Q_rint (r_ao_debug.value);
		int debug_mode_i = (debug_cvar > 0) ? CLAMP (1, debug_cvar, 11) : -1;
		ssao_debug_mode = (debug_mode_i > 0) ? (float)debug_mode_i : -1.f;
	}
	if (ssao_texture == 0)
		ssao_debug_mode = -1.f;
	if (ssao_texture == 0 || s_ao.value <= 0.f || s_ao_mode.value <= 0.f)
		ssao_intensity = 0.f;
	ssao_fog_strength = CLAMP (0.f, r_ssao_fog_strength.value, 1.f);
	ssao_fog_power = q_max (0.01f, r_ssao_fog_power.value);

	msaa = framebufs.scene.samples > 1;
	motion_strength = q_max (0.f, r_motionblur.value);
	if (!GL_ShouldApplyMotionBlur ())
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
	if (framebufs.scene.velocity_tex)
	{
		velocity_texture = msaa ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
		if (framesetup.scene_fbo != framebufs.scene.fbo)
		{
			// The scene FBO was not used this frame, so the velocity attachment still holds stale data.
			velocity_texture = 0;
		}
	}
	motion_enabled = (motion_effective_shutter > 0.f && motion_max_samples > 0 && velocity_texture != 0);

	R_StateDebugMark ("POST_BEFORE_FINAL_COMPOSITE");
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
	R_ResetViewportAndScissorFullscreen ("POST_FINAL_COMPOSITE_RESET");
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
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.color_tex);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_3D, gl_palette_lut);
	GL_BindNative (GL_TEXTURE3, GL_TEXTURE_2D, bloom_texture);
	GL_BindNative (GL_TEXTURE4, GL_TEXTURE_2D, velocity_texture);
	GL_BindNative (GL_TEXTURE5, GL_TEXTURE_2D, godrays_texture);
	GL_BindNative (GL_TEXTURE6, GL_TEXTURE_2D, godrays_mask);
	GL_BindNative (GL_TEXTURE7, GL_TEXTURE_2D, 0);
	GL_BindNative (GL_TEXTURE8, GL_TEXTURE_2D, ssao_texture);
	GL_BindNative (GL_TEXTURE9, GL_TEXTURE_2D_ARRAY, R_PostFX_GetLUTTexture ());
	GL_BindNative (GL_TEXTURE10, GL_TEXTURE_3D,
		(r_fog_temporal.value > 0.f) ? framebufs.atmos_froxel.scatter_resolved_tex : framebufs.atmos_froxel.scatter_tex);
	GL_BindNative (GL_TEXTURE11, GL_TEXTURE_3D, framebufs.atmos_froxel.transmittance_tex);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, gl_palette_buffer[palidx], 0, 256 * sizeof (GLuint));
	if (variant != 2) // some AMD drivers optimize out the uniform in variant #2
	{
		float post_contrast = CLAMP (0.8f, r_color_contrast.value, 1.2f);
		GL_Uniform4fFunc (0, 1.f, post_contrast, 1.f / r_refdef.scale, dither);
	}
	{
		int debug_mode = (int)Q_rint (CLAMP (0.f, r_debug_colorspace.value, 4.f));
		qboolean linear_debug = (debug_mode == 2);
		qboolean output_srgb = (r_srgb_framebuffer.value <= 0.f) || !vid_framebuffer_srgb_capable;
		if (linear_debug)
			output_srgb = false;
		GL_Uniform4fFunc (19, (float)debug_mode, 0.f, output_srgb ? 1.f : 0.f, 0.f);
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
	screen_darken_strength,
	screen_darken_depth,
	0.f);
	GL_Uniform4fFunc (11, teleport_fade, teleport_blur, 0.f, 0.f);
	GL_Uniform1fFunc (12, CLAMP (0.9f, r_color_saturation.value, 1.2f));
	GL_Uniform1fFunc (20, q_max (0.1f, r_color_midtone.value));
	{
		float black_lift = CLAMP (0.f, r_tonemap_black_lift.value, 0.05f);
		float black_lift_strength = q_max (0.f, r_tonemap_black_lift_strength.value);
		GL_Uniform4fFunc (27, black_lift, black_lift_strength, 0.f, 0.f);
	}
	GL_Uniform4fFunc (28,
		r_atmosphere.froxel_enabled ? 1.f : 0.f,
		(float)framebufs.atmos_froxel.width,
		(float)framebufs.atmos_froxel.height,
		(float)framebufs.atmos_froxel.depth);
	GL_Uniform4fFunc (29,
		(float)q_max (1, (int)Q_rint (r_fog_froxel_res.value)),
		(float)framebufs.atmos_froxel.depth,
		q_max (view_zfar, 1.f),
		0.f);
	{
		float dbg_mode = (r_fog_debug.value > 0.f) ? CLAMP (0.f, r_fog_debug_mode.value, 7.f) : 0.f;
		if (dbg_mode <= 0.f)
		{
			if (r_fog_debug_density.value > 0.f) dbg_mode = 1.f;
			else if (r_fog_debug_transmittance.value > 0.f) dbg_mode = 2.f;
			else if (r_fog_debug_scattering.value > 0.f) dbg_mode = 3.f;
			else if (r_fog_debug_showgrid.value > 0.f) dbg_mode = 6.f;
		}
		GL_Uniform4fFunc (30,
			dbg_mode,
			r_atmosphere.settings.debug_slice / q_max (1.f, (float)framebufs.atmos_froxel.depth),
			(float)r_fog_volume_count,
			0.f);
	}
	GL_Uniform4fFunc (21, postfx_exposure_add, postfx_bloom_boost, postfx_emissive_boost, postfx_desat);
	GL_Uniform4fFunc (22, postfx_lut_strength, postfx_state.underwater_grade_strength, postfx_state.underwater_fog_strength, postfx_vignette_softness);
	GL_Uniform4fFunc (23, (float)postfx_lut_size, (float)postfx_lut_id, 0.f, 0.f);
	GL_Uniform4fFunc (24, fog_r, fog_g, fog_b, 0.f);
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
	{ float sx = 0.5f, sy = 0.5f; qboolean vis = false; R_ComputeGodraysSunScreenPos (&sx, &sy, &vis); GL_Uniform4fFunc (16, godrays_texture ? 1.f : 0.f, godrays_debug, sx, sy); }
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
		GL_Uniform4fFunc (19, R_SanitizeSSAOValue (r_ao_power.value, 1.5f, 0.01f, 8.f), CLAMP (0.f, r_ao_applymode.value, 1.f), 0.f, 0.f);
	}
	{
		float filmgrain_amount = 0.f;
		qboolean filmgrain_enabled = (r_filmgrain.value > 0.f && r_filmgrain_affect_ui.value <= 0.f);
		if (filmgrain_enabled)
			filmgrain_amount = r_filmgrain_amount.value;
		GL_SetFilmgrainUniforms (filmgrain_amount, filmgrain_enabled);
	}

	dof_enabled = R_DoFEnabled ();

	{
		GL_Uniform4fFunc (3, view_min_x, view_min_y, view_max_x, view_max_y);
		GL_Uniform4fFunc (4, inv_scale, inv_scale, 0.f, 0.f);
	}

	depth_texture = 0;
	{
		qboolean ssao_needs_depth = (ssao_texture != 0
			&& (s_ao_halfres.value > 0.f || ssao_debug_mode == 8.f || ssao_fog_strength > 0.f));
		if (framebufs.composite.depth_stencil_tex && (dof_enabled || screen_darken_enabled || (motion_enabled && motion_depth_threshold > 0.f) || ssao_needs_depth))
			depth_texture = framebufs.composite.depth_stencil_tex;
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
	R_StateDebugMark ("POST_AFTER_FINAL_COMPOSITE");

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
	/*
	Reverse-Z / Normal-Z invariants (source of truth):

	1) Context setup (GL_SetupState):
	   - Reverse-Z path (gl_clipcontrol_able):
	       ClipControl = GL_ZERO_TO_ONE, ClearDepth = 0, DepthFunc = GL_GEQUAL.
	   - Normal path:
	       default clip space (-1..1), ClearDepth = 1, DepthFunc = GL_LEQUAL.

	2) Depth range wrappers must preserve draw ordering semantics:
	   - ZRANGE_FULL:      [0, 1] in both modes.
	   - ZRANGE_VIEWMODEL: [0.7, 1] in reverse-Z, [0, 0.3] in normal-Z.
	   - ZRANGE_NEAR:      [1, 1] in reverse-Z, [0, 0] in normal-Z.

	3) All mode-dependent depth compares map through R_MapDepthFunc(),
	   so material depth funcs keep the same logical meaning across modes.

	These invariants are consumed by projection/depth reconstruction shaders and
	the sky-depth cutoffs sent from GL_GenerateLegacySSAOTexture().
	*/
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
	float w, h, d;
	float znear, zfar;
	float logznear, logzfar;
	float translation[16];
	float rotation[16];
	int i;

	// reduce near clip distance at high FOV's to avoid seeing through walls
	w = 1.f / tanf (DEG2RAD (r_fovx) * 0.5f);
	h = 1.f / tanf (DEG2RAD (r_fovy) * 0.5f);
	d = 12.f * q_min (w, h);
	znear = CLAMP (0.5f, d, 4.f);
	zfar = gl_farclip.value;

	view_znear = znear;
	view_zfar = zfar;

	GL_FrustumMatrix (r_matproj, DEG2RAD (r_fovx), DEG2RAD (r_fovy), znear, zfar);
	if (!MatrixInverse4x4 (r_matproj, r_matinvproj))
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
        memcpy (r_framedata.viewproj, r_matviewproj, 16 * sizeof (float));
        memcpy (r_framedata.view, r_matview, 16 * sizeof (float));
        r_framedata.zparams[0] = LIGHT_TILES_Z / (logzfar - logznear);
        r_framedata.zparams[1] = -r_framedata.zparams[0] * logznear;
}

/*
=============
GL_NeedsSceneEffects
=============
*/

qboolean GL_NeedsSceneEffects (void)
{
        if (framebufs.scene.samples > 1 || water_warp || r_refdef.scale != 1)
		return true;

	if (R_DlightsAdditivePassEnabled () && framebufs.dlight.fbo && r_framedata.numlights > 0)
		return true;

        if (GL_ShouldApplyMotionBlur ())
		return true;

        if (R_DoFEnabled ())
		return true;

	if (r_fog_enable.value > 0.f && fabsf (Fog_GetDensity ()) * q_max (0.f, r_fog_density.value) > 0.f)
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
        float saturation = CLAMP (0.9f, r_color_saturation.value, 1.2f);
	r_color_saturation.value = saturation;
	if (softemu || R_GetEffectiveAlphaMode () == ALPHAMODE_OIT || R_DoFEnabled ())
		return true;
	if (r_debug_colorspace.value > 0.f)
		return true;
	if (r_tonemap.value > 0.f || r_bloom.value > 0.f || r_color_contrast.value != 1.f || saturation != 1.f || r_color_midtone.value != 1.f || GL_ShouldApplyMotionBlur ())
		return true;
	if (r_srgb_framebuffer.value <= 0.f)
		return true;
	if (r_filmgrain.value > 0.f && r_filmgrain_affect_ui.value <= 0.f)
		return true;
	if (r_ao_method.value > 0.f)
		return true;
	if (r_godrays.value > 0.f)
		return true;
	if (r_fog_enable.value > 0.f && fabsf (Fog_GetDensity ()) * q_max (0.f, r_fog_density.value) > 0.f)
		return true;
	return false;
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

void GL_ApplyFilmgrainUI (void)
{
	if (r_filmgrain.value <= 0.f || r_filmgrain_affect_ui.value <= 0.f)
		return;
	if (!glprogs.filmgrain || framebufs.composite.color_tex == 0)
		return;

	GL_BeginGroup ("Filmgrain UI");

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
	glViewport (glx, gly, glwidth, glheight);
	glReadBuffer (GL_BACK);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, framebufs.composite.color_tex);
	glCopyTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, glx, gly, glwidth, glheight);

	GL_UseProgram (glprogs.filmgrain);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_SetFilmgrainUniforms (r_filmgrain_amount.value, true);

	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();
}

/*
=============
R_SetupGL
=============
*/
void R_SetupGL (void)
{
	if (!GL_NeedsSceneEffects ())
	{
		GLuint target = GL_NeedsPostprocess () ? framebufs.composite.fbo : 0u;
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
		glViewport (0, 0, r_refdef.vrect.width / r_refdef.scale, r_refdef.vrect.height / r_refdef.scale);
	}
}

/*
=============
R_Clear -- johnfitz -- rewritten and gutted
=============
*/
static void R_LogClearDebug (const char *tag, GLbitfield clearbits)
{
	GLint draw_fbo, read_fbo;
	GLint viewport[4], scissor_box[4];
	GLboolean scissor_test;
	GLfloat clear_color[4];

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, viewport);
	scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetFloatv (GL_COLOR_CLEAR_VALUE, clear_color);

	Con_DPrintf (
		"CLEARDBG %s draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) scissor_test=%d scissor_box=(%d %d %d %d) clear_color=(%.3f %.3f %.3f %.3f) clear_mask=0x%08x\n",
		tag,
		draw_fbo,
		read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_test,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		clear_color[0], clear_color[1], clear_color[2], clear_color[3],
		(unsigned int)clearbits);
}

void R_Clear (void)
{
	GLbitfield clearbits = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
	if (gl_clear.value)
		clearbits |= GL_COLOR_BUFFER_BIT;

	glDisable (GL_SCISSOR_TEST);
	GL_SetState (glstate & ~GLS_NO_ZWRITE); // make sure depth writes are enabled
	glStencilMask (~0u);
	R_LogClearDebug ("R_Clear", clearbits);
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
	GL_BindBufferRange (GL_UNIFORM_BUFFER, FRAME_UBO_BINDING, buf, (GLintptr)ofs, sizeof (r_framedata));
}

/*
===============
R_SetupView -- johnfitz -- this is the stuff that needs to be done once per frame, even in stereo mode
===============
*/
void R_SetupView (void)
{
	r_dlight_buffered_frame = false;
	framesetup.composite_ready = false;

	R_AnimateLight ();

	{
		int overbright_bits = CLAMP (0, (int)Q_rint (r_overbrightbits.value), 3);
		float overbright = (float)(1 << overbright_bits);

		if (overbright > 1.f)
		{
			overbright = GL_TemperedOverbright (overbright);

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
	R_EnvLight_BuildFrameUniforms (r_framedata.lighting_params, r_framedata.lightgrid_params);
	r_framedata.dlight_params[0] = R_DlightsAdditivePassEnabled () ? 1.f : 0.f;
	r_framedata.dlight_params[1] = 0.f;
	r_framedata.dlight_params[2] = 0.f;
	r_framedata.dlight_params[3] = 1.f;
        r_framedata.colorspace_params[0] = CLAMP (0.f, r_debug_colorspace.value, 4.f);
        r_framedata.colorspace_params[1] = 0.f;
        r_framedata.colorspace_params[2] = 0.f;
        r_framedata.colorspace_params[3] = 0.f;
        r_framedata.shader_params[0] = r_shader_debug.value;
        r_framedata.shader_params[1] = r_tcgen_debug.value;
        r_framedata.shader_params[2] = 0.f;
        r_framedata.shader_params[3] = 0.f;
        r_framedata.shadow_params[0] = r_shadow_bias.value;
        r_framedata.shadow_params[1] = r_shadow_normalbias.value;
        r_framedata.shadow_params[2] = r_shadow_pcf.value > 0.f ? 1.f : 0.f;
        r_framedata.shadow_params[3] = r_shadow_pcf_taps.value;
        r_framedata.shadow_debug[0] = (r_shadows.value > 0.f && r_shadow_sun.value > 0.f) ? 1.f : 0.f;
        r_framedata.shadow_debug[1] = r_shadow_debug.value;
        r_framedata.shadow_debug[2] = gl_clipcontrol_able ? 2.f : 1.f;
        r_framedata.shadow_debug[3] = 0.f;
        r_framedata.shadow_dlight_params[0] = r_shadow_dlight_bias.value;
        r_framedata.shadow_dlight_params[1] = r_shadow_dlight_pcf_taps.value;
        r_framedata.shadow_dlight_params[2] = (r_shadows.value > 0.f && r_shadow_dlights.value > 0.f) ? 1.f : 0.f;
        r_framedata.shadow_dlight_params[3] = 0.f;

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
	R_UpdateSunVirtualOrigin ();

	// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();

	//johnfitz -- calculate r_fovx and r_fovy here
	r_fovx = r_refdef.fov_x;
	r_fovy = r_refdef.fov_y;
	water_warp = false;
	{
		int contents = r_viewleaf->contents;
		qboolean forced = M_ForcedUnderwater ();
		qboolean underwater_active = (contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA || cl.forceunderwater || forced);
	if (r_waterwarp.value)
	{
		if (underwater_active)
		{
			double t = forced ? realtime : cl.time;
			if (r_waterwarp.value > 1.f)
			{
				//variance is a percentage of width, where width = 2 * tan(fov / 2) otherwise the effect is too dramatic at high FOV and too subtle at low FOV.  what a mess!
				r_fovx = atan (tan (DEG2RAD (r_refdef.fov_x) / 2) * (0.97 + sin (t * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
				r_fovy = atan (tan (DEG2RAD (r_refdef.fov_y) / 2) * (1.03 - sin (t * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
			}
			else
			{
				water_warp = true;
			}
		}
	}
	// TODO(postfx): hook underwater contents for postfx stack here.
	CL_PostFX_SetContents (contents, underwater_active);
	}
	//johnfitz

	R_SetFrustum ();

	R_MarkSurfaces (); //johnfitz -- create texture chains from PVS

	R_SortEntities ();


	R_PushDlights ();

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
	if (!alphapass)
		R_DlightLogf ("MODELS", "opaque brush=%d alias=%d sprites=%d numlights=%u",
			ofs[2 * mod_brush + 1] - ofs[2 * mod_brush],
			ofs[2 * mod_alias + 1] - ofs[2 * mod_alias],
			cl_modtype_ofs[2 * mod_sprite + 2] - cl_modtype_ofs[2 * mod_sprite],
			r_framedata.numlights);
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

	if (!R_IsViewModelVisible ())
	{
		R_DlightLogf ("VIEWWEAPON", "visible=0");
		return;
	}

	R_DlightLogf ("VIEWWEAPON", "visible=1 model=%s numlights=%u", e->model ? e->model->name : "<none>", r_framedata.numlights);

	GL_BeginGroup ("View model");

	// hack the depth range to prevent view model from poking into walls
	GL_DepthRange (ZRANGE_VIEWMODEL);
	R_DrawAliasModels (&e, 1);
	GL_DepthRange (ZRANGE_FULL);

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
		rcpdelta[i] = 1.f / (gl_farclip.value * vpn[i]);

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
		// Find outgoing links (entity field references other than .chain)
		if ((int)r_showbboxes_links.value & SHOWBBOX_LINK_OUTGOING)
		{
			for (i = 0; i < qcvm->numentityfields; i++)
			{
				eval_t* val = (eval_t*)((char*)&focused->v + qcvm->entityfieldofs[i]);
				if (qcvm->entityfieldofs[i] == offsetof (entvars_t, chain) || !val->edict)
					continue;
				ed = PROG_TO_EDICT (val->edict);
				if (ed == focused || ed->free || ed == sv_player)
					continue;
				R_AddHighlightedEntity (ed, SHOWBBOX_LINK_OUTGOING);
			}
		}

		// Inspect all other edicts to find incoming links
		// (either entity field references or target/targetname matches)
		if ((int)r_showbboxes_links.value & SHOWBBOX_LINK_INCOMING || r_showbboxes_targets.value)
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
				if ((int)r_showbboxes_links.value & SHOWBBOX_LINK_INCOMING)
				{
					for (j = 0; j < qcvm->numentityfields; j++)
					{
						eval_t* val = (eval_t*)((char*)&ed->v + qcvm->entityfieldofs[j]);
						if (qcvm->entityfieldofs[i] == offsetof (entvars_t, chain) || !val->edict)
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
	R_DrawDecals_ShowTris ();

	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_PolygonOffset (OFFSET_NONE);
	if (r_showtris.value == 1)
		GL_DepthRange (ZRANGE_FULL);

	Sbar_Changed (); //so we don't get dots collecting on the statusbar

	GL_EndGroup ();
}

/*
================
R_BeginTranslucency
================
*/
static void R_BeginTranslucency (void)
{
	static const float zeroes[4] = { 0.f, 0.f, 0.f, 0.f };
	static const float ones[4] = { 1.f, 1.f, 1.f, 1.f };

	GL_BeginGroup ("Translucent objects");

	if (R_GetEffectiveAlphaMode () == ALPHAMODE_OIT)
	{
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framesetup.oit_fbo);
		GL_ClearBufferfvFunc (GL_COLOR, 0, zeroes);
		GL_ClearBufferfvFunc (GL_COLOR, 1, ones);

		glEnable (GL_STENCIL_TEST);
		glStencilMask (2);
		glStencilFunc (GL_ALWAYS, 2, 2);
		glStencilOp (GL_KEEP, GL_KEEP, GL_REPLACE);
	}
}

/*
================
R_EndTranslucency
================
*/
static void R_EndTranslucency (void)
{
	if (R_GetEffectiveAlphaMode () == ALPHAMODE_OIT)
	{
		GL_BeginGroup ("OIT resolve");

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framesetup.scene_fbo);

		glStencilFunc (GL_EQUAL, 2, 2);
		glStencilOp (GL_KEEP, GL_KEEP, GL_KEEP);

		GL_UseProgram (glprogs.oit_resolve[framebufs.scene.samples > 1]);
		GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		GL_BindNative (GL_TEXTURE0, framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, framebufs.oit.accum_tex);
		GL_BindNative (GL_TEXTURE1, framebufs.scene.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, framebufs.oit.revealage_tex);

		glDrawArrays (GL_TRIANGLES, 0, 3);

		glDisable (GL_STENCIL_TEST);

		GL_EndGroup ();
	}

        GL_EndGroup (); // translucent objects
}

// Quake3-style dynamic light pass rationale: render only additive dlight
// contributions (albedo * dlight) in a separate pass to preserve contrast of
// the baked lighting while avoiding gamma artifacts from modulating the base
// color. Static lighting remains untouched in the base pass.
static void R_SetDlightConfig (GLuint program, float scale)
{
	if (!program)
		return;

	GL_UseProgram (program);
	GL_Uniform4fFunc (0, scale, r_dlight_radius_scale.value, 1.f, 2.f);
	GL_Uniform4fFunc (1, 0.f, 1.f, 0.f, 0.f);
	GL_Uniform4fFunc (2, 0.f, 0.f, 0.f, 0.f);
}

static void R_DrawDLightPass (void)
{
        int count = 0;
        entity_t **ents;
	qboolean use_buffer = false;

	r_dlight_buffered_frame = false;

	if (!R_DlightsAdditivePassEnabled ())
		return;

        if (r_framedata.numlights == 0 || !r_drawworld_cheatsafe)
                return;

        ents = R_GetVisEntities (mod_brush, false, &count);
        if (count <= 0)
                return;

        GL_BeginGroup ("Dynamic lights (additive)");

	if (framebufs.dlight.fbo && framebufs.scene.samples == 1)
	{
		use_buffer = true;
		r_dlight_buffered_frame = true;
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.dlight.fbo);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
		{
			static const float zeroes[4] = { 0.f, 0.f, 0.f, 0.f };
			GL_ClearBufferfvFunc (GL_COLOR, 0, zeroes);
		}
	}


        r_framedata.dlight_params[2] = 1.f;
        {
                GLuint buf;
                GLbyte *ofs;
                GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
                GL_BindBufferRange (GL_UNIFORM_BUFFER, FRAME_UBO_BINDING, buf, (GLintptr)ofs, sizeof (r_framedata));
        }

		{
		float scale = use_buffer ? 1.f : q_max (0.f, r_dlight_scale.value);
		R_SetDlightConfig (glprogs.world_dlight[0], scale);
		R_SetDlightConfig (glprogs.world_dlight[1], scale);
	}

	GL_SetState (GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (6));
        R_DrawBrushModels_DLights (ents, count);

        r_framedata.dlight_params[2] = 0.f;
        {
                GLuint buf;
                GLbyte *ofs;
                GL_Upload (GL_UNIFORM_BUFFER, &r_framedata, sizeof (r_framedata), &buf, &ofs);
                GL_BindBufferRange (GL_UNIFORM_BUFFER, FRAME_UBO_BINDING, buf, (GLintptr)ofs, sizeof (r_framedata));
        }

	if (use_buffer)
	{
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framesetup.scene_fbo);
		if (framesetup.scene_fbo)
		{
			glDrawBuffer (GL_COLOR_ATTACHMENT0);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
		}
		else
		{
			glDrawBuffer (GL_BACK);
			glReadBuffer (GL_BACK);
		}
	}

        GL_EndGroup ();
}

/*
================
R_RenderScene
================
*/
void R_RenderScene (void)
{
	R_StateDebugMark ("FRAME_START");
	R_ResetViewportAndScissorFullscreen ("WORLD_RESET_BEFORE_SETUP");
	R_SetupScene (); //johnfitz -- this does everything that should be done once per call to RenderScene
	R_Decals_FrameBegin ();
	R_Shadow_SunPass ();
	R_Shadow_DlightPass ();
	R_SetupGL ();
	R_Clear ();
	R_StateDebugMark ("WORLD_BEFORE_GEOMETRY");
	
	// Upload frame data after fog has been set up to ensure fog parameters
	// are available to all draw calls, even when light clustering is skipped.
	R_UploadFrameData ();
	R_StateDebugMark ("WEAPON_BEFORE");
	R_DrawViewModel (); //johnfitz -- moved here from R_RenderView
	R_StateDebugMark ("WEAPON_AFTER");
	S_ExtraUpdate (); // don't let sound get messed up if going slow
	R_DrawEntitiesOnList (false); //johnfitz -- false means this is the pass for nonalpha entities
	R_StateDebugMark ("WORLD_DLIGHT_BEGIN");
	R_LogWorldDlightState ("WORLD_DLIGHT_BEGIN");
	R_DrawDLightPass ();
	R_CompositeDlightBuffer ();
	R_LogWorldDlightState ("WORLD_DLIGHT_END");
	R_StateDebugMark ("WORLD_DLIGHT_END");
	R_DrawDecals (false);
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
	R_StateDebugMark ("WORLD_AFTER_GEOMETRY");
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
void R_WarpScaleView (void)
{
	int srcx, srcy, srcw, srch;
	float smax, tmax;
	qboolean msaa = framebufs.scene.samples > 1;
	qboolean needwarpscale;
	qboolean need_depth_resolve;
	GLuint fbodest;
	double t;

	if (!GL_NeedsSceneEffects ())
		return;

	srcx = glx + r_refdef.vrect.x;
	srcy = gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height;
	srcw = r_refdef.vrect.width / r_refdef.scale;
	srch = r_refdef.vrect.height / r_refdef.scale;

	needwarpscale = r_refdef.scale != 1 || water_warp;
	fbodest = GL_NeedsPostprocess () ? framebufs.composite.fbo : 0;
	need_depth_resolve = (fbodest == framebufs.composite.fbo)
		&& (R_DoFEnabled () || r_ao_method.value > 0.f || r_ao_debug.value > 0.f || (r_fog_enable.value > 0.f && fabsf (Fog_GetDensity ()) * q_max (0.f, r_fog_density.value) > 0.f));

	if (msaa)
	{
		GL_BeginGroup ("MSAA resolve");

		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.scene.fbo);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.resolved_scene.fbo);

		glReadBuffer (GL_COLOR_ATTACHMENT0);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		GL_BlitFramebufferFunc (0, 0, srcw, srch, 0, 0, srcw, srch, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		if (framebufs.scene.velocity_tex && framebufs.resolved_scene.velocity_tex)
		{
			glReadBuffer (GL_COLOR_ATTACHMENT1);
			glDrawBuffer (GL_COLOR_ATTACHMENT1);
			GL_BlitFramebufferFunc (0, 0, srcw, srch, 0, 0, srcw, srch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		GL_EndGroup ();

		if (!needwarpscale)
		{
			GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.resolved_scene.fbo);
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
				GL_BlitFramebufferFunc (0, 0, srcw, srch, srcx, srcy, srcx + srcw, srcy + srch, mask, GL_NEAREST);
			}
		}
	}

	if (need_depth_resolve)
	{
		int dstw = (r_refdef.scale != 1) ? r_refdef.vrect.width : srcw;
		int dsth = (r_refdef.scale != 1) ? r_refdef.vrect.height : srch;

		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.scene.fbo);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		GL_BlitFramebufferFunc (0, 0, srcw, srch, srcx, srcy, srcx + dstw, srcy + dsth, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	}

	if (!msaa && !needwarpscale)
	{
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.scene.fbo);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, fbodest);
		if (fbodest)
			glDrawBuffer (GL_COLOR_ATTACHMENT0);
		else
			glDrawBuffer (GL_BACK);
		GL_BlitFramebufferFunc (0, 0, srcw, srch,
			srcx, srcy, srcx + srcw, srcy + srch,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
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
		if (fbodest == framebufs.composite.fbo)
			framesetup.composite_ready = true;
		R_CompositeDlightBuffer ();
		return;
	}

	GL_BeginGroup ("Warp/scale view");

	smax = srcw / (float)vid.width;
	tmax = srch / (float)vid.height;

	GL_UseProgram (glprogs.warpscale[water_warp]);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));

	t = M_ForcedUnderwater () ? realtime : cl.time;
	GL_Uniform4fFunc (0, smax, tmax, water_warp ? 1.f / 256.f : 0.f, (float)t);
	// View blends are applied after postprocess/UI to avoid AO/tone-map affecting overlays.
	GL_Uniform4fFunc (1, 0.f, 0.f, 0.f, 0.f);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, msaa ? framebufs.resolved_scene.color_tex : framebufs.scene.color_tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, water_warp && msaa ? GL_LINEAR : GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, water_warp && msaa ? GL_LINEAR : GL_NEAREST);

	glDrawArrays (GL_TRIANGLES, 0, 3);

	GL_EndGroup ();

	if (fbodest == framebufs.composite.fbo)
		framesetup.composite_ready = true;
	R_CompositeDlightBuffer ();
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

        R_SetupView (); //johnfitz -- this does everything that should be done once per frame
        R_Atmosphere_Render (false);
        R_RenderScene ();
        R_WarpScaleView ();
        R_Atmosphere_Render (true); // Leave fog disabled for 2D overlays
	R_Shadow_DrawDebug ();

	r_frame_rendered_this_update = true;

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
