/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2010-2011 O. Sezer <sezero@users.sourceforge.net>
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
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "quakedef.h"
#include "sounddef.h"

#define	PAINTBUFFER_SIZE	2048
#define S_REVERB_BUFFER_SAMPLES	32768
portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
static portable_samplepair_t reverbbuffer[PAINTBUFFER_SIZE];
int		snd_scaletable[32][256];

static int	snd_vol;

static float	snd_lofreqlevel;
static float	snd_hifreqlevel;

static struct
{
	float	left[S_REVERB_BUFFER_SAMPLES];
	float	right[S_REVERB_BUFFER_SAMPLES];
	int	index;
} reverb_state;

static float S_SoftClipSample (float sample)
{
	if (fabsf (sample) <= 1.f)
		return sample;

	return sample / (1.f + fabsf (sample));
}

static short S_OutputSample16 (int sample)
{
	float normalized = sample / (32768.f * 256.f);
	return (short) lrintf (S_SoftClipSample (normalized) * 32767.f);
}

static signed char S_OutputSample8Signed (int sample)
{
	float normalized = sample / (32768.f * 256.f);
	return (signed char) lrintf (S_SoftClipSample (normalized) * 127.f);
}

static unsigned char S_OutputSample8Unsigned (int sample)
{
	float normalized = S_SoftClipSample (sample / (32768.f * 256.f));
	return (unsigned char) lrintf ((normalized * 127.f) + 128.f);
}

static void S_TransferStereo16 (int endtime)
{
	int		lpos;
	int		lpaintedtime;
	int		sample_offset;
	lpaintedtime = paintedtime;

	while (lpaintedtime < endtime)
	{
	// handle recirculating buffer issues
		lpos = lpaintedtime & ((shm->samples >> 1) - 1);
		sample_offset = lpaintedtime - paintedtime;

		{
			short *out = (short *)shm->buffer + (lpos << 1);
			int sample_count = (shm->samples >> 1) - lpos;
			int i;

			if (lpaintedtime + sample_count > endtime)
				sample_count = endtime - lpaintedtime;

			for (i = 0; i < sample_count; ++i)
			{
				const portable_samplepair_t *sample = &paintbuffer[sample_offset + i];
				out[i * 2 + 0] = S_OutputSample16 (sample->left);
				out[i * 2 + 1] = S_OutputSample16 (sample->right);
			}

			lpaintedtime += sample_count;
		}
	}
}

static void S_TransferPaintBuffer (int endtime)
{
	int	out_idx, out_mask;
	int	count, step;
	int	*p;

	if (shm->samplebits == 16 && shm->channels == 2)
	{
		S_TransferStereo16 (endtime);
		return;
	}

	p = (int *) paintbuffer;
	count = (endtime - paintedtime) * shm->channels;
	out_mask = shm->samples - 1;
	out_idx = paintedtime * shm->channels & out_mask;
	step = 3 - shm->channels;

	if (shm->samplebits == 16)
	{
		short *out = (short *)shm->buffer;
		while (count--)
		{
			const int sample = *p;
			p+= step;
			out[out_idx] = S_OutputSample16 (sample);
			out_idx = (out_idx + 1) & out_mask;
		}
	}
	else if (shm->samplebits == 8 && !shm->signed8)
	{
		unsigned char *out = shm->buffer;
		while (count--)
		{
			const int sample = *p;
			p+= step;
			out[out_idx] = S_OutputSample8Unsigned (sample);
			out_idx = (out_idx + 1) & out_mask;
		}
	}
	else if (shm->samplebits == 8)	/* S8 format, e.g. with Amiga AHI */
	{
		signed char *out = (signed char *) shm->buffer;
		while (count--)
		{
			const int sample = *p;
			p+= step;
			out[out_idx] = S_OutputSample8Signed (sample);
			out_idx = (out_idx + 1) & out_mask;
		}
	}
}

