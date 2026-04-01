/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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

// snd_dma.c -- main control for any streaming sound output device

#include "quakedef.h"
#include "snd_codec.h"
#include "bgmusic.h"
#include "sounddef.h"

static void S_Play (void);
static void S_PlayVol (void);
static void S_PlayDef_f (void);
static void S_SoundList (void);
static void S_ListActiveVoices_f (void);
static void S_ListDefUsage_f (void);
static void S_ListLegacyMappings_f (void);
static audio_voice_handle_t S_TryPlayLegacySoundDef (const char *def_name, const audio_play_params_t *params);
static void S_Update_ (void);
static float SND_ClampPlaybackStep (float pitch);
static const char *S_LegacySoundDefNameForSample (const char *name);
static int SND_ChannelSamplesUntilEnd (const channel_t *ch, const sfxcache_t *sc);
static int SND_CalcChannelEndTime (int starttime, const channel_t *ch, const sfxcache_t *sc);
static qboolean S_InitAmbientChannel (channel_t *chan, sfx_t *sfx);
static audio_voice_handle_t SND_PlaySfxInternal (sfx_t *sfx, const audio_play_params_t *params, int def_id, int def_instance_id);
void S_StopAllSounds (qboolean clear);
static void S_StopAllSoundsC (void);

void S_SetUnderwaterIntensity (float intensity);

// =======================================================================
// Internal sound data & structures
// =======================================================================

channel_t	snd_channels[MAX_CHANNELS];
int		total_channels;

static int	snd_blocked = 0;
static qboolean	snd_initialized = false;

static dma_t	sn;
volatile dma_t	*shm = NULL;

vec3_t		listener_origin;
vec3_t		listener_forward;
vec3_t		listener_right;
vec3_t		listener_up;

#define	sound_nominal_clip_dist	1000.0

int		soundtime;	// sample PAIRS
int		paintedtime;	// sample PAIRS

int		s_rawend;
portable_samplepair_t	s_rawsamples[MAX_RAW_SAMPLES];


#define	MAX_SFX		1024
static sfx_t	*known_sfx = NULL;	// hunk allocated [MAX_SFX]
static int	num_sfx;

static sfx_t	*ambient_sfx[NUM_AMBIENTS];

static qboolean	sound_started = false;

cvar_t		bgmvolume = {"bgmvolume", "1", CVAR_ARCHIVE};
cvar_t		sfxvolume = {"volume", "0.7", CVAR_ARCHIVE};

cvar_t		precache = {"precache", "1", CVAR_NONE};
cvar_t		loadas8bit = {"loadas8bit", "0", CVAR_NONE};

cvar_t		sndspeed = {"sndspeed", "11025", CVAR_NONE};
cvar_t		snd_mixspeed = {"snd_mixspeed", "44100", CVAR_NONE};

cvar_t		snd_waterfx = {"snd_waterfx", "1", CVAR_ARCHIVE};

cvar_t		snd_filterquality = {"snd_filterquality", "5", CVAR_ARCHIVE};

static	cvar_t	nosound = {"nosound", "0", CVAR_NONE};
static	cvar_t	ambient_level = {"ambient_level", "0.3", CVAR_NONE};
static	cvar_t	ambient_fade = {"ambient_fade", "100", CVAR_NONE};
static	cvar_t	snd_noextraupdate = {"snd_noextraupdate", "0", CVAR_NONE};
static	cvar_t	snd_show = {"snd_show", "0", CVAR_NONE};
static	cvar_t	_snd_mixahead = {"_snd_mixahead", "0.1", CVAR_ARCHIVE};
static	cvar_t	snd_debug = {"snd_debug", "0", CVAR_NONE};
static	cvar_t	snd_bus_sfxvolume = {"snd_bus_sfxvolume", "1", CVAR_ARCHIVE};
static	cvar_t	snd_bus_uivolume = {"snd_bus_uivolume", "1", CVAR_ARCHIVE};
static	cvar_t	snd_bus_ambientvolume = {"snd_bus_ambientvolume", "1", CVAR_ARCHIVE};
static	cvar_t	snd_bus_musicvolume = {"snd_bus_musicvolume", "1", CVAR_ARCHIVE};
static	cvar_t	snd_reverbvolume = {"snd_reverbvolume", "0.2", CVAR_ARCHIVE};

static struct
{
	unsigned int dropped_voices;
	double last_mix_time_ms;
} snd_metrics;

static int snd_next_voice_id = 1;
static int snd_next_def_instance_id = 1;
static vec3_t snd_listener_velocity;

typedef struct legacy_sounddef_map_s
{
	const char *sample_name;
	const char *def_name;
} legacy_sounddef_map_t;

static const legacy_sounddef_map_t snd_legacy_sounddef_maps[] =
{
	{"misc/menu1.wav", "ui/menu_move"},
	{"misc/menu2.wav", "ui/menu_accept"},
	{"misc/menu3.wav", "ui/menu_back"},
	{"misc/talk.wav", "ui/talk"},
	{"weapons/tink1.wav", "weapons/bullet_tink"},
	{"weapons/ric1.wav", "weapons/ricochet"},
	{"weapons/ric2.wav", "weapons/ricochet"},
	{"weapons/ric3.wav", "weapons/ricochet"},
	{"weapons/r_exp3.wav", "weapons/rocket_explode"},
	{"wizard/hit.wav", "monsters/wizard_hit"},
	{"hknight/hit.wav", "monsters/hellknight_hit"}
};


static void S_SoundInfo_f (void)
{
	audio_debug_stats_t stats;

	if (!sound_started || !shm)
	{
		Con_Printf ("sound system not started\n");
		return;
	}

	Con_Printf("%d bit, %s, %d Hz\n", shm->samplebits,
			(shm->channels == 2) ? "stereo" : "mono", shm->speed);
	Con_Printf("%5d samples\n", shm->samples);
	Con_Printf("%5d samplepos\n", shm->samplepos);
	Con_Printf("%5d submission_chunk\n", shm->submission_chunk);
	Con_Printf("%5d total_channels\n", total_channels);
	Con_Printf("%p dma buffer\n", shm->buffer);
	Audio_GetDebugStats (&stats);
	Con_Printf("%5d active_voices\n", stats.active_voices);
	Con_Printf("%5d looped_voices\n", stats.active_looped_voices);
	Con_Printf("%5d static_voices\n", stats.active_static_voices);
	Con_Printf("%5u dropped_voices\n", stats.dropped_voices);
	Con_Printf("%8.3f last_mix_ms\n", stats.last_mix_time_ms);
	if (snd_debug.value > 0.f)
	{
		int bus_counts[SOUND_BUS_COUNT] = {0};
		int i;

		for (i = 0; i < total_channels; i++)
		{
			const channel_t *ch = &snd_channels[i];

			if (!ch->sfx || ch->bus_id < 0 || ch->bus_id >= SOUND_BUS_COUNT)
				continue;
			bus_counts[ch->bus_id]++;
		}

		for (i = 0; i < SOUND_BUS_COUNT; i++)
		{
			Con_Printf("%5d bus_%s\n", bus_counts[i], SoundDef_BusName ((sound_bus_id_t) i));
			Con_Printf("%8.3f bus_%s_volume\n", S_GetBusVolume (i), SoundDef_BusName ((sound_bus_id_t) i));
		}
		Con_Printf("%8.3f reverb_volume\n", S_GetReverbVolume ());
	}
}

