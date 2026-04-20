#ifndef __QUAKE_SOUNDDEF__
#define __QUAKE_SOUNDDEF__

#define SOUNDDEF_MAX_DEFS			256
#define SOUNDDEF_MAX_LAYERS			8
#define SOUNDDEF_MAX_SAMPLES_PER_LAYER	8

typedef int sound_def_id_t;

typedef enum sound_bus_id_e
{
	SOUND_BUS_INVALID = -1,
	SOUND_BUS_SFX = 0,
	SOUND_BUS_UI,
	SOUND_BUS_AMBIENT,
	SOUND_BUS_MUSIC,
	SOUND_BUS_COUNT
} sound_bus_id_t;

typedef struct sound_def_sample_s
{
	char	path[MAX_QPATH];
	sfx_t	*sfx;
} sound_def_sample_t;

typedef struct sound_def_layer_s
{
	int		sample_count;
	sound_def_sample_t samples[SOUNDDEF_MAX_SAMPLES_PER_LAYER];
	sound_bus_id_t	bus_id;
	float		volume;
	float		volume_random_min;
	float		volume_random_max;
	float		pitch;
	float		pitch_random_min;
	float		pitch_random_max;
	qboolean	loop;
	float		chance;
	int		delay_ms;
	int		start_offset_ms;
} sound_def_layer_t;

typedef struct sound_def_s
{
	sound_def_id_t	id;
	char		name[MAX_QPATH];
	char		source_file[MAX_OSPATH];
	int		source_line;
	int		priority;
	int		max_instances;
	qboolean	spatialize;
	qboolean	doppler;
	qboolean	lowpass_by_distance;
	float		reverb_send;
	int		layer_count;
	sound_def_layer_t layers[SOUNDDEF_MAX_LAYERS];
} sound_def_t;

typedef struct sound_def_layer_desc_s
{
	int		sample_count;
	char		samples[SOUNDDEF_MAX_SAMPLES_PER_LAYER][MAX_QPATH];
	sound_bus_id_t	bus_id;
	float		volume;
	float		volume_random_min;
	float		volume_random_max;
	float		pitch;
	float		pitch_random_min;
	float		pitch_random_max;
	qboolean	loop;
	float		chance;
	int		delay_ms;
	int		start_offset_ms;
} sound_def_layer_desc_t;

typedef struct sound_def_desc_s
{
	char		name[MAX_QPATH];
	int		priority;
	int		max_instances;
	qboolean	spatialize;
	qboolean	doppler;
	qboolean	lowpass_by_distance;
	float		reverb_send;
	int		layer_count;
	sound_def_layer_desc_t layers[SOUNDDEF_MAX_LAYERS];
} sound_def_desc_t;

void SoundDef_Init (void);
void SoundDef_Shutdown (void);
void SoundDef_Clear (void);
void SoundDef_LoadAll (void);
void SoundDef_BeginLoad (void);
void SoundDef_FinishLoad (qboolean commit);

void SoundDef_InitDesc (sound_def_desc_t *desc);
void SoundDef_InitLayerDesc (sound_def_layer_desc_t *layer);

qboolean SoundDef_Register (const sound_def_desc_t *desc, const char *source_file, int source_line);
const sound_def_t *SoundDef_Find (const char *name);
const sound_def_t *SoundDef_GetById (sound_def_id_t id);
int SoundDef_Count (void);

const char *SoundDef_BusName (sound_bus_id_t bus_id);
qboolean SoundDef_BusFromName (const char *name, sound_bus_id_t *out_bus_id);

audio_voice_handle_t Audio_PlayDef (const char *name, const audio_play_params_t *params);
audio_voice_handle_t Audio_PlayDefById (sound_def_id_t id, const audio_play_params_t *params);

#endif