/*
==============
S_MakeBlackmanWindowKernel

Makes a lowpass filter kernel, from equation 16-4 in
"The Scientist and Engineer's Guide to Digital Signal Processing"

M is the kernel size (not counting the center point), must be even
kernel has room for M+1 floats
f_c is the filter cutoff frequency, as a fraction of the samplerate
==============
*/
static void S_MakeBlackmanWindowKernel(float *kernel, int M, float f_c)
{
	int i;
	for (i = 0; i <= M; i++)
	{
		if (i == M/2)
		{
			kernel[i] = 2 * M_PI * f_c;
		}
		else
		{
			kernel[i] = ( sin(2 * M_PI * f_c * (i - M/2.0)) / (i - (M/2.0)) )
				* (0.42 - 0.5*cos(2 * M_PI * i / (double)M)
				   + 0.08*cos(4 * M_PI * i / (double)M) );
		}
	}

// normalize the kernel so all of the values sum to 1
	{
		float sum = 0;
		for (i = 0; i <= M; i++)
		{
			sum += kernel[i];
		}

		for (i = 0; i <= M; i++)
		{
			kernel[i] /= sum;
		}
	}
}

typedef struct {
	float *memory;  // kernelsize floats
	float *kernel;  // kernelsize floats
	int kernelsize; // M+1, rounded up to be a multiple of 16
	int M;			// M value used to make kernel, even
	int parity;		// 0-3
	float f_c;		// cutoff frequency, [0..1], fraction of sample rate
} filter_t;

static void S_UpdateFilter(filter_t *filter, int M, float f_c)
{
	if (filter->f_c != f_c || filter->M != M)
	{
		if (filter->memory != NULL) q_free(filter->memory);
		if (filter->kernel != NULL) q_free(filter->kernel);

		filter->M = M;
		filter->f_c = f_c;

		filter->parity = 0;
	// M + 1 rounded up to the next multiple of 16
		filter->kernelsize = (M + 1) + 16 - ((M + 1) % 16);
		filter->memory = (float *) q_calloc(filter->kernelsize, sizeof(float));
		filter->kernel = (float *) q_calloc(filter->kernelsize, sizeof(float));

		if (!filter->memory || !filter->kernel)
			Sys_Error ("S_UpdateFilter: out of memory (%" SDL_PRIu64 " bytes)", (uint64_t)(filter->kernelsize * sizeof (float)));

		S_MakeBlackmanWindowKernel(filter->kernel, M, f_c);
	}
}

/*
==============
S_ApplyFilter

Lowpass-filter the given buffer containing 44100Hz audio.

As an optimization, it decimates the audio to 11025Hz (setting every sample
position that's not a multiple of 4 to 0), then convoluting with the filter
kernel is 4x faster, because we can skip 3/4 of the input samples that are
known to be 0 and skip 3/4 of the filter kernel.
==============
*/
static void S_ApplyFilter(filter_t *filter, int *data, int stride, int count)
{
	int i, j;
	size_t inputsize;
	float *input;
	const int kernelsize = filter->kernelsize;
	const float *kernel = filter->kernel;
	int mark;
	int parity;

	mark = Hunk_LowMark ();
	inputsize = sizeof(float) * (filter->kernelsize + count);
	input = (float *) Hunk_AllocNoFill (inputsize);

// set up the input buffer
// memory holds the previous filter->kernelsize samples of input.
	memcpy(input, filter->memory, filter->kernelsize * sizeof(float));

	for (i=0; i<count; i++)
	{
		input[filter->kernelsize+i] = data[i * stride] / (32768.0 * 256.0);
	}

// copy out the last filter->kernelsize samples to 'memory' for next time
	memcpy(filter->memory, input + count, filter->kernelsize * sizeof(float));

// apply the filter
	parity = filter->parity;

	for (i=0; i<count; i++)
	{
		const float *input_plus_i = input + i;
		float val[4] = {0, 0, 0, 0};

		for (j = (4 - parity) % 4; j < kernelsize; j+=16)
		{
			val[0] += kernel[j] * input_plus_i[j];
			val[1] += kernel[j+4] * input_plus_i[j+4];
			val[2] += kernel[j+8] * input_plus_i[j+8];
			val[3] += kernel[j+12] * input_plus_i[j+12];
		}

	// 4.0 factor is to increase volume by 12 dB; this is to make up the
	// volume drop caused by the zero-filling this filter does.
		data[i * stride] = (val[0] + val[1] + val[2] + val[3])
			* (32768.0 * 256.0 * 4.0);

		parity = (parity + 1) % 4;
	}

	filter->parity = parity;

	Hunk_FreeToLowMark (mark);
}