static void S_ListActiveVoices_f (void)
{
	int i;
	int total = 0;

	for (i = 0; i < total_channels; i++)
	{
		const channel_t *ch = &snd_channels[i];
		const sound_def_t *def;
		const char *def_name;
		const char *sample_name;
		const char *bus_name;

		if (!ch->sfx)
			continue;

		def = SoundDef_GetById (ch->def_id);
		def_name = def ? def->name : (ch->def_id > 0 ? "<stale>" : "<raw>");
		sample_name = ch->sfx->name[0] ? ch->sfx->name : "<unknown>";
		bus_name = SoundDef_BusName ((sound_bus_id_t) ch->bus_id);

		Con_Printf ("voice %4d ch %3d bus %-7s prio %3d gain %3d pitch %.3f loop %d wet %.2f delay %d %s%s | %s | %s\n",
			ch->voice_id, i, bus_name, ch->priority, ch->master_vol, ch->step, ch->looping >= 0,
			ch->reverb_send, q_max (0, ch->start - paintedtime),
			ch->doppler ? "doppler " : "", ch->lowpass_by_distance ? "lowpass" : "",
			def_name, sample_name);
		total++;
	}

	if (!total)
		Con_Printf ("No active voices\n");
}

static void S_ListDefUsage_f (void)
{
	int voice_counts[SOUNDDEF_MAX_DEFS];
	int instance_counts[SOUNDDEF_MAX_DEFS];
	int raw_voices = 0;
	int i;
	int total_defs = 0;

	memset (voice_counts, 0, sizeof (voice_counts));
	memset (instance_counts, 0, sizeof (instance_counts));

	for (i = 0; i < total_channels; i++)
	{
		const channel_t *ch = &snd_channels[i];
		int def_index;
		int j;
		qboolean seen = false;

		if (!ch->sfx)
			continue;

		if (ch->def_id <= 0 || ch->def_id > SOUNDDEF_MAX_DEFS)
		{
			raw_voices++;
			continue;
		}

		def_index = ch->def_id - 1;
		voice_counts[def_index]++;

		if (ch->def_instance_id <= 0)
			continue;

		for (j = 0; j < i; j++)
		{
			const channel_t *prev = &snd_channels[j];

			if (!prev->sfx)
				continue;
			if (prev->def_id == ch->def_id && prev->def_instance_id == ch->def_instance_id)
			{
				seen = true;
				break;
			}
		}

		if (!seen)
			instance_counts[def_index]++;
	}

	if (raw_voices > 0)
		Con_Printf ("raw voices %d\n", raw_voices);

	for (i = 0; i < SOUNDDEF_MAX_DEFS; i++)
	{
		const sound_def_t *def;

		if (!voice_counts[i])
			continue;

		def = SoundDef_GetById (i + 1);
		Con_Printf ("def %-32s voices %3d instances %3d\n",
			def ? def->name : "<stale>", voice_counts[i], instance_counts[i]);
		total_defs++;
	}

	if (!total_defs && !raw_voices)
		Con_Printf ("No active sound defs\n");
}

static void S_ListLegacyMappings_f (void)
{
	size_t i;

	Con_Printf ("Legacy sound mappings: %d\n", (int) countof (snd_legacy_sounddef_maps));
	for (i = 0; i < countof (snd_legacy_sounddef_maps); i++)
	{
		const legacy_sounddef_map_t *map = &snd_legacy_sounddef_maps[i];
		const sound_def_t *def = SoundDef_Find (map->def_name);

		Con_Printf (" %-24s -> %-28s [%s]\n",
			map->sample_name, map->def_name, def ? "present" : "missing");
	}
}

static audio_voice_handle_t S_TryPlayLegacySoundDef (const char *def_name, const audio_play_params_t *params)
{
	const sound_def_t *def;

	if (!def_name)
		return 0;

	def = SoundDef_Find (def_name);
	if (!def)
		return 0;

	return Audio_PlayDefById (def->id, params);
}


static void SND_Callback_sfxvolume (cvar_t *var)
{
	SND_InitScaletable ();
}

static void SND_Callback_snd_filterquality (cvar_t *var)
{
	if (snd_filterquality.value < 1 || snd_filterquality.value > 5)
	{
		Con_Printf ("snd_filterquality must be between 1 and 5\n");
		Cvar_SetQuick (&snd_filterquality, snd_filterquality.default_string);
	}
}

/*
================
S_Startup
================
*/
void S_Startup (void)
{
	if (!snd_initialized)
		return;

	sound_started = SNDDMA_Init(&sn);

	if (!sound_started)
	{
		Con_Printf("Failed initializing sound\n");
	}
	else
	{
		Con_Printf("Audio: %d bit, %s, %d Hz\n", shm->samplebits,
				(shm->channels == 2) ? "stereo" : "mono", shm->speed);
	}
}

float S_GetBusVolume (int bus_id)
{
	switch ((sound_bus_id_t) bus_id)
	{
	case SOUND_BUS_SFX:
		return CLAMP (0.f, snd_bus_sfxvolume.value, 4.f);
	case SOUND_BUS_UI:
		return CLAMP (0.f, snd_bus_uivolume.value, 4.f);
	case SOUND_BUS_AMBIENT:
		return CLAMP (0.f, snd_bus_ambientvolume.value, 4.f);
	case SOUND_BUS_MUSIC:
		return CLAMP (0.f, snd_bus_musicvolume.value, 4.f);
	default:
		return 1.f;
	}
}

float S_GetReverbVolume (void)
{
	return CLAMP (0.f, snd_reverbvolume.value, 4.f);
}


