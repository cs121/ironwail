/*
 * snd_sdl.c - SDL audio driver for Hexen II: Hammer of Thyrion (uHexen2)
 * based on implementations found in the quakeforge and ioquake3 projects.
 *
 * Copyright (C) 1999-2005 Id Software, Inc.
 * Copyright (C) 2005-2012 O.Sezer <sezero@users.sourceforge.net>
 * Copyright (C) 2010-2014 QuakeSpasm developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL.h>
#else
#include "SDL.h"
#endif

static int	buffersize;
static SDL_AudioDeviceID sdl_audio_device;


static void SDLCALL paint_audio (void *unused, Uint8 *stream, int len)
{
	int	pos, tobufend;
	int	len1, len2;

	if (!shm)
	{	/* shouldn't happen, but just in case */
		memset(stream, 0, len);
		return;
	}

	pos = (shm->samplepos * (shm->samplebits / 8));
	if (pos >= buffersize)
		shm->samplepos = pos = 0;

	tobufend = buffersize - pos;  /* bytes to buffer's end. */
	len1 = len;
	len2 = 0;

	if (len1 > tobufend)
	{
		len1 = tobufend;
		len2 = len - len1;
	}

	memcpy(stream, shm->buffer + pos, len1);

	if (len2 <= 0)
	{
		shm->samplepos += (len1 / (shm->samplebits / 8));
	}
	else
	{	/* wraparound? */
		memcpy(stream + len1, shm->buffer, len2);
		shm->samplepos = (len2 / (shm->samplebits / 8));
	}

	if (shm->samplepos >= buffersize)
		shm->samplepos = 0;
}

