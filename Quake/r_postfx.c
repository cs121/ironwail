/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2024 Ironwail contributors

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

#include "quakedef.h"
#include "r_postfx.h"
#include "cl_postfx.h"
#include "gl_texmgr.h"
#include "image.h"

static GLuint r_postfx_lut_tex;
static int r_postfx_lut_size;

static void R_PostFX_ReloadLUTs (void);
static void R_PostFX_ReloadLUTs_f (cvar_t *var);

extern cvar_t r_postfx;
extern cvar_t r_polyblend_legacy;
extern cvar_t r_postfx_pickup;
extern cvar_t r_postfx_pickup_exposure;
extern cvar_t r_postfx_pickup_bloom;
extern cvar_t r_postfx_pickup_duration;
extern cvar_t r_postfx_damage;
extern cvar_t r_postfx_damage_vignette;
extern cvar_t r_postfx_damage_vignette_softness;
extern cvar_t r_postfx_damage_desat;
extern cvar_t r_postfx_damage_exposure;
extern cvar_t r_postfx_damage_duration;
extern cvar_t r_postfx_damage_accum_window;
extern cvar_t r_postfx_damage_accum_scale;
extern cvar_t r_post_damage_doublevision;
extern cvar_t r_post_damage_dv_strength;
extern cvar_t r_post_damage_dv_px;
extern cvar_t r_post_damage_dv_freq;
extern cvar_t r_post_damage_trauma_scale;
extern cvar_t r_post_damage_trauma_decay;
extern cvar_t r_post_damage_dv_quality;
extern cvar_t r_post_damage_dv_debug;
extern cvar_t r_postfx_powerup;
extern cvar_t r_postfx_powerup_lut_strength;
extern cvar_t r_postfx_powerup_ramp_in;
extern cvar_t r_postfx_powerup_ramp_out;
extern cvar_t r_postfx_underwater;
extern cvar_t r_postfx_underwater_grade_strength;
extern cvar_t r_postfx_underwater_fog_strength;
extern cvar_t r_postfx_underwater_ramp_in;
extern cvar_t r_postfx_underwater_ramp_out;
extern cvar_t r_postfx_underwater_fog_water_r;
extern cvar_t r_postfx_underwater_fog_water_g;
extern cvar_t r_postfx_underwater_fog_water_b;
extern cvar_t r_postfx_underwater_fog_slime_r;
extern cvar_t r_postfx_underwater_fog_slime_g;
extern cvar_t r_postfx_underwater_fog_slime_b;
extern cvar_t r_postfx_underwater_fog_lava_r;
extern cvar_t r_postfx_underwater_fog_lava_g;
extern cvar_t r_postfx_underwater_fog_lava_b;
extern cvar_t r_postfx_quad;
extern cvar_t r_postfx_quad_emissive_boost;
extern cvar_t r_postfx_quad_bloom_boost;
extern cvar_t r_postfx_quad_pulse_speed;
extern cvar_t r_postfx_quad_pulse_intensity;
extern cvar_t r_postfx_bloom_mode;
extern cvar_t r_postfx_lut;
extern cvar_t r_postfx_lut_strength_powerup;
extern cvar_t r_postfx_lut_strength_underwater;
extern cvar_t r_postfx_lut_debug_id;
extern cvar_t r_postfx_debug;

static const char *const postfx_lut_files[PFX_LUT_COUNT] =
{
	NULL,
	"gfx/lut/water",
	"gfx/lut/slime",
	"gfx/lut/lava",
	"gfx/lut/pent",
	"gfx/lut/ring",
	"gfx/lut/suit",
	"gfx/lut/quad"
};

static void R_PostFX_DestroyLUTTexture (void)
{
	if (r_postfx_lut_tex)
	{
		GL_DeleteNativeTexture (r_postfx_lut_tex);
		r_postfx_lut_tex = 0;
	}
	r_postfx_lut_size = 0;
}

static void R_PostFX_GenerateIdentityLUT (byte *buffer, int size)
{
	int x, y, z;
	int width = size * size;
	for (z = 0; z < size; ++z)
	{
		for (y = 0; y < size; ++y)
		{
			for (x = 0; x < size; ++x)
			{
				int index = (y * width + z * size + x) * 4;
				float r = x / (float)(size - 1);
				float g = y / (float)(size - 1);
				float b = z / (float)(size - 1);
				buffer[index + 0] = (byte)CLAMP (0, (int)(r * 255.f + 0.5f), 255);
				buffer[index + 1] = (byte)CLAMP (0, (int)(g * 255.f + 0.5f), 255);
				buffer[index + 2] = (byte)CLAMP (0, (int)(b * 255.f + 0.5f), 255);
				buffer[index + 3] = 255;
			}
		}
	}
}