/*
================
S_Init
================
*/
void S_Init (void)
{
	int i;

	if (snd_initialized)
	{
		Con_Printf("Sound is already initialized\n");
		return;
	}

	Cvar_RegisterVariable(&nosound);
	Cvar_RegisterVariable(&sfxvolume);
	Cvar_RegisterVariable(&precache);
	Cvar_RegisterVariable(&loadas8bit);
	Cvar_RegisterVariable(&bgmvolume);
	Cvar_RegisterVariable(&ambient_level);
	Cvar_RegisterVariable(&ambient_fade);
	Cvar_RegisterVariable(&snd_noextraupdate);
	Cvar_RegisterVariable(&snd_show);
	Cvar_RegisterVariable(&_snd_mixahead);
	Cvar_RegisterVariable(&snd_debug);
	Cvar_RegisterVariable(&snd_bus_sfxvolume);
	Cvar_RegisterVariable(&snd_bus_uivolume);
	Cvar_RegisterVariable(&snd_bus_ambientvolume);
	Cvar_RegisterVariable(&snd_bus_musicvolume);
	Cvar_RegisterVariable(&snd_reverbvolume);
	Cvar_RegisterVariable(&sndspeed);
	Cvar_RegisterVariable(&snd_mixspeed);
	Cvar_RegisterVariable(&snd_filterquality);
	Cvar_RegisterVariable(&snd_waterfx);

	if (safemode || COM_CheckParm("-nosound"))
		return;

	Con_Printf("\nSound Initialization\n");

	Cmd_AddCommand("play", S_Play);
	Cmd_AddCommand("playvol", S_PlayVol);
	Cmd_AddCommand("snd_play_def", S_PlayDef_f);
	Cmd_AddCommand("stopsound", S_StopAllSoundsC);
	Cmd_AddCommand("soundlist", S_SoundList);
	Cmd_AddCommand("soundinfo", S_SoundInfo_f);
	Cmd_AddCommand("snd_list_active", S_ListActiveVoices_f);
	Cmd_AddCommand("snd_list_def_usage", S_ListDefUsage_f);
	Cmd_AddCommand("snd_list_legacy_mappings", S_ListLegacyMappings_f);

	i = COM_CheckParm("-sndspeed");
	if (i && i < com_argc-1)
	{
		Cvar_SetQuick (&sndspeed, com_argv[i + 1]);
	}

	i = COM_CheckParm("-mixspeed");
	if (i && i < com_argc-1)
	{
		Cvar_SetQuick (&snd_mixspeed, com_argv[i + 1]);
	}

	if (host_parms->memsize < 0x800000)
	{
		Cvar_SetQuick (&loadas8bit, "1");
		Con_Printf ("loading all sounds as 8bit\n");
	}

	Cvar_SetCallback(&sfxvolume, SND_Callback_sfxvolume);
	Cvar_SetCallback(&snd_filterquality, &SND_Callback_snd_filterquality);

	SND_InitScaletable ();

	known_sfx = (sfx_t *) Hunk_AllocName (MAX_SFX*sizeof(sfx_t), "sfx_t");
	num_sfx = 0;

	S_InitWavinfoMutex ();

	snd_initialized = true;

	S_Startup ();
	if (sound_started == 0)
		return;

// provides a tick sound until washed clean
//	if (shm->buffer)
//		shm->buffer[4] = shm->buffer[5] = 0x7f;	// force a pop for debugging

	ambient_sfx[AMBIENT_WATER] = S_PrecacheSound ("ambience/water1.wav");
	ambient_sfx[AMBIENT_SKY] = S_PrecacheSound ("ambience/wind2.wav");

	S_CodecInit ();
	SoundDef_Init ();
	SoundDef_LoadAll ();

	S_StopAllSounds (true);
}


// =======================================================================
// Shutdown sound engine
// =======================================================================
void S_Shutdown (void)
{
	if (!sound_started)
		return;

	sound_started = 0;
	snd_blocked = 0;

	SoundDef_Shutdown ();
	S_CodecShutdown();
	S_ShutdownWavinfoMutex ();

	SNDDMA_Shutdown();
	shm = NULL;
	memset (&snd_metrics, 0, sizeof (snd_metrics));
	snd_next_voice_id = 1;
	snd_next_def_instance_id = 1;
	VectorCopy (vec3_origin, snd_listener_velocity);
}


// =======================================================================
// Load a sound
// =======================================================================

/*
==================
S_FindName

==================
*/
static sfx_t *S_FindName (const char *name)
{
	int		i;
	sfx_t	*sfx;

	if (!name)
		Sys_Error ("S_FindName: NULL");

	if (strlen(name) >= MAX_QPATH)
		Sys_Error ("Sound name too long: %s", name);

// see if already loaded
	for (i = 0; i < num_sfx; i++)
	{
		if (!strcmp(known_sfx[i].name, name))
		{
			return &known_sfx[i];
		}
	}

	if (num_sfx == MAX_SFX)
		Sys_Error ("S_FindName: out of sfx_t");

	sfx = &known_sfx[i];
	q_strlcpy (sfx->name, name, sizeof(sfx->name));

	num_sfx++;

	return sfx;
}


/*
==================
S_TouchSound

==================
*/
void S_TouchSound (const char *name)
{
	sfx_t	*sfx;

	if (!sound_started)
		return;

	sfx = S_FindName (name);
	Cache_Check (&sfx->cache);
}

/*
==================
S_PrecacheSound

==================
*/
sfx_t *S_PrecacheSound (const char *name)
{
	sfx_t	*sfx;

	if (!sound_started || nosound.value)
		return NULL;

	sfx = S_FindName (name);

// cache it in
	if (precache.value)
		S_LoadSound (sfx);

	return sfx;
}


//=============================================================================

/*
=================
SND_PickChannel

picks a channel based on priorities, empty slots, number of channels
=================
*/
channel_t *SND_PickChannel (int entnum, int entchannel, int priority)
{
	int	ch_idx;
	int	first_to_die;
	int	best_priority;
	int	life_left;

// Check for replacement sound, or find the best one to replace
	first_to_die = -1;
	best_priority = 0x7fffffff;
	life_left = 0x7fffffff;
	for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++)
	{
		int candidate_priority;
		int candidate_life;

		if (entchannel != 0		// channel 0 never overrides
			&& snd_channels[ch_idx].entnum == entnum
			&& (snd_channels[ch_idx].entchannel == entchannel || entchannel == -1) )
		{	// always override sound from same entity
			first_to_die = ch_idx;
			break;
		}

		// don't let monster sounds override player sounds
		if (snd_channels[ch_idx].entnum == cl.viewentity && entnum != cl.viewentity && snd_channels[ch_idx].sfx)
			continue;

		if (!snd_channels[ch_idx].sfx)
		{
			first_to_die = ch_idx;
			break;
		}

		candidate_priority = CLAMP (0, snd_channels[ch_idx].priority, 255);
		if (candidate_priority > priority)
			continue;

		candidate_life = snd_channels[ch_idx].end - paintedtime;
		if (candidate_priority < best_priority
			|| (candidate_priority == best_priority && candidate_life < life_left))
		{
			best_priority = candidate_priority;
			life_left = candidate_life;
			first_to_die = ch_idx;
		}
	}

	if (first_to_die == -1)
		return NULL;

	return &snd_channels[first_to_die];
}

/*
=================
SND_Spatialize

spatializes a channel
=================
*/
void SND_Spatialize (channel_t *ch)
{
	vec_t	dot;
	vec_t	dist;
	vec_t	lscale, rscale, scale;
	vec3_t	source_vec;

	if (!ch->spatialize)
	{
		ch->leftvol = ch->master_vol;
		ch->rightvol = ch->master_vol;
		return;
	}

// anything coming from the view entity will always be full volume
	if (ch->entnum == cl.viewentity)
	{
		ch->leftvol = ch->master_vol;
		ch->rightvol = ch->master_vol;
		return;
	}

// calculate stereo seperation and distance attenuation
	VectorSubtract(ch->origin, listener_origin, source_vec);
	dist = VectorNormalize(source_vec) * ch->dist_mult;
	dot = DotProduct(listener_right, source_vec);

	if (shm->channels == 1)
	{
		rscale = 1.0;
		lscale = 1.0;
	}
	else
	{
		rscale = 1.0 + dot;
		lscale = 1.0 - dot;
	}

// add in distance effect
	scale = (1.0 - dist) * rscale;
	ch->rightvol = (int) (ch->master_vol * scale);
	if (ch->rightvol < 0)
		ch->rightvol = 0;

	scale = (1.0 - dist) * lscale;
	ch->leftvol = (int) (ch->master_vol * scale);
	if (ch->leftvol < 0)
		ch->leftvol = 0;
}

