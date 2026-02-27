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
#include "rb_gl.h"
#include "r_postfx.h"
#include "cl_postfx.h"
#include "gl_texmgr.h"
#include "image.h"

static GLuint r_postfx_lut_tex;
static int r_postfx_lut_size;

extern cvar_t r_postfx_lut;

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

void R_PostFX_ReloadLUTs (void)
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
	lut_storage = (byte *)calloc (PFX_LUT_COUNT * layer_bytes, 1);
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

	glGenTextures (1, &r_postfx_lut_tex); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#r_postfxc
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D_ARRAY, r_postfx_lut_tex);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#r_postfxc
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#r_postfxc
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#r_postfxc
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#r_postfxc
	GL_TexImage3DFunc (GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size * size, size, PFX_LUT_COUNT, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut_storage);
	GL_ObjectLabelFunc (GL_TEXTURE, r_postfx_lut_tex, -1, "postfx lut");

	free (lut_storage);
	r_postfx_lut_size = size;
}

void R_PostFX_Init (void)
{
	R_PostFX_ReloadLUTs ();
}

void R_PostFX_ReloadLUTs_f (cvar_t *var)
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
