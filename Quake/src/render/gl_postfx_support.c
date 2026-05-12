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

#include "quakedef.h"
#include "glquake.h"
#include "r_postfx.h"

extern cvar_t r_srgb_framebuffer;
extern cvar_t r_color_saturation;
extern cvar_t r_color_contrast;
extern cvar_t r_color_midtone;
extern cvar_t r_autoexposure_async;
extern cvar_t r_autoexposure;
extern cvar_t r_exposure_lock;
extern cvar_t r_exposure_bias;
extern cvar_t r_exposure_min;
extern cvar_t r_exposure_max;
extern cvar_t r_exposure_speed_up;
extern cvar_t r_exposure_speed_down;
extern cvar_t r_postfx_bloom_mode;
extern cvar_t r_ref_enable_postfx;
extern cvar_t r_postfx;

static qboolean gl_framebuffer_srgb_enabled = false;
static qboolean gl_srgb_capability_warned = false;

extern float r_autoexposure_debug_luminance;

void GL_SetFramebufferSRGB (qboolean enable)
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

qboolean GL_UseSRGBFramebuffer (void)
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

void GL_PostProcessFallback (void)
{
	int width = glwidth;
	int height = glheight;
	size_t numpixels = (size_t)width * (size_t)height;
	size_t bufsize = numpixels * 4;
	byte *pixels;

	if (framebufs.composite.fbo == 0 || framebufs.composite.color_tex == 0)
		return;

	pixels = (byte *)q_malloc (bufsize);
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

	q_free (pixels);
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

void GL_AutoExposureDeletePBOs (void)
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

void GL_AutoExposureInitPBOs (void)
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
	const qboolean use_async_readback = (r_autoexposure_async.value > 0.f);
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

	if (use_async_readback && framebufs.autoexposure.pbo[0] && framebufs.autoexposure.pbo[1] && GL_AutoExposurePBOAvailable ())
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
		if (use_async_readback && GL_AutoExposurePBOAvailable () && !framebufs.autoexposure.pbo_ready)
			GL_AutoExposureInitPBOs ();
		else if (!use_async_readback && (framebufs.autoexposure.pbo[0] || framebufs.autoexposure.pbo[1]))
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

float GL_UpdateAutoExposure (void)
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
		const float min_scene_luma = q_max (0.f, 0.001f);
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
		const float hard_min_exposure = q_max (0.f, q_min (0.25f, 8.0f));
		const float hard_max_exposure = q_max (hard_min_exposure, q_max (0.25f, 8.0f));
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

float GL_ComputeEffectiveBloomIntensity (float bloom_base, float bloom_boost)
{
	float base = q_max (0.f, bloom_base);
	float boost = q_max (0.f, bloom_boost);

	if (r_postfx_bloom_mode.value > 0.f)
		return q_max (base, boost);
	return base + boost;
}

qboolean GL_PostFXBloomBoostActive (void)
{
	postfx_state_t state;

	if (r_ref_enable_postfx.value == 0.f || r_postfx.value <= 0.f)
		return false;

	R_PostFX_GetState (&state);
	return state.bloom_boost > 0.f;
}