static float SND_RandomRange (float min_value, float max_value)
{
	if (max_value <= min_value)
		return min_value;

	return min_value + ((float) rand () / (float) RAND_MAX) * (max_value - min_value);
}

static int SND_AllocDefInstanceId (void)
{
	int instance_id = snd_next_def_instance_id++;

	if (snd_next_def_instance_id <= 0)
		snd_next_def_instance_id = 1;

	return instance_id;
}

static int SND_CountActiveDefInstances (int def_id)
{
	int i, j;
	int instances = 0;

	if (def_id <= 0)
		return 0;

	for (i = 0; i < total_channels; i++)
	{
		const channel_t *ch = &snd_channels[i];
		qboolean seen = false;

		if (!ch->sfx || ch->def_id != def_id || ch->def_instance_id <= 0)
			continue;

		for (j = 0; j < i; j++)
		{
			const channel_t *prev = &snd_channels[j];
			if (prev->sfx && prev->def_id == def_id && prev->def_instance_id == ch->def_instance_id)
			{
				seen = true;
				break;
			}
		}

		if (!seen)
			instances++;
	}

	return instances;
}

static float SND_ApplyDopplerStep (const channel_t *ch)
{
	vec3_t source_vec;
	float dist;
	float source_velocity;
	float listener_velocity;
	float factor;
	const float speed_of_sound = 34300.f;

	if (!ch->doppler || !ch->spatialize)
		return ch->base_step > 0.f ? ch->base_step : ch->step;

	VectorSubtract (ch->origin, listener_origin, source_vec);
	dist = VectorNormalize (source_vec);
	if (dist <= 0.f)
		return ch->base_step > 0.f ? ch->base_step : ch->step;

	source_velocity = DotProduct (ch->velocity, source_vec);
	listener_velocity = DotProduct (snd_listener_velocity, source_vec);
	factor = (speed_of_sound + listener_velocity) / (speed_of_sound + source_velocity);
	factor = CLAMP (0.5f, factor, 2.f);

	return SND_ClampPlaybackStep ((ch->base_step > 0.f ? ch->base_step : ch->step) * factor);
}

static float SND_ClampPlaybackStep (float pitch)
{
	if (pitch <= 0.f)
		return 1.f;

	return CLAMP (0.125f, pitch, 8.f);
}

static int SND_ResolvePriority (const audio_play_params_t *params, int default_priority)
{
	if (!params || params->priority <= 0)
		return CLAMP (0, default_priority, 255);

	return CLAMP (0, params->priority, 255);
}

static int SND_ChannelSamplesUntilEnd (const channel_t *ch, const sfxcache_t *sc)
{
	float remaining;

	if (!sc || ch->step <= 0.f)
		return 0;

	remaining = (float) sc->length - ch->pos;
	if (remaining <= 0.f)
		return 0;

	return q_max (1, (int) ceilf (remaining / ch->step));
}

static int SND_CalcChannelEndTime (int starttime, const channel_t *ch, const sfxcache_t *sc)
{
	return starttime + SND_ChannelSamplesUntilEnd (ch, sc);
}

static float SND_StartOffsetToSamplePos (const channel_t *ch, const sfxcache_t *sc, int start_offset_ms)
{
	int offset_samples;

	if (!sc || start_offset_ms <= 0)
		return 0.f;

	offset_samples = (int) ((double) sc->speed * (double) start_offset_ms / 1000.0);
	if (offset_samples <= 0)
		return 0.f;

	if (ch->looping >= 0 && sc->length > 0)
		return (float) (offset_samples % sc->length);

	if (offset_samples >= sc->length)
		return (float) sc->length;

	return (float) offset_samples;
}


// =======================================================================
// Start a sound effect
// =======================================================================

static audio_voice_handle_t SND_PlaySfxInternal (sfx_t *sfx, const audio_play_params_t *params, int def_id, int def_instance_id)
{
	channel_t	*target_chan, *check;
	sfxcache_t	*sc;
	sfx_t		*old_sfx;
	vec3_t		origin;
	vec3_t		old_origin;
	float		gain, pitch, attenuation;
	float		old_vol;
	float		old_atten;
	int		priority;
	int		entnum, entchannel;
	int		ch_idx;
	int		skip;

	if (!sound_started || !sfx || nosound.value)
		return 0;

	entnum = params ? params->entnum : 0;
	entchannel = params ? params->entchannel : 0;
	gain = params ? params->gain : 1.f;
	pitch = params ? params->pitch : 1.f;
	attenuation = params ? params->attenuation : 1.f;
	priority = SND_ResolvePriority (params, 128);
	if (params)
		VectorCopy (params->origin, origin);
	else
		VectorCopy (vec3_origin, origin);

// pick a channel to play on
	target_chan = SND_PickChannel(entnum, entchannel, priority);
	if (!target_chan)
	{
		snd_metrics.dropped_voices++;
		return 0;
	}

// keep track of the old sound playing on this channel (for demo rewinding)
	old_sfx = NULL;
	VectorCopy (origin, old_origin);
	old_vol = gain;
	old_atten = attenuation;
	if (entnum > 0 && entchannel > 0 && target_chan->entnum == entnum && target_chan->entchannel == entchannel)
	{
		old_sfx = target_chan->sfx;
		VectorCopy (target_chan->origin, old_origin);
		old_vol = target_chan->master_vol / 255.f;
		old_atten = target_chan->dist_mult * sound_nominal_clip_dist;
	}

// spatialize
	memset (target_chan, 0, sizeof(*target_chan));
	VectorCopy(origin, target_chan->origin);
	target_chan->dist_mult = attenuation / sound_nominal_clip_dist;
	target_chan->master_vol = (int) (CLAMP (0.f, gain, 4.f) * 255.f);
	target_chan->priority = priority;
	target_chan->entnum = entnum;
	target_chan->entchannel = entchannel;
	target_chan->start = paintedtime;
	if (params)
		VectorCopy (params->velocity, target_chan->velocity);
	else
		VectorCopy (vec3_origin, target_chan->velocity);
	target_chan->base_step = SND_ClampPlaybackStep (pitch);
	target_chan->step = target_chan->base_step;
	target_chan->spatialize = params ? !params->no_spatialize : true;
	target_chan->doppler = params ? params->doppler : false;
	target_chan->lowpass_by_distance = params ? params->lowpass_by_distance : false;
	target_chan->reverb_send = params ? CLAMP (0.f, params->reverb_send, 1.f) : 0.f;
	target_chan->lowpass_alpha = 1.f;
	target_chan->lowpass_history = 0.f;
	target_chan->bus_id = params ? params->bus_id : SOUND_BUS_SFX;
	if (target_chan->bus_id < 0 || target_chan->bus_id >= SOUND_BUS_COUNT)
		target_chan->bus_id = SOUND_BUS_SFX;
	target_chan->voice_id = snd_next_voice_id++;
	target_chan->def_id = def_id;
	target_chan->def_instance_id = def_instance_id;
	if (snd_next_voice_id <= 0)
		snd_next_voice_id = 1;
	SND_Spatialize(target_chan);

	if (!target_chan->leftvol && !target_chan->rightvol)
		return 0;		// not audible at all

// new channel
	sc = S_LoadSound (sfx);
	if (!sc)
	{
		target_chan->sfx = NULL;
		target_chan->voice_id = 0;
		return 0;		// couldn't load the sound's data
	}

	// if this is a looping sound and we're not rewinding, keep track of the previous sound playing
// on the same ent/channel so that when we do rewind past this frame we start playing it instead
	if (cls.demoplayback && cls.demospeed > 0.f && (sc->loopstart != -1 || (params && params->loop)))
		CL_AddDemoRewindSound (entnum, entchannel, old_sfx, old_origin, old_vol, old_atten);

	target_chan->sfx = sfx;
	target_chan->looping = sc->loopstart;
	if (params && params->loop && target_chan->looping < 0)
		target_chan->looping = 0;
	target_chan->pos = SND_StartOffsetToSamplePos (target_chan, sc, params ? params->start_offset_ms : 0);
	if (target_chan->pos >= sc->length)
	{
		target_chan->sfx = NULL;
		target_chan->voice_id = 0;
		return 0;
	}
	if (params && params->delay_ms > 0)
		target_chan->start += (int) ((double) shm->speed * (double) params->delay_ms / 1000.0);
	target_chan->end = SND_CalcChannelEndTime (target_chan->start, target_chan, sc);

// if an identical sound has also been started this frame, offset the pos
// a bit to keep it from just making the first one louder
	check = &snd_channels[NUM_AMBIENTS];
	for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++, check++)
	{
		if (check == target_chan)
			continue;
		if (check->sfx == sfx && check->pos == 0.f)
		{
			skip = 0.1f * shm->speed;
			if (skip > sc->length)
				skip = sc->length;
			if (skip > 0)
				skip = rand() % skip;
			target_chan->pos += (float) skip;
			// Recalculate from the channel's actual start so delayed voices keep their delay.
			target_chan->end = SND_CalcChannelEndTime (target_chan->start, target_chan, sc);
			break;
		}
	}

	return target_chan->voice_id;
}