/*
==============
S_LowpassFilter

lowpass filters 24-bit integer samples in 'data' (stored in 32-bit ints).
assumes 44100Hz sample rate, and lowpasses at around 5kHz
memory should be a zero-filled filter_t struct
==============
*/
static void S_LowpassFilter(int *data, int stride, int count,
							filter_t *memory)
{
	int M;
	float bw, f_c;

	switch ((int)snd_filterquality.value)
	{
	case 1:
		M = 126; bw = 0.900; break;
	case 2:
		M = 150; bw = 0.915; break;
	case 3:
		M = 174; bw = 0.930; break;
	case 4:
		M = 198; bw = 0.945; break;
	case 5:
	default:
		M = 222; bw = 0.960; break;
	}

	f_c = (bw * 11025 / 2.0) / 44100.0;

	S_UpdateFilter(memory, M, f_c);
	S_ApplyFilter(memory, data, stride, count);
}

/*
===============================================================================

UNDERWATER EFFECT

===============================================================================
*/

static struct {
	float	intensity;
	float	alpha;
	float	accum[2];
} underwater = {0.f, 1.f, {0.f, 0.f}};

extern cvar_t snd_waterfx;

void S_SetUnderwaterIntensity (float target)
{
	target *= CLAMP (0.f, snd_waterfx.value, 2.f);
	if (underwater.intensity < target)
	{
		underwater.intensity += host_frametime * 4.f;
		underwater.intensity = q_min (underwater.intensity, target);
	}
	else if (underwater.intensity > target)
	{
		underwater.intensity -= host_frametime * 4.f;
		underwater.intensity = q_max (underwater.intensity, target);
	}
	underwater.alpha = exp (-underwater.intensity * log (12.f));
}

static void S_UnderwaterFilter (int endtime)
{
	int i;
	if (!underwater.intensity)
	{
		if (endtime > 0)
		{
			underwater.accum[0] = paintbuffer[endtime-1].left;
			underwater.accum[1] = paintbuffer[endtime-1].right;
		}
		return;
	}
	for (i = 0; i < endtime; i++)
	{
		underwater.accum[0] += underwater.alpha * (paintbuffer[i].left  - underwater.accum[0]);
		underwater.accum[1] += underwater.alpha * (paintbuffer[i].right - underwater.accum[1]);
		paintbuffer[i].left  = (int) underwater.accum[0];
		paintbuffer[i].right = (int) underwater.accum[1];
	}
}

static void S_UpdateLevels (int endtime)
{
	int i;
	float scale;

	if (snd_vol <= 0)
	{
		snd_lofreqlevel = snd_hifreqlevel = 0.f;
		return;
	}

	scale = 0.5f / (snd_vol * 32768.f);
	for (i = 0; i < endtime; i++)
	{
		float sample = (abs (paintbuffer[i].left) + abs (paintbuffer[i].right)) * scale;
		snd_lofreqlevel = LERP (snd_lofreqlevel, sample, 1e-3f);
		snd_hifreqlevel = LERP (snd_hifreqlevel, sample, 1e-2f);
	}
}

