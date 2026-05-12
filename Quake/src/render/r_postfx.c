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
#include "glquake.h"
#include "gl_backend.h"
#include "r_framegraph.h"
#include "r_resources_gl.h"
#include "r_postfx.h"
#include "cl_postfx.h"
#include "gl_texmgr.h"
#include "image.h"

/* LEGACY_GL_HANDLE / PHASE3_BOUNDARY_CANDIDATE:
 * Transitional native texture id exposed in this unit for LUT management.
 * Long-term target: opaque backend resource handle + resolve/upload service. */
static GLuint r_postfx_lut_tex;
static render_texture_handle_t r_postfx_lut_handle = RENDER_TEXTURE_HANDLE_INVALID;
static int r_postfx_lut_size;

static void R_PostFX_ReloadLUTs (void);
static void R_PostFX_ReloadLUTs_f (cvar_t *var);

extern cvar_t r_postfx;
extern cvar_t r_postfx_pickup;
extern cvar_t r_postfx_damage;
extern cvar_t r_post_damage_doublevision;
extern cvar_t r_postfx_powerup;
extern cvar_t r_postfx_underwater;
extern cvar_t r_postfx_quad;
extern cvar_t r_postfx_lut;
extern cvar_t r_postfx_lut_debug_id;
extern cvar_t r_postfx_debug;
extern cvar_t r_refgl_debug;
extern cvar_t r_refgl_log_resources;
extern cvar_t r_refgl_validate_lifetime;
#ifndef RENDERER_PLUGIN_BUILD
extern cvar_t r_backend_debug;
extern cvar_t r_renderer_migration_debug;
#endif

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

static qboolean R_PostFX_TextureHandleDebugEnabled (void)
{
#ifdef RENDERER_PLUGIN_BUILD
	return false;
#else
	return r_renderer_migration_debug.value != 0.f || r_backend_debug.value != 0.f;
#endif
}

static void R_PostFX_DestroyLUTTexture (void)
{
	if (r_postfx_lut_tex)
	{
		if (r_refgl_log_resources.value != 0.f || r_refgl_debug.value != 0.f || (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_BACKEND)))
			Con_DPrintf ("ref_gl: R_PostFX_DestroyLUTTexture tex=%u size=%d\n", (unsigned)r_postfx_lut_tex, r_postfx_lut_size);
		GL_DeleteNativeTexture (r_postfx_lut_tex);
		r_postfx_lut_tex = 0;
	}
	r_postfx_lut_handle = R_TextureHandle_Invalid ();
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

	if (r_refgl_log_resources.value != 0.f || r_refgl_debug.value != 0.f || (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_BACKEND)))
		Con_DPrintf ("ref_gl: R_PostFX_ReloadLUTs begin\n");

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

	r_postfx_lut_tex = (GLuint)R_Backend_CreatePostFXLUTTexture ();
	if (!r_postfx_lut_tex)
	{
		q_free (lut_storage);
		return;
	}
	R_Backend_ConfigurePostFXLUTTexture (r_postfx_lut_tex);
	GL_Backend_UploadPostFXLUTData (r_postfx_lut_tex, lut_storage, size * size, size, PFX_LUT_COUNT);
	r_postfx_lut_handle = GL_Backend_TextureHandleFromNativeTexture ((unsigned)GL_TEXTURE_2D_ARRAY, (unsigned)r_postfx_lut_tex);
	if (!R_TextureHandle_IsValid (r_postfx_lut_handle) && R_PostFX_TextureHandleDebugEnabled ())
		Con_DPrintf ("R_TextureHandle: PostFX LUT native handle registration failed; legacy path remains available\n");

	q_free(lut_storage);
	r_postfx_lut_size = size;

	if (r_refgl_validate_lifetime.value != 0.f || r_refgl_debug.value != 0.f || (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_BACKEND)))
		Con_DPrintf ("ref_gl: R_PostFX_ReloadLUTs done tex=%u size=%d\n", (unsigned)r_postfx_lut_tex, r_postfx_lut_size);
}

void R_PostFX_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_postfx);
	Cvar_RegisterVariable (&r_postfx_pickup);
	Cvar_RegisterVariable (&r_postfx_damage);
	Cvar_RegisterVariable (&r_post_damage_doublevision);
	Cvar_RegisterVariable (&r_postfx_powerup);
	Cvar_RegisterVariable (&r_postfx_underwater);
	Cvar_RegisterVariable (&r_postfx_quad);
	Cvar_RegisterVariable (&r_postfx_lut);
	Cvar_SetCallback (&r_postfx_lut, R_PostFX_ReloadLUTs_f);
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

unsigned R_PostFX_GetLUTTexture (void)
{
	return (unsigned)r_postfx_lut_tex;
}

render_texture_handle_t R_PostFX_GetLUTTextureHandle (void)
{
	return r_postfx_lut_handle;
}

int R_PostFX_GetLUTSize (void)
{
	return r_postfx_lut_size;
}