audio_voice_handle_t Audio_PlayRawSfx (sfx_t *sfx, const audio_play_params_t *params)
{
	return SND_PlaySfxInternal (sfx, params, 0, 0);
}

audio_voice_handle_t Audio_PlayRaw (const char *name, const audio_play_params_t *params)
{
	sfx_t *sfx;

	if (!name)
		return 0;

	sfx = S_PrecacheSound (name);
	if (!sfx)
		return 0;

	return Audio_PlayRawSfx (sfx, params);
}

audio_voice_handle_t Audio_PlayDefById (sound_def_id_t id, const audio_play_params_t *params)
{
	const sound_def_t *def;
	float caller_gain = 1.f;
	float caller_pitch = 1.f;
	float attenuation = 1.f;
	int instance_id;
	audio_voice_handle_t first_handle = 0;
	int layer_index;

	def = SoundDef_GetById (id);
	if (!def)
		return 0;

	if (def->max_instances > 0 && SND_CountActiveDefInstances (def->id) >= def->max_instances)
	{
		snd_metrics.dropped_voices++;
		if (snd_debug.value > 0.f)
			Con_DWarning ("sound def '%s' hit max_instances %d\n", def->name, def->max_instances);
		return 0;
	}

	if (params)
	{
		if (params->gain > 0.f)
			caller_gain = params->gain;
		if (params->pitch > 0.f)
			caller_pitch = params->pitch;
		if (params->attenuation > 0.f)
			attenuation = params->attenuation;
	}

	instance_id = SND_AllocDefInstanceId ();

	for (layer_index = 0; layer_index < def->layer_count; layer_index++)
	{
		const sound_def_layer_t *layer = &def->layers[layer_index];
		audio_play_params_t layer_params;
		sfx_t *sample_sfx;
		int sample_index;
		audio_voice_handle_t handle;

		if (layer->sample_count <= 0)
			continue;

		/* Edge-case intent: chance <= 0 never plays, chance >= 1 always plays. */
		if (layer->chance <= 0.f)
			continue;
		if (layer->chance < 1.f)
		{
			const float chance_roll = (float) rand () / ((float) RAND_MAX + 1.f); /* [0, 1) */

			if (!(chance_roll < layer->chance))
				continue;
		}

		sample_index = (layer->sample_count == 1) ? 0 : rand () % layer->sample_count;
		sample_sfx = layer->samples[sample_index].sfx;
		if (!sample_sfx)
			continue;

		memset (&layer_params, 0, sizeof (layer_params));
		if (params)
		{
			layer_params.entnum = params->entnum;
			layer_params.entchannel = params->entchannel;
			VectorCopy (params->origin, layer_params.origin);
			VectorCopy (params->velocity, layer_params.velocity);
			layer_params.no_spatialize = params->no_spatialize;
			layer_params.doppler = params->doppler;
			layer_params.lowpass_by_distance = params->lowpass_by_distance;
			layer_params.reverb_send = params->reverb_send;
			layer_params.bus_id = params->bus_id;
		}
		else
		{
			VectorCopy (vec3_origin, layer_params.origin);
			VectorCopy (vec3_origin, layer_params.velocity);
		}

		layer_params.gain = layer->volume * SND_RandomRange (layer->volume_random_min, layer->volume_random_max) * caller_gain;
		layer_params.pitch = layer->pitch * SND_RandomRange (layer->pitch_random_min, layer->pitch_random_max) * caller_pitch;
		layer_params.attenuation = attenuation;
		layer_params.loop = layer->loop || (params && params->loop);
		layer_params.doppler = def->doppler;
		layer_params.lowpass_by_distance = def->lowpass_by_distance;
		layer_params.reverb_send = def->reverb_send;
		layer_params.bus_id = layer->bus_id;
		layer_params.priority = params && params->priority > 0 ? params->priority : def->priority;
		layer_params.delay_ms = layer->delay_ms;
		layer_params.start_offset_ms = layer->start_offset_ms;
		if (!def->spatialize)
			layer_params.no_spatialize = true;

		handle = SND_PlaySfxInternal (sample_sfx, &layer_params, def->id, instance_id);
		if (!first_handle && handle)
			first_handle = handle;
	}

	return first_handle;
}

audio_voice_handle_t Audio_PlayDef (const char *name, const audio_play_params_t *params)
{
	const sound_def_t *def;

	if (!name)
		return 0;

	def = SoundDef_Find (name);
	if (!def)
	{
		Con_Printf ("Audio_PlayDef: unknown sound def '%s'\n", name);
		return 0;
	}

	return Audio_PlayDefById (def->id, params);
}

void Audio_StopVoice (audio_voice_handle_t handle)
{
	int i;

	if (handle <= 0)
		return;

	for (i = 0; i < MAX_CHANNELS; i++)
	{
		if (snd_channels[i].voice_id == handle)
		{
			snd_channels[i].end = 0;
			snd_channels[i].sfx = NULL;
			snd_channels[i].step = 1.f;
			snd_channels[i].base_step = 1.f;
			snd_channels[i].doppler = false;
			snd_channels[i].lowpass_by_distance = false;
			snd_channels[i].lowpass_alpha = 1.f;
			snd_channels[i].lowpass_history = 0.f;
			snd_channels[i].reverb_send = 0.f;
			snd_channels[i].start = 0;
			snd_channels[i].voice_id = 0;
			snd_channels[i].def_id = 0;
			snd_channels[i].def_instance_id = 0;
			return;
		}
	}
}