float S_GetLoFreqLevel (void)
{
	return snd_lofreqlevel;
}

float S_GetHiFreqLevel (void)
{
	return snd_hifreqlevel;
}

void S_ResetReverbState (void)
{
	memset (&reverb_state, 0, sizeof (reverb_state));
	memset (reverbbuffer, 0, sizeof (reverbbuffer));
}

/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

static int SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int endtime, int paintbufferstart);
static int SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int endtime, int paintbufferstart);

static void S_ReverbAccumulateSample (int index, float left, float right, float send)
{
	send = CLAMP (0.f, send, 1.f);
	if (send <= 0.f)
		return;

	reverbbuffer[index].left += (int) lrintf (left * send);
	reverbbuffer[index].right += (int) lrintf (right * send);
}

static void S_ApplyReverb (int count)
{
	const float output_gain = S_GetReverbVolume ();
	const int delay_a = q_min (S_REVERB_BUFFER_SAMPLES - 1, q_max (1, (int) ((double) shm->speed * 0.029)));
	const int delay_b = q_min (S_REVERB_BUFFER_SAMPLES - 1, q_max (1, (int) ((double) shm->speed * 0.037)));
	const int delay_c = q_min (S_REVERB_BUFFER_SAMPLES - 1, q_max (1, (int) ((double) shm->speed * 0.041)));
	const int mask = S_REVERB_BUFFER_SAMPLES - 1;
	const float scale = 32768.f * 256.f;
	int i;

	for (i = 0; i < count; i++)
	{
		const int index = reverb_state.index;
		const int tap_a = (index - delay_a) & mask;
		const int tap_b = (index - delay_b) & mask;
		const int tap_c = (index - delay_c) & mask;
		const float input_l = reverbbuffer[i].left / scale;
		const float input_r = reverbbuffer[i].right / scale;
		const float wet_l = reverb_state.left[tap_a] * 0.55f + reverb_state.left[tap_b] * 0.25f + reverb_state.right[tap_c] * 0.20f;
		const float wet_r = reverb_state.right[tap_a] * 0.55f + reverb_state.right[tap_b] * 0.25f + reverb_state.left[tap_c] * 0.20f;

		reverb_state.left[index] = CLAMP (-1.f, input_l + wet_l * 0.45f, 1.f);
		reverb_state.right[index] = CLAMP (-1.f, input_r + wet_r * 0.45f, 1.f);
		reverb_state.index = (index + 1) & mask;

		if (output_gain > 0.f)
		{
			paintbuffer[i].left += (int) lrintf (wet_l * output_gain * scale);
			paintbuffer[i].right += (int) lrintf (wet_r * output_gain * scale);
		}
	}
}

static int SND_ChannelSamplesUntilEndLocal (const channel_t *ch, const sfxcache_t *sc)
{
	float remaining;

	if (!sc || ch->step <= 0.f)
		return 0;

	remaining = (float) sc->length - ch->pos;
	if (remaining <= 0.f)
		return 0;

	return q_max (1, (int) ceilf (remaining / ch->step));
}

static void SND_StopChannelLocal (channel_t *ch)
{
	ch->sfx = NULL;
	ch->start = 0;
	ch->end = 0;
	ch->pos = 0.f;
	ch->looping = -1;
	ch->step = 1.f;
	ch->base_step = 1.f;
	ch->lowpass_alpha = 1.f;
	ch->lowpass_history = 0.f;
	ch->voice_id = 0;
	ch->def_id = 0;
	ch->def_instance_id = 0;
}