static void R_PostFX_ReloadLUTs (void)
{
	int i;
	int width;
	int height;
	int size = 0;
	int layer_bytes;
	byte *layer_data;
	byte *lut_storage;
	enum srcformat fmt;

	R_PostFX_DestroyLUTTexture ();

	if (r_postfx_lut.value <= 0.f)
		return;

	for (i = 1; i < PFX_LUT_COUNT; ++i)
	{
		int mark = Hunk_LowMark ();
		byte *data = Image_LoadImage (postfx_lut_files[i], &width, &height, &fmt);
		if (!data)
		{
			Hunk_FreeToLowMark (mark);
			continue;
		}
		if (fmt != SRC_RGBA)
		{
			Con_Printf ("PostFX LUT %s has unsupported format\n", postfx_lut_files[i]);
			Hunk_FreeToLowMark (mark);
			continue;
		}
		if (height <= 0 || width != height * height)
		{
			Con_Printf ("PostFX LUT %s has invalid dimensions %dx%d\n", postfx_lut_files[i], width, height);
			Hunk_FreeToLowMark (mark);
			continue;
		}
		if (size == 0)
			size = height;
		if (size != height)
		{
			Con_Printf ("PostFX LUT %s has mismatched size %d (expected %d)\n", postfx_lut_files[i], height, size);
			Hunk_FreeToLowMark (mark);
			continue;
		}
		Hunk_FreeToLowMark (mark);
	}

	if (size <= 0)
		return;

	layer_bytes = size * size * size * 4;
	lut_storage = (byte *)q_calloc(PFX_LUT_COUNT * layer_bytes, 1);
	if (!lut_storage)
		return;

	for (i = 1; i < PFX_LUT_COUNT; ++i)
	{
		int mark = Hunk_LowMark ();
		byte *data = Image_LoadImage (postfx_lut_files[i], &width, &height, &fmt);
		layer_data = lut_storage + i * layer_bytes;
		if (!data || fmt != SRC_RGBA || height != size || width != size * size)
		{
			R_PostFX_GenerateIdentityLUT (layer_data, size);
			Hunk_FreeToLowMark (mark);
			continue;
		}
		memcpy (layer_data, data, layer_bytes);
		Hunk_FreeToLowMark (mark);
	}
	R_PostFX_GenerateIdentityLUT (lut_storage, size);

	glGenTextures (1, &r_postfx_lut_tex);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D_ARRAY, r_postfx_lut_tex);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GL_TexImage3DFunc (GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size * size, size, PFX_LUT_COUNT, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut_storage);
	GL_ObjectLabelFunc (GL_TEXTURE, r_postfx_lut_tex, -1, "postfx lut");

	q_free(lut_storage);
	r_postfx_lut_size = size;
}

void R_PostFX_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_postfx);
	Cvar_RegisterVariable (&r_polyblend_legacy);
	Cvar_RegisterVariable (&r_postfx_pickup);
	Cvar_RegisterVariable (&r_postfx_pickup_exposure);
	Cvar_RegisterVariable (&r_postfx_pickup_bloom);
	Cvar_RegisterVariable (&r_postfx_pickup_duration);
	Cvar_RegisterVariable (&r_postfx_damage);
	Cvar_RegisterVariable (&r_postfx_damage_vignette);
	Cvar_RegisterVariable (&r_postfx_damage_vignette_softness);
	Cvar_RegisterVariable (&r_postfx_damage_desat);
	Cvar_RegisterVariable (&r_postfx_damage_exposure);
	Cvar_RegisterVariable (&r_postfx_damage_duration);
	Cvar_RegisterVariable (&r_postfx_damage_accum_window);
	Cvar_RegisterVariable (&r_postfx_damage_accum_scale);
	Cvar_RegisterVariable (&r_post_damage_doublevision);
	Cvar_RegisterVariable (&r_post_damage_dv_strength);
	Cvar_RegisterVariable (&r_post_damage_dv_px);
	Cvar_RegisterVariable (&r_post_damage_dv_freq);
	Cvar_RegisterVariable (&r_post_damage_trauma_scale);
	Cvar_RegisterVariable (&r_post_damage_trauma_decay);
	Cvar_RegisterVariable (&r_post_damage_dv_quality);
	Cvar_RegisterVariable (&r_post_damage_dv_debug);
	Cvar_RegisterVariable (&r_postfx_powerup);
	Cvar_RegisterVariable (&r_postfx_powerup_lut_strength);
	Cvar_RegisterVariable (&r_postfx_powerup_ramp_in);
	Cvar_RegisterVariable (&r_postfx_powerup_ramp_out);
	Cvar_RegisterVariable (&r_postfx_underwater);
	Cvar_RegisterVariable (&r_postfx_underwater_grade_strength);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_strength);
	Cvar_RegisterVariable (&r_postfx_underwater_ramp_in);
	Cvar_RegisterVariable (&r_postfx_underwater_ramp_out);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_water_r);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_water_g);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_water_b);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_slime_r);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_slime_g);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_slime_b);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_lava_r);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_lava_g);
	Cvar_RegisterVariable (&r_postfx_underwater_fog_lava_b);
	Cvar_RegisterVariable (&r_postfx_quad);
	Cvar_RegisterVariable (&r_postfx_quad_emissive_boost);
	Cvar_RegisterVariable (&r_postfx_quad_bloom_boost);
	Cvar_RegisterVariable (&r_postfx_quad_pulse_speed);
	Cvar_RegisterVariable (&r_postfx_quad_pulse_intensity);
	Cvar_RegisterVariable (&r_postfx_bloom_mode);
	Cvar_RegisterVariable (&r_postfx_lut);
	Cvar_SetCallback (&r_postfx_lut, R_PostFX_ReloadLUTs_f);
	Cvar_RegisterVariable (&r_postfx_lut_strength_powerup);
	Cvar_RegisterVariable (&r_postfx_lut_strength_underwater);
	Cvar_RegisterVariable (&r_postfx_lut_debug_id);
	Cvar_RegisterVariable (&r_postfx_debug);
}

void R_PostFX_Init (void)
{
	R_PostFX_ReloadLUTs ();
}

static void R_PostFX_ReloadLUTs_f (cvar_t *var)
{
	(void)var;
	R_PostFX_ReloadLUTs ();
}

void R_PostFX_GetState (postfx_state_t *out_state)
{
	CL_PostFX_GetState (out_state);
}

GLuint R_PostFX_GetLUTTexture (void)
{
	return r_postfx_lut_tex;
}

int R_PostFX_GetLUTSize (void)
{
	return r_postfx_lut_size;
}