void Audio_GetDebugStats (audio_debug_stats_t *stats)
{
	int i;

	if (!stats)
		return;

	memset (stats, 0, sizeof (*stats));
	stats->dropped_voices = snd_metrics.dropped_voices;
	stats->last_mix_time_ms = snd_metrics.last_mix_time_ms;

	for (i = 0; i < total_channels; i++)
	{
		const channel_t *ch = &snd_channels[i];
		if (!ch->sfx)
			continue;

		stats->active_voices++;
		if (ch->looping >= 0)
			stats->active_looped_voices++;
		if (i >= MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS)
			stats->active_static_voices++;
	}
}

void S_StartSound (int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol, float attenuation)
{
	audio_play_params_t params;

	memset (&params, 0, sizeof (params));
	params.entnum = entnum;
	params.entchannel = entchannel;
	VectorCopy (origin, params.origin);
	VectorCopy (vec3_origin, params.velocity);
	params.gain = fvol;
	params.pitch = 1.f;
	params.attenuation = attenuation;
	params.loop = false;
	params.no_spatialize = false;
	params.doppler = false;
	params.lowpass_by_distance = false;
	params.reverb_send = 0.f;
	params.bus_id = SOUND_BUS_SFX;
	params.priority = 128;
	params.delay_ms = 0;
	params.start_offset_ms = 0;

	if (sfx)
	{
		const char *def_name = S_LegacySoundDefNameForSample (sfx->name);
		if (S_TryPlayLegacySoundDef (def_name, &params))
			return;
	}

	Audio_PlayRawSfx (sfx, &params);
}

void S_StopSound (int entnum, int entchannel)
{
	int	i;

	for (i = 0; i < MAX_DYNAMIC_CHANNELS; i++)
	{
		if (snd_channels[i].entnum == entnum
			&& snd_channels[i].entchannel == entchannel)
		{
			snd_channels[i].end = 0;
			snd_channels[i].sfx = NULL;
			snd_channels[i].voice_id = 0;
			return;
		}
	}
}

void S_StopAllSounds (qboolean clear)
{
	int		i;

	if (!sound_started)
		return;

	total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;	// no statics

	for (i = 0; i < MAX_CHANNELS; i++)
	{
		if (snd_channels[i].sfx)
			snd_channels[i].sfx = NULL;
	}

	memset(snd_channels, 0, MAX_CHANNELS * sizeof(channel_t));
	snd_next_voice_id = 1;
	snd_next_def_instance_id = 1;

	if (clear)
		S_ClearBuffer ();
}

static void S_StopAllSoundsC (void)
{
	S_StopAllSounds (true);
}

void S_ClearBuffer (void)
{
	int		clear;

	if (!sound_started || !shm)
		return;

	SNDDMA_LockBuffer ();
	if (! shm->buffer)
		return;

	s_rawend = 0;

	if (shm->samplebits == 8 && !shm->signed8)
		clear = 0x80;
	else
		clear = 0;

	memset (shm->buffer, clear, shm->samples * shm->samplebits / 8);
	memset (s_rawsamples, 0, sizeof (s_rawsamples));
	S_ResetReverbState ();

	SNDDMA_Submit ();
}


/*
=================
S_StaticSound
=================
*/
void S_StaticSound (sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
	channel_t	*ss;
	sfxcache_t		*sc;

	if (!sfx)
		return;

	if (total_channels == MAX_CHANNELS)
	{
		Con_Printf ("total_channels == MAX_CHANNELS\n");
		return;
	}

	ss = &snd_channels[total_channels];
	total_channels++;

	sc = S_LoadSound (sfx);
	if (!sc)
		return;

	if (sc->loopstart == -1)
	{
		Con_Printf ("Sound %s not looped\n", sfx->name);
		return;
	}

	ss->sfx = sfx;
	VectorCopy (origin, ss->origin);
	ss->master_vol = (int)vol;
	ss->priority = 255;
	ss->dist_mult = (attenuation / 64) / sound_nominal_clip_dist;
	VectorCopy (vec3_origin, ss->velocity);
	ss->base_step = 1.f;
	ss->step = 1.f;
	ss->spatialize = true;
	ss->doppler = false;
	ss->lowpass_by_distance = false;
	ss->reverb_send = 0.f;
	ss->lowpass_alpha = 1.f;
	ss->lowpass_history = 0.f;
	ss->bus_id = SOUND_BUS_AMBIENT;
	ss->voice_id = snd_next_voice_id++;
	if (snd_next_voice_id <= 0)
		snd_next_voice_id = 1;
	ss->start = paintedtime;
	ss->pos = 0.f;
	ss->looping = sc->loopstart;
	ss->end = SND_CalcChannelEndTime (paintedtime, ss, sc);

	SND_Spatialize (ss);
}


//=============================================================================

/*
===================
S_UnderwaterIntensityForContents
===================
*/
static float S_UnderwaterIntensityForContents (int contents)
{
	switch (contents)
	{
		case CONTENTS_WATER:
		case CONTENTS_SLIME:
		case CONTENTS_LAVA:
			return 1.f;
		default:
			return 0.f;
	}
}

/*
===================
S_UpdateAmbientSounds
===================
*/
static void S_UpdateAmbientSounds (void)
{
	mleaf_t			*l;
	int				ambient_channel;
	channel_t		*chan;
	float			vol, underwater;
	static float	levels[NUM_AMBIENTS];

// no ambients when disconnected
	if (cls.state != ca_connected || !cl.worldmodel)
	{
		memset (levels, 0, sizeof (levels));
		S_SetUnderwaterIntensity (0.f);
		return;
	}

// calc ambient sound levels
	l = Mod_PointInLeaf (listener_origin, cl.worldmodel);
	if (cl.forceunderwater)
		underwater = 1.f;
	else if (l)
		underwater = S_UnderwaterIntensityForContents (l->contents);
	else
		underwater = 0.f;
	S_SetUnderwaterIntensity (underwater);
	if (!l || !ambient_level.value)
	{
		for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++)
			snd_channels[ambient_channel].sfx = NULL;
		memset (levels, 0, sizeof (levels));
		return;
	}

	for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++)
	{
		chan = &snd_channels[ambient_channel];
		if (chan->sfx != ambient_sfx[ambient_channel] || chan->step <= 0.f || chan->base_step <= 0.f)
		{
			if (!S_InitAmbientChannel (chan, ambient_sfx[ambient_channel]))
			{
				memset (chan, 0, sizeof (*chan));
				continue;
			}
		}

		vol = (int) (ambient_level.value * l->ambient_sound_level[ambient_channel]);
		if (vol < 8.f)
			vol = 0.f;
		else if (vol > 255.f)
			vol = 255.f;

	// don't adjust volume too fast
		if (levels[ambient_channel] < vol)
		{
			levels[ambient_channel] += host_frametime * ambient_fade.value;
			if (levels[ambient_channel] > vol)
				levels[ambient_channel] = vol;
		}
		else if (levels[ambient_channel] > vol)
		{
			levels[ambient_channel] -= host_frametime * ambient_fade.value;
			if (levels[ambient_channel] < vol)
				levels[ambient_channel] = vol;
		}

		chan->leftvol = chan->rightvol = chan->master_vol = (int) levels[ambient_channel];
	}
}