void S_PaintChannels (int endtime)
{
	int		i;
	int		end, ltime, count;
	channel_t	*ch;
	sfxcache_t	*sc;

	snd_vol = sfxvolume.value * 256;

	while (paintedtime < endtime)
	{
	// if paintbuffer is smaller than DMA buffer
		end = endtime;
		if (endtime - paintedtime > PAINTBUFFER_SIZE)
			end = paintedtime + PAINTBUFFER_SIZE;

	// clear the paint buffer
		memset(paintbuffer, 0, (end - paintedtime) * sizeof(portable_samplepair_t));
		memset(reverbbuffer, 0, (end - paintedtime) * sizeof(portable_samplepair_t));

	// paint in the channels.
		ch = snd_channels;
		for (i = 0; i < total_channels; i++, ch++)
		{
			if (!ch->sfx)
				continue;
			if (!ch->leftvol && !ch->rightvol)
				continue;
			sc = S_LoadSound (ch->sfx);
			if (!sc)
				continue;

			ltime = paintedtime;

			while (ltime < end)
			{	// paint up to end
				if (ltime < ch->start)
				{
					if (ch->start >= end)
						break;
					ltime = ch->start;
					continue;
				}

				count = end - ltime;
				if (ch->end < ltime + count)
					count = ch->end - ltime;

				if (count > 0)
				{
					int mixed;

					// the last param to SND_PaintChannelFrom is the index
					// to start painting to in the paintbuffer, usually 0.
					if (sc->width == 1)
						mixed = SND_PaintChannelFrom8(ch, sc, count, ltime - paintedtime);
					else
						mixed = SND_PaintChannelFrom16(ch, sc, count, ltime - paintedtime);

					ltime += mixed;
					count = mixed;
				}

				// if at end of loop, restart
				if ((count <= 0 || ch->pos >= sc->length) && ch->looping >= 0)
				{
					if (ch->step <= 0.f || ch->looping >= sc->length)
					{
						SND_StopChannelLocal (ch);
						break;
					}

					ch->pos = ch->looping;
					ch->end = ltime + SND_ChannelSamplesUntilEndLocal (ch, sc);
					if (ch->end <= ltime)
					{
						SND_StopChannelLocal (ch);
						break;
					}
				}
				else if (count <= 0 || ch->pos >= sc->length)
				{	// channel just stopped
					SND_StopChannelLocal (ch);
					break;
				}
				else
				{
					ch->end = ltime + SND_ChannelSamplesUntilEndLocal (ch, sc);
				}
			}
		}

	// clip each sample to 0dB, then reduce by 6dB (to leave some headroom for
	// the lowpass filter and the music). the lowpass will smooth out the
	// clipping
		for (i=0; i<end-paintedtime; i++)
		{
			paintbuffer[i].left = CLAMP(-32768 * 256, paintbuffer[i].left, 32767 * 256) / 2;
			paintbuffer[i].right = CLAMP(-32768 * 256, paintbuffer[i].right, 32767 * 256) / 2;
		}

	// apply a lowpass filter
		if (sndspeed.value == 11025 && shm->speed == 44100)
		{
			static filter_t memory_l, memory_r;
			S_LowpassFilter((int *)paintbuffer,       2, end - paintedtime, &memory_l);
			S_LowpassFilter(((int *)paintbuffer) + 1, 2, end - paintedtime, &memory_r);
		}

		S_UnderwaterFilter (end - paintedtime);
		S_UpdateLevels (end - paintedtime);

	// paint in the music
		if (s_rawend >= paintedtime)
		{	// copy from the streaming sound source
			int		s;
			int		stop;
			float		music_bus_volume;

			stop = (end < s_rawend) ? end : s_rawend;
			music_bus_volume = S_GetBusVolume (SOUND_BUS_MUSIC);

			for (i = paintedtime; i < stop; i++)
			{
				s = i & (MAX_RAW_SAMPLES - 1);
			// lower music by 6db to match sfx
				paintbuffer[i - paintedtime].left += (int) lrintf ((s_rawsamples[s].left / 2.0f) * music_bus_volume);
				paintbuffer[i - paintedtime].right += (int) lrintf ((s_rawsamples[s].right / 2.0f) * music_bus_volume);
			}
			//	if (i != end)
			//		Con_Printf ("partial stream\n");
			//	else
			//		Con_Printf ("full stream\n");
		}

		S_ApplyReverb (end - paintedtime);

	// transfer out according to DMA format
		S_TransferPaintBuffer(end);
		paintedtime = end;
	}
}