qboolean SNDDMA_Init (dma_t *dma)
{
	SDL_AudioSpec desired;
	SDL_AudioSpec obtained;
	int		tmp, val;
	char	drivername[128];
	const char *driver, *device;

	TexMgr_Trace ("SNDDMA_Init: begin");
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		TexMgr_Trace ("SNDDMA_Init: SDL_InitSubSystem failed");
		Con_Printf("Couldn't init SDL audio: %s\n", SDL_GetError());
		return false;
	}
	TexMgr_Trace ("SNDDMA_Init: SDL_InitSubSystem ok");

	/* Set up the desired format */
	desired.freq = snd_mixspeed.value;
	desired.format = (loadas8bit.value) ? AUDIO_U8 : AUDIO_S16SYS;
	desired.channels = 2; /* = desired_channels; */
	if (desired.freq <= 11025)
		desired.samples = 256;
	else if (desired.freq <= 22050)
		desired.samples = 512;
	else if (desired.freq <= 44100)
		desired.samples = 1024;
	else if (desired.freq <= 56000)
		desired.samples = 2048; /* for 48 kHz */
	else
		desired.samples = 4096; /* for 96 kHz */
	desired.callback = paint_audio;
	desired.userdata = NULL;
	TexMgr_Trace ("SNDDMA_Init: desired spec prepared");

	/* Open the audio device */
	TexMgr_Trace ("SNDDMA_Init: SDL_OpenAudioDevice begin");
	sdl_audio_device = SDL_OpenAudioDevice (NULL, 0, &desired, &obtained, 0);
	TexMgr_Trace ("SNDDMA_Init: SDL_OpenAudioDevice end");
	if (sdl_audio_device == 0)
	{
		TexMgr_Trace ("SNDDMA_Init: SDL_OpenAudioDevice failed");
		Con_Printf("Couldn't open SDL audio: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return false;
	}

	TexMgr_Trace ("SNDDMA_Init: DMA clear begin");
	memset ((void *) dma, 0, sizeof(dma_t));
	shm = dma;
	TexMgr_Trace ("SNDDMA_Init: DMA clear end");

	/* Fill the audio DMA information block using the obtained format. */
	shm->samplebits = (obtained.format & 0xFF); /* first byte of format is bits */
	shm->signed8 = (obtained.format == AUDIO_S8);
	shm->speed = obtained.freq;
	shm->channels = obtained.channels;
	tmp = (obtained.samples * obtained.channels) * 10;
	if (tmp & (tmp - 1))
	{	/* make it a power of two */
		val = 1;
		while (val < tmp)
			val <<= 1;

		tmp = val;
	}
	shm->samples = tmp;
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	TexMgr_Trace ("SNDDMA_Init: DMA config end");

	TexMgr_Trace ("SNDDMA_Init: SDL audio spec print begin");
	{
		char tracebuf[128];
		q_snprintf (tracebuf, sizeof (tracebuf),
			"SDL audio spec  : %d Hz, %d samples, %d channels",
			obtained.freq, obtained.samples, obtained.channels);
		TexMgr_Trace (tracebuf);
	}
	TexMgr_Trace ("SNDDMA_Init: SDL audio spec print end");

	TexMgr_Trace ("SNDDMA_Init: audio driver query begin");
	driver = SDL_GetCurrentAudioDriver();
	device = SDL_GetAudioDeviceName(0, SDL_FALSE);
	TexMgr_Trace ("SNDDMA_Init: audio driver query end");
	q_snprintf(drivername, sizeof(drivername), "%s - %s",
		driver != NULL ? driver : "(UNKNOWN)",
		device != NULL ? device : "(UNKNOWN)");
	TexMgr_Trace ("SNDDMA_Init: driver name formatted");
	buffersize = shm->samples * (shm->samplebits / 8);
	TexMgr_Trace ("SNDDMA_Init: buffersize computed");
	TexMgr_Trace ("SNDDMA_Init: SDL audio driver print begin");
	{
		char tracebuf[256];
		q_snprintf (tracebuf, sizeof (tracebuf),
			"SDL audio driver: %s, %d bytes buffer", drivername, buffersize);
		TexMgr_Trace (tracebuf);
	}
	TexMgr_Trace ("SNDDMA_Init: SDL audio driver print end");

	TexMgr_Trace ("SNDDMA_Init: buffer alloc begin");
	shm->buffer = (unsigned char *) q_calloc(1, buffersize);
	TexMgr_Trace ("SNDDMA_Init: buffer alloc end");
	if (!shm->buffer)
	{
		TexMgr_Trace ("SNDDMA_Init: buffer alloc failed");
		SDL_CloseAudioDevice (sdl_audio_device);
		sdl_audio_device = 0;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		shm = NULL;
		Con_Printf ("Failed allocating memory for SDL audio\n");
		return false;
	}

	TexMgr_Trace ("SNDDMA_Init: SDL_PauseAudioDevice begin");
	SDL_PauseAudioDevice (sdl_audio_device, 0);
	TexMgr_Trace ("SNDDMA_Init: SDL_PauseAudioDevice end");

	TexMgr_Trace ("SNDDMA_Init: end");
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	return shm->samplepos;
}

void SNDDMA_Shutdown (void)
{
	if (shm)
	{
		Con_Printf ("Shutting down SDL sound\n");
		SDL_CloseAudioDevice (sdl_audio_device);
		sdl_audio_device = 0;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		if (shm->buffer)
			q_free(shm->buffer);
		shm->buffer = NULL;
		shm = NULL;
	}
}

void SNDDMA_LockBuffer (void)
{
	if (sdl_audio_device)
		SDL_LockAudioDevice (sdl_audio_device);
}

void SNDDMA_Submit (void)
{
	if (sdl_audio_device)
		SDL_UnlockAudioDevice (sdl_audio_device);
}

void SNDDMA_BlockSound (void)
{
	if (sdl_audio_device)
		SDL_PauseAudioDevice (sdl_audio_device, 1);
}

void SNDDMA_UnblockSound (void)
{
	if (sdl_audio_device)
		SDL_PauseAudioDevice (sdl_audio_device, 0);
}