/*
===================
S_RawSamples		(from QuakeII)

Streaming music support. Byte swapping
of data must be handled by the codec.
Expects data in signed 16 bit, or unsigned
8 bit format.
===================
*/
void S_RawSamples (int samples, int rate, int width, int channels, byte *data, float volume)
{
	int i;
	int src, dst;
	float scale;
	int intVolume;

	if (s_rawend < paintedtime)
		s_rawend = paintedtime;

	scale = (float) rate / shm->speed;
	intVolume = (int) (256 * volume);

	if (channels == 2 && width == 2)
	{
		for (i = 0; ; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
			s_rawsamples [dst].left = ((short *) data)[src * 2] * intVolume;
			s_rawsamples [dst].right = ((short *) data)[src * 2 + 1] * intVolume;
		}
	}
	else if (channels == 1 && width == 2)
	{
		for (i = 0; ; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
			s_rawsamples [dst].left = ((short *) data)[src] * intVolume;
			s_rawsamples [dst].right = ((short *) data)[src] * intVolume;
		}
	}
	else if (channels == 2 && width == 1)
	{
		intVolume *= 256;

		for (i = 0; ; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
		//	s_rawsamples [dst].left = ((signed char *) data)[src * 2] * intVolume;
		//	s_rawsamples [dst].right = ((signed char *) data)[src * 2 + 1] * intVolume;
			s_rawsamples [dst].left = (((byte *) data)[src * 2] - 128) * intVolume;
			s_rawsamples [dst].right = (((byte *) data)[src * 2 + 1] - 128) * intVolume;
		}
	}
	else if (channels == 1 && width == 1)
	{
		intVolume *= 256;

		for (i = 0; ; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
		//	s_rawsamples [dst].left = ((signed char *) data)[src] * intVolume;
		//	s_rawsamples [dst].right = ((signed char *) data)[src] * intVolume;
			s_rawsamples [dst].left = (((byte *) data)[src] - 128) * intVolume;
			s_rawsamples [dst].right = (((byte *) data)[src] - 128) * intVolume;
		}
	}
}

/*
============
S_Update

Called once each time through the main loop
============
*/
void S_Update (vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
	int			i, j;
	int			total;
	channel_t	*ch;
	channel_t	*combine;

	if (!sound_started || (snd_blocked > 0))
		return;

	VectorCopy(origin, listener_origin);
	VectorCopy(forward, listener_forward);
	VectorCopy(right, listener_right);
	VectorCopy(up, listener_up);
	VectorCopy (cl.velocity, snd_listener_velocity);

// update general area ambient sound sources
	S_UpdateAmbientSounds ();

	combine = NULL;

// update spatialization for static and dynamic sounds
	ch = snd_channels + NUM_AMBIENTS;
	for (i = NUM_AMBIENTS; i < total_channels; i++, ch++)
	{
		vec3_t source_vec;
		float dist;
		float normalized_dist;

		if (!ch->sfx)
			continue;
		SND_Spatialize(ch);	// respatialize channel
		ch->step = SND_ApplyDopplerStep (ch);
		if (ch->lowpass_by_distance && ch->spatialize)
		{
			VectorSubtract (ch->origin, listener_origin, source_vec);
			dist = VectorNormalize (source_vec);
			normalized_dist = CLAMP (0.f, dist * ch->dist_mult, 1.f);
			ch->lowpass_alpha = CLAMP (0.15f, 1.f - normalized_dist * 0.85f, 1.f);
		}
		else
		{
			ch->lowpass_alpha = 1.f;
		}
		if (!ch->leftvol && !ch->rightvol)
			continue;

	// try to combine static sounds with a previous channel of the same
	// sound effect so we don't mix five torches every frame

		if (i >= MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS)
		{
		// see if it can just use the last one
			if (combine && combine->sfx == ch->sfx)
			{
				combine->leftvol += ch->leftvol;
				combine->rightvol += ch->rightvol;
				ch->leftvol = ch->rightvol = 0;
				continue;
			}
		// search for one
			combine = snd_channels + MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;
			for (j = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS; j < i; j++, combine++)
			{
				if (combine->sfx == ch->sfx)
					break;
			}

			if (j == total_channels)
			{
				combine = NULL;
			}
			else
			{
				if (combine != ch)
				{
					combine->leftvol += ch->leftvol;
					combine->rightvol += ch->rightvol;
					ch->leftvol = ch->rightvol = 0;
				}
				continue;
			}
		}
	}

//
// debugging output
//
	if (snd_show.value)
	{
		total = 0;
		ch = snd_channels;
		for (i = 0; i < total_channels; i++, ch++)
		{
			if (ch->sfx && (ch->leftvol || ch->rightvol) )
			{
				sfxcache_t *sc = (sfxcache_t *) Cache_Check (&ch->sfx->cache);
				if (snd_show.value >= 2.f)
					Con_SafePrintf ("L:%3i R:%3i | ENT:%5i CH:%3i | %s%s\n",
						ch->leftvol, ch->rightvol, ch->entnum, ch->entchannel, ch->sfx->name, sc && sc->loopstart >= 0 ? " [L]" : "");
				total++;
			}
		}

		Con_Printf ("----(%i)----\n", total);
	}

// add raw data from streamed samples
//	BGM_Update();	// moved to the main loop just before S_Update ()

// mix some sound
	S_Update_();
}

static void GetSoundtime (void)
{
	int		samplepos;
	static	int		buffers;
	static	int		oldsamplepos;
	int		fullsamples;

	fullsamples = shm->samples / shm->channels;

// it is possible to miscount buffers if it has wrapped twice between
// calls to S_Update.  Oh well.
	samplepos = SNDDMA_GetDMAPos();

	if (samplepos < oldsamplepos)
	{
		buffers++;	// buffer wrapped

		if (paintedtime > 0x40000000)
		{	// time to chop things off to avoid 32 bit limits
			buffers = 0;
			paintedtime = fullsamples;
			S_StopAllSounds (true);
		}
	}
	oldsamplepos = samplepos;

	soundtime = buffers*fullsamples + samplepos/shm->channels;
}

void S_ExtraUpdate (void)
{
	if (snd_noextraupdate.value)
		return;		// don't pollute timings
	S_Update_();
}

static void S_Update_ (void)
{
	unsigned int	endtime;
	int		samps;
	double		mix_start;

	if (!sound_started || (snd_blocked > 0))
		return;

	mix_start = Sys_DoubleTime ();

	SNDDMA_LockBuffer ();
	if (! shm->buffer)
		return;

	// Updates DMA time
	GetSoundtime();

// check to make sure that we haven't overshot
	if (paintedtime < soundtime)
	{
	//	Con_Printf ("S_Update_ : overflow\n");
		paintedtime = soundtime;
	}

// mix ahead of current position
	endtime = soundtime + (unsigned int)(_snd_mixahead.value * shm->speed);
	samps = shm->samples >> (shm->channels - 1);
	endtime = q_min(endtime, (unsigned int)(soundtime + samps));

	S_PaintChannels (endtime);
	snd_metrics.last_mix_time_ms = (Sys_DoubleTime () - mix_start) * 1000.0;

	SNDDMA_Submit ();
}