void SND_InitScaletable (void)
{
	int		i, j;
	int		scale;

	for (i = 0; i < 32; i++)
	{
		scale = i * 8 * 256 * sfxvolume.value;
		for (j = 0; j < 256; j++)
		{
		/* When compiling with gcc-4.1.0 at optimisations O1 and
		   higher, the tricky signed char type conversion is not
		   guaranteed. Therefore we explicity calculate the signed
		   value from the index as required. From Kevin Shanahan.
		   See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=26719
		*/
		//	snd_scaletable[i][j] = ((signed char)j) * scale;
			snd_scaletable[i][j] = ((j < 128) ?  j : j - 256) * scale;
		}
	}
}


static int SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int count, int paintbufferstart)
{
	float	data;
	int leftscale, rightscale;
	float bus_volume;
	int		i;

	if (ch->leftvol > 255)
		ch->leftvol = 255;
	if (ch->rightvol > 255)
		ch->rightvol = 255;

	leftscale = ch->leftvol * snd_vol;
	rightscale = ch->rightvol * snd_vol;
	leftscale /= 256;
	rightscale /= 256;
	bus_volume = S_GetBusVolume (ch->bus_id);
	leftscale = (int) lrintf (leftscale * bus_volume);
	rightscale = (int) lrintf (rightscale * bus_volume);

	for (i = 0; i < count; i++)
	{
		int sample_index = (int) ch->pos;
		float sample_left;
		float sample_right;

		if (sample_index >= sc->length)
			break;

		data = ((signed char *)sc->data)[sample_index];
		ch->lowpass_history += ch->lowpass_alpha * (data - ch->lowpass_history);
		data = ch->lowpass_history;
		sample_left = data * leftscale;
		sample_right = data * rightscale;
		paintbuffer[paintbufferstart + i].left += (int) lrintf (sample_left);
		paintbuffer[paintbufferstart + i].right += (int) lrintf (sample_right);
		S_ReverbAccumulateSample (paintbufferstart + i, sample_left, sample_right, ch->reverb_send);
		ch->pos += ch->step;
	}

	return i;
}

static int SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int count, int paintbufferstart)
{
	float	data;
	int	left, right;
	int	leftvol, rightvol;
	float bus_volume;
	int	i;

	leftvol = ch->leftvol * snd_vol;
	rightvol = ch->rightvol * snd_vol;
	leftvol /= 256;
	rightvol /= 256;
	bus_volume = S_GetBusVolume (ch->bus_id);
	leftvol = (int) lrintf (leftvol * bus_volume);
	rightvol = (int) lrintf (rightvol * bus_volume);

	for (i = 0; i < count; i++)
	{
		int sample_index = (int) ch->pos;
		float sample_left;
		float sample_right;

		if (sample_index >= sc->length)
			break;

		data = ((signed short *)sc->data)[sample_index];
		ch->lowpass_history += ch->lowpass_alpha * (data - ch->lowpass_history);
		data = ch->lowpass_history;
	// this was causing integer overflow as observed in quakespasm
	// with the warpspasm mod moved >>8 to left/right volume above.
	//	left = (data * leftvol) >> 8;
	//	right = (data * rightvol) >> 8;
		left = (int) lrintf (data * leftvol);
		right = (int) lrintf (data * rightvol);
		paintbuffer[paintbufferstart + i].left += left;
		paintbuffer[paintbufferstart + i].right += right;
		sample_left = (float) left;
		sample_right = (float) right;
		S_ReverbAccumulateSample (paintbufferstart + i, sample_left, sample_right, ch->reverb_send);
		ch->pos += ch->step;
	}

	return i;
}