static qboolean S_InitAmbientChannel (channel_t *chan, sfx_t *sfx)
{
	sfxcache_t *sc;

	if (!chan || !sfx)
		return false;

	sc = S_LoadSound (sfx);
	if (!sc || sc->loopstart < 0)
		return false;

	chan->sfx = sfx;
	chan->start = paintedtime;
	chan->end = SND_CalcChannelEndTime (paintedtime, chan, sc);
	chan->pos = 0.f;
	chan->looping = sc->loopstart;
	chan->base_step = 1.f;
	chan->step = 1.f;
	chan->spatialize = false;
	chan->doppler = false;
	chan->lowpass_by_distance = false;
	chan->reverb_send = 0.f;
	chan->lowpass_alpha = 1.f;
	chan->lowpass_history = 0.f;
	chan->bus_id = SOUND_BUS_AMBIENT;
	if (chan->voice_id <= 0)
	{
		chan->voice_id = snd_next_voice_id++;
		if (snd_next_voice_id <= 0)
			snd_next_voice_id = 1;
	}

	return true;
}

void S_BlockSound (void)
{
/* FIXME: do we really need the blocking at the
 * driver level?
 */
	if (sound_started && snd_blocked == 0)	/* ++snd_blocked == 1 */
	{
		snd_blocked  = 1;
		S_ClearBuffer ();
		if (shm)
			SNDDMA_BlockSound();
	}
}

void S_UnblockSound (void)
{
	if (!sound_started || !snd_blocked)
		return;
	if (snd_blocked == 1)			/* --snd_blocked == 0 */
	{
		snd_blocked  = 0;
		SNDDMA_UnblockSound();
		S_ClearBuffer ();
	}
}

qboolean S_IsSoundBlocked (void)
{
	return snd_blocked > 0;
}


/*
===============================================================================

console functions

===============================================================================
*/

static void S_Play (void)
{
	static int hash = 345;
	int		i;
	char	name[256];
	sfx_t	*sfx;

	i = 1;
	while (i < Cmd_Argc())
	{
		q_strlcpy(name, Cmd_Argv(i), sizeof(name));
		if (!strrchr(Cmd_Argv(i), '.'))
		{
			q_strlcat(name, ".wav", sizeof(name));
		}
		sfx = S_PrecacheSound(name);
		S_StartSound(hash++, 0, sfx, listener_origin, 1.0, 1.0);
		i++;
	}
}

static void S_PlayVol (void)
{
	static int hash = 543;
	int		i;
	float	vol;
	char	name[256];
	sfx_t	*sfx;

	i = 1;
	while (i < Cmd_Argc())
	{
		q_strlcpy(name, Cmd_Argv(i), sizeof(name));
		if (!strrchr(Cmd_Argv(i), '.'))
		{
			q_strlcat(name, ".wav", sizeof(name));
		}
		sfx = S_PrecacheSound(name);
		vol = atof(Cmd_Argv(i + 1));
		S_StartSound(hash++, 0, sfx, listener_origin, vol, 1.0);
		i += 2;
	}
}

static void S_PlayDef_f (void)
{
	audio_play_params_t params;
	audio_voice_handle_t handle;
	float distance = 256.f;
	float speed = 0.f;

	if (Cmd_Argc () < 2 || Cmd_Argc () > 4)
	{
		Con_Printf ("snd_play_def <name> [distance] [speed]\n");
		Con_Printf ("  distance: units in front of listener (default 256)\n");
		Con_Printf ("  speed: source velocity along listener forward, negative approaches (default 0)\n");
		return;
	}

	if (Cmd_Argc () >= 3)
		distance = (float) atof (Cmd_Argv (2));
	if (Cmd_Argc () >= 4)
		speed = (float) atof (Cmd_Argv (3));

	memset (&params, 0, sizeof (params));
	params.entnum = -1;
	params.entchannel = 0;
	VectorMA (listener_origin, distance, listener_forward, params.origin);
	VectorScale (listener_forward, speed, params.velocity);
	params.gain = 1.f;
	params.pitch = 1.f;
	params.attenuation = 1.f;
	params.loop = false;
	params.no_spatialize = false;
	params.doppler = false;
	params.lowpass_by_distance = false;
	params.reverb_send = 0.f;
	params.bus_id = SOUND_BUS_SFX;
	params.priority = 0;
	params.delay_ms = 0;
	params.start_offset_ms = 0;

	handle = Audio_PlayDef (Cmd_Argv (1), &params);
	if (!handle)
		Con_Printf ("snd_play_def: '%s' did not start a voice\n", Cmd_Argv (1));
}

static void S_SoundList (void)
{
	int		i;
	sfx_t	*sfx;
	sfxcache_t	*sc;
	int	size, total;

	total = 0;
	for (sfx = known_sfx, i = 0; i < num_sfx; i++, sfx++)
	{
		sc = (sfxcache_t *) Cache_Check (&sfx->cache);
		if (!sc)
			continue;
		size = sc->length*sc->width*(sc->stereo + 1);
		total += size;
		if (sc->loopstart >= 0)
			Con_SafePrintf ("L"); //johnfitz -- was Con_Printf
		else
			Con_SafePrintf (" "); //johnfitz -- was Con_Printf
		Con_SafePrintf("(%2db) %6i : %s\n", sc->width*8, size, sfx->name); //johnfitz -- was Con_Printf
	}
	Con_Printf ("%i sounds, %i bytes\n", num_sfx, total); //johnfitz -- added count
}


static const char *S_LegacySoundDefNameForSample (const char *name)
{
	size_t i;

	if (!name)
		return NULL;

	for (i = 0; i < countof (snd_legacy_sounddef_maps); i++)
	{
		if (!q_strcasecmp (name, snd_legacy_sounddef_maps[i].sample_name))
			return snd_legacy_sounddef_maps[i].def_name;
	}

	return NULL;
}

void S_LocalSound (const char *name)
{
	sfx_t	*sfx;
	const char *def_name;
	audio_play_params_t params;

	if (nosound.value)
		return;
	if (!sound_started)
		return;

	def_name = S_LegacySoundDefNameForSample (name);
	if (def_name)
	{
		memset (&params, 0, sizeof (params));
		params.entnum = cl.viewentity;
		params.entchannel = -1;
		VectorCopy (vec3_origin, params.origin);
		VectorCopy (vec3_origin, params.velocity);
		params.gain = 1.f;
		params.pitch = 1.f;
		params.attenuation = 1.f;
		params.loop = false;
		params.no_spatialize = true;
		params.doppler = false;
		params.lowpass_by_distance = false;
		params.reverb_send = 0.f;
		params.bus_id = SOUND_BUS_UI;
		params.priority = 192;
		params.delay_ms = 0;
		params.start_offset_ms = 0;

		if (S_TryPlayLegacySoundDef (def_name, &params))
			return;
	}

	sfx = S_PrecacheSound (name);
	if (!sfx)
	{
		Con_Printf ("S_LocalSound: can't cache %s\n", name);
		return;
	}
	S_StartSound (cl.viewentity, -1, sfx, vec3_origin, 1, 1);
}


void S_ClearPrecache (void)
{
}

void S_BeginPrecaching (void)
{
}

void S_EndPrecaching (void)
{
}
