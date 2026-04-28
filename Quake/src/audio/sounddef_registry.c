#include "quakedef.h"
#include "sounddef.h"

typedef struct sound_def_registry_s
{
	int		count;
	sound_def_t	defs[SOUNDDEF_MAX_DEFS];
} sound_def_registry_t;

static sound_def_registry_t sounddef_registry;
static sound_def_registry_t sounddef_staging_registry;
static sound_def_registry_t *sounddef_build_registry;
static qboolean sounddef_commands_registered;

static void SoundDef_List_f (void);
static void SoundDef_Print_f (void);
static void SoundDef_Reload_f (void);
static qboolean SoundDef_ValidateAndCopy (sound_def_t *out_def, const sound_def_desc_t *desc, const char *source_file, int source_line, const sound_def_registry_t *registry);
static qboolean SoundDef_ValidateLayer (sound_def_layer_t *out_layer, const sound_def_layer_desc_t *layer_desc, const char *def_name, int layer_index, const char *source_file, int source_line);
static qboolean SoundDef_NormalizeSamplePath (const char *input, char *output, size_t output_size);
static qboolean SoundDef_BuildAssetPath (const char *sample_path, char *output, size_t output_size);
static const char *SoundDef_SourceFileOrRuntime (const char *source_file);
static const sound_def_t *SoundDef_FindInRegistry (const sound_def_registry_t *registry, const char *name);
static sound_def_registry_t *SoundDef_TargetRegistry (void);

void SoundDef_InitDesc (sound_def_desc_t *desc)
{
	int i;

	if (!desc)
		return;

	memset (desc, 0, sizeof (*desc));
	desc->priority = 128;
	desc->spatialize = true;
	desc->doppler = false;
	desc->lowpass_by_distance = false;
	desc->reverb_send = 0.f;
	desc->layer_count = 0;

	for (i = 0; i < SOUNDDEF_MAX_LAYERS; i++)
		SoundDef_InitLayerDesc (&desc->layers[i]);
}

void SoundDef_InitLayerDesc (sound_def_layer_desc_t *layer)
{
	if (!layer)
		return;

	memset (layer, 0, sizeof (*layer));
	layer->bus_id = SOUND_BUS_SFX;
	layer->volume = 1.f;
	layer->volume_random_min = 1.f;
	layer->volume_random_max = 1.f;
	layer->pitch = 1.f;
	layer->pitch_random_min = 1.f;
	layer->pitch_random_max = 1.f;
	layer->chance = 1.f;
}

void SoundDef_Init (void)
{
	SoundDef_Clear ();
	memset (&sounddef_staging_registry, 0, sizeof (sounddef_staging_registry));
	sounddef_build_registry = NULL;

	if (!sounddef_commands_registered)
	{
		Cmd_AddCommand ("snd_list_defs", SoundDef_List_f);
		Cmd_AddCommand ("snd_print_def", SoundDef_Print_f);
		Cmd_AddCommand ("snd_reload_defs", SoundDef_Reload_f);
		sounddef_commands_registered = true;
	}
}

void SoundDef_Shutdown (void)
{
	SoundDef_Clear ();
	memset (&sounddef_staging_registry, 0, sizeof (sounddef_staging_registry));
	sounddef_build_registry = NULL;
}

void SoundDef_Clear (void)
{
	memset (sounddef_registry.defs, 0, sizeof (sounddef_registry.defs));
	sounddef_registry.count = 0;
}

void SoundDef_BeginLoad (void)
{
	memset (&sounddef_staging_registry, 0, sizeof (sounddef_staging_registry));
	sounddef_build_registry = &sounddef_staging_registry;
}

void SoundDef_FinishLoad (qboolean commit)
{
	if (sounddef_build_registry != &sounddef_staging_registry)
		return;

	if (commit)
		sounddef_registry = sounddef_staging_registry;

	memset (&sounddef_staging_registry, 0, sizeof (sounddef_staging_registry));
	sounddef_build_registry = NULL;
}

qboolean SoundDef_Register (const sound_def_desc_t *desc, const char *source_file, int source_line)
{
	sound_def_registry_t *registry = SoundDef_TargetRegistry ();
	sound_def_t def;

	{
		char tracebuf[256];
		q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_Register: begin name=%s", desc ? desc->name : "<null>");
		TexMgr_Trace (tracebuf);
	}
	if (!desc)
	{
		Con_Warning ("sounddef %s:%d: null definition descriptor\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line);
		return false;
	}

	if (registry->count >= SOUNDDEF_MAX_DEFS)
	{
		Con_Warning ("sounddef %s:%d: registry full (%d defs max)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, SOUNDDEF_MAX_DEFS);
		return false;
	}

	TexMgr_Trace ("SoundDef_Register: before validate");
	if (!SoundDef_ValidateAndCopy (&def, desc, source_file, source_line, registry))
	{
		TexMgr_Trace ("SoundDef_Register: validate failed");
		return false;
	}
	TexMgr_Trace ("SoundDef_Register: after validate");

	TexMgr_Trace ("SoundDef_Register: assign id");
	def.id = registry->count + 1;
	TexMgr_Trace ("SoundDef_Register: copy to registry");
	registry->defs[registry->count] = def;
	TexMgr_Trace ("SoundDef_Register: increment count");
	registry->count++;
	TexMgr_Trace ("SoundDef_Register: end");
	return true;
}

const sound_def_t *SoundDef_Find (const char *name)
{
	return SoundDef_FindInRegistry (&sounddef_registry, name);
}

const sound_def_t *SoundDef_GetById (sound_def_id_t id)
{
	if (id <= 0 || id > sounddef_registry.count)
		return NULL;

	return &sounddef_registry.defs[id - 1];
}

int SoundDef_Count (void)
{
	return sounddef_registry.count;
}

const char *SoundDef_BusName (sound_bus_id_t bus_id)
{
	switch (bus_id)
	{
	case SOUND_BUS_SFX:
		return "sfx";
	case SOUND_BUS_UI:
		return "ui";
	case SOUND_BUS_AMBIENT:
		return "ambient";
	case SOUND_BUS_MUSIC:
		return "music";
	default:
		return "invalid";
	}
}

qboolean SoundDef_BusFromName (const char *name, sound_bus_id_t *out_bus_id)
{
	static const struct
	{
		const char *name;
		sound_bus_id_t bus_id;
	} buses[] =
	{
		{"sfx", SOUND_BUS_SFX},
		{"ui", SOUND_BUS_UI},
		{"ambient", SOUND_BUS_AMBIENT},
		{"music", SOUND_BUS_MUSIC}
	};
	size_t i;

	if (!name || !name[0] || !out_bus_id)
		return false;

	for (i = 0; i < countof (buses); i++)
	{
		if (!q_strcasecmp (name, buses[i].name))
		{
			*out_bus_id = buses[i].bus_id;
			return true;
		}
	}

	return false;
}

static qboolean SoundDef_ValidateAndCopy (sound_def_t *out_def, const sound_def_desc_t *desc, const char *source_file, int source_line, const sound_def_registry_t *registry)
{
	int layer_index;

	{
		char tracebuf[256];
		q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_ValidateAndCopy: begin name=%s", desc->name);
		TexMgr_Trace (tracebuf);
	}
	memset (out_def, 0, sizeof (*out_def));

	if (!desc->name || !desc->name[0])
	{
		Con_Warning ("sounddef %s:%d: missing definition name\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line);
		return false;
	}

	if (strlen (desc->name) >= MAX_QPATH)
	{
		Con_Warning ("sounddef %s:%d: definition name '%s' exceeds MAX_QPATH\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, desc->name);
		return false;
	}

	if (SoundDef_FindInRegistry (registry, desc->name))
	{
		Con_Warning ("sounddef %s:%d: duplicate definition '%s'\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, desc->name);
		return false;
	}

	if (desc->layer_count <= 0 || desc->layer_count > SOUNDDEF_MAX_LAYERS)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' has invalid layer count %d (expected 1-%d)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, desc->name,
			desc->layer_count, SOUNDDEF_MAX_LAYERS);
		return false;
	}

	if (desc->priority < 0 || desc->priority > 255)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' has invalid priority %d (expected 0-255)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, desc->name, desc->priority);
		return false;
	}

	if (desc->max_instances < 0 || desc->max_instances > MAX_CHANNELS)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' has invalid max_instances %d (expected 0-%d)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, desc->name,
			desc->max_instances, MAX_CHANNELS);
		return false;
	}

	if (desc->reverb_send < 0.f || desc->reverb_send > 1.f)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' has invalid reverb_send %.3f (expected 0-1)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, desc->name, desc->reverb_send);
		return false;
	}

	q_strlcpy (out_def->name, desc->name, sizeof (out_def->name));
	q_strlcpy (out_def->source_file, SoundDef_SourceFileOrRuntime (source_file), sizeof (out_def->source_file));
	out_def->source_line = source_line;
	out_def->priority = desc->priority;
	out_def->max_instances = desc->max_instances;
	out_def->spatialize = desc->spatialize;
	out_def->doppler = desc->doppler;
	out_def->lowpass_by_distance = desc->lowpass_by_distance;
	out_def->reverb_send = desc->reverb_send;
	out_def->layer_count = desc->layer_count;

	for (layer_index = 0; layer_index < desc->layer_count; layer_index++)
	{
		{
			char tracebuf[256];
			q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_ValidateAndCopy: layer=%d name=%s", layer_index, desc->name);
			TexMgr_Trace (tracebuf);
		}
		if (!SoundDef_ValidateLayer (&out_def->layers[layer_index], &desc->layers[layer_index],
			desc->name, layer_index, source_file, source_line))
			return false;
	}

	TexMgr_Trace ("SoundDef_ValidateAndCopy: end");
	return true;
}

static qboolean SoundDef_ValidateLayer (sound_def_layer_t *out_layer, const sound_def_layer_desc_t *layer_desc, const char *def_name, int layer_index, const char *source_file, int source_line)
{
	int sample_index;

	{
		char tracebuf[256];
		q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_ValidateLayer: begin def=%s layer=%d", def_name, layer_index);
		TexMgr_Trace (tracebuf);
	}
	memset (out_layer, 0, sizeof (*out_layer));

	if (layer_desc->sample_count <= 0 || layer_desc->sample_count > SOUNDDEF_MAX_SAMPLES_PER_LAYER)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid sample count %d (expected 1-%d)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index,
			layer_desc->sample_count, SOUNDDEF_MAX_SAMPLES_PER_LAYER);
		return false;
	}

	if (layer_desc->bus_id < 0 || layer_desc->bus_id >= SOUND_BUS_COUNT)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid bus id %d\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index, layer_desc->bus_id);
		return false;
	}

	if (layer_desc->volume < 0.f || layer_desc->volume > 4.f)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid volume %.3f (expected 0-4)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index, layer_desc->volume);
		return false;
	}

	if (layer_desc->volume_random_min < 0.f || layer_desc->volume_random_max < 0.f
		|| layer_desc->volume_random_min > layer_desc->volume_random_max
		|| layer_desc->volume_random_max > 4.f)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid volume_random range %.3f..%.3f\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index,
			layer_desc->volume_random_min, layer_desc->volume_random_max);
		return false;
	}

	if (layer_desc->pitch <= 0.f || layer_desc->pitch > 8.f)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid pitch %.3f (expected >0 and <=8)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index, layer_desc->pitch);
		return false;
	}

	if (layer_desc->pitch_random_min <= 0.f || layer_desc->pitch_random_max <= 0.f
		|| layer_desc->pitch_random_min > layer_desc->pitch_random_max
		|| layer_desc->pitch_random_max > 8.f)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid pitch_random range %.3f..%.3f\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index,
			layer_desc->pitch_random_min, layer_desc->pitch_random_max);
		return false;
	}

	if (layer_desc->chance < 0.f || layer_desc->chance > 1.f)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid chance %.3f (expected 0-1)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index, layer_desc->chance);
		return false;
	}

	if (layer_desc->delay_ms < 0 || layer_desc->start_offset_ms < 0)
	{
		Con_Warning ("sounddef %s:%d: definition '%s' layer %d has negative delay/start offsets (%d, %d)\n",
			SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index,
			layer_desc->delay_ms, layer_desc->start_offset_ms);
		return false;
	}

	out_layer->sample_count = layer_desc->sample_count;
	out_layer->bus_id = layer_desc->bus_id;
	out_layer->volume = layer_desc->volume;
	out_layer->volume_random_min = layer_desc->volume_random_min;
	out_layer->volume_random_max = layer_desc->volume_random_max;
	out_layer->pitch = layer_desc->pitch;
	out_layer->pitch_random_min = layer_desc->pitch_random_min;
	out_layer->pitch_random_max = layer_desc->pitch_random_max;
	out_layer->loop = layer_desc->loop;
	out_layer->chance = layer_desc->chance;
	out_layer->delay_ms = layer_desc->delay_ms;
	out_layer->start_offset_ms = layer_desc->start_offset_ms;

	for (sample_index = 0; sample_index < layer_desc->sample_count; sample_index++)
	{
		char sample_path[MAX_QPATH];
		char asset_path[256];
		sfx_t *sfx;

		{
			char tracebuf[256];
			q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_ValidateLayer: sample=%d raw=%s", sample_index, layer_desc->samples[sample_index]);
			TexMgr_Trace (tracebuf);
		}
		if (!SoundDef_NormalizeSamplePath (layer_desc->samples[sample_index], sample_path, sizeof (sample_path)))
		{
			Con_Warning ("sounddef %s:%d: definition '%s' layer %d has invalid sample path\n",
				SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index);
			return false;
		}
		{
			char tracebuf[256];
			q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_ValidateLayer: sample=%d normalized=%s", sample_index, sample_path);
			TexMgr_Trace (tracebuf);
		}

		if (!SoundDef_BuildAssetPath (sample_path, asset_path, sizeof (asset_path)))
		{
			Con_Warning ("sounddef %s:%d: definition '%s' layer %d sample %d path '%s' is too long\n",
				SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index,
				sample_index, layer_desc->samples[sample_index]);
			return false;
		}
		{
			char tracebuf[256];
			q_snprintf (tracebuf, sizeof (tracebuf), "SoundDef_ValidateLayer: sample=%d asset=%s", sample_index, asset_path);
			TexMgr_Trace (tracebuf);
		}

		sfx = S_PrecacheSound (sample_path);
		if (!sfx)
		{
			Con_Warning ("sounddef %s:%d: definition '%s' layer %d sample '%s' could not be resolved\n",
				SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index, sample_path);
			return false;
		}
		TexMgr_Trace ("SoundDef_ValidateLayer: precache ok");

		if (!Host_AsyncAssetsEnabled () && !S_LoadSound (sfx))
		{
			Con_Warning ("sounddef %s:%d: definition '%s' layer %d sample '%s' failed to load\n",
				SoundDef_SourceFileOrRuntime (source_file), source_line, def_name, layer_index, asset_path);
			return false;
		}
		TexMgr_Trace ("SoundDef_ValidateLayer: load ok");
		q_strlcpy (out_layer->samples[sample_index].path, sample_path, sizeof (out_layer->samples[sample_index].path));
		out_layer->samples[sample_index].sfx = sfx;
	}

	TexMgr_Trace ("SoundDef_ValidateLayer: end");
	return true;
}

static qboolean SoundDef_NormalizeSamplePath (const char *input, char *output, size_t output_size)
{
	size_t i;
	const char *start;

	if (!input || !input[0] || !output || output_size < 2)
		return false;

	start = input;
	if (!q_strncasecmp (start, "sound/", 6))
		start += 6;

	if (!start[0])
		return false;

	for (i = 0; start[i]; i++)
	{
		char c = start[i];

		if (i + 1 >= output_size)
			return false;

		if (c == '\\')
			c = '/';

		output[i] = c;
	}

	output[i] = 0;
	return true;
}

static qboolean SoundDef_BuildAssetPath (const char *sample_path, char *output, size_t output_size)
{
	int written;

	if (!sample_path || !sample_path[0] || !output || !output_size)
		return false;

	written = q_snprintf (output, output_size, "sound/%s", sample_path);
	return written > 0 && (size_t) written < output_size;
}

static const char *SoundDef_SourceFileOrRuntime (const char *source_file)
{
	return (source_file && source_file[0]) ? source_file : "<runtime>";
}

static const sound_def_t *SoundDef_FindInRegistry (const sound_def_registry_t *registry, const char *name)
{
	int i;

	if (!registry || !name || !name[0])
		return NULL;

	for (i = 0; i < registry->count; i++)
	{
		if (!strcmp (registry->defs[i].name, name))
			return &registry->defs[i];
	}

	return NULL;
}

static sound_def_registry_t *SoundDef_TargetRegistry (void)
{
	return sounddef_build_registry ? sounddef_build_registry : &sounddef_registry;
}

static void SoundDef_List_f (void)
{
	int i;

	Con_Printf ("Sound defs: %d\n", sounddef_registry.count);
	for (i = 0; i < sounddef_registry.count; i++)
	{
		const sound_def_t *def = &sounddef_registry.defs[i];
		Con_Printf (" %3d %s (layers=%d priority=%d max_instances=%d)\n",
			def->id, def->name, def->layer_count, def->priority, def->max_instances);
	}
}

static void SoundDef_Print_f (void)
{
	const sound_def_t *def;
	int layer_index;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("snd_print_def <name>\n");
		return;
	}

	def = SoundDef_Find (Cmd_Argv (1));
	if (!def)
	{
		Con_Printf ("snd_print_def: '%s' not found\n", Cmd_Argv (1));
		return;
	}

	Con_Printf ("sound %s\n", def->name);
	Con_Printf (" id %d\n", def->id);
	Con_Printf (" source %s:%d\n", def->source_file, def->source_line);
	Con_Printf (" priority %d\n", def->priority);
	Con_Printf (" max_instances %d\n", def->max_instances);
	Con_Printf (" spatialize %s\n", def->spatialize ? "true" : "false");
	Con_Printf (" doppler %s\n", def->doppler ? "true" : "false");
	Con_Printf (" lowpass_by_distance %s\n", def->lowpass_by_distance ? "true" : "false");
	Con_Printf (" reverb_send %.3f\n", def->reverb_send);

	for (layer_index = 0; layer_index < def->layer_count; layer_index++)
	{
		const sound_def_layer_t *layer = &def->layers[layer_index];
		int sample_index;

		Con_Printf (" layer %d\n", layer_index);
		Con_Printf ("  bus %s\n", SoundDef_BusName (layer->bus_id));
		Con_Printf ("  volume %.3f\n", layer->volume);
		Con_Printf ("  volume_random %.3f %.3f\n", layer->volume_random_min, layer->volume_random_max);
		Con_Printf ("  pitch %.3f\n", layer->pitch);
		Con_Printf ("  pitch_random %.3f %.3f\n", layer->pitch_random_min, layer->pitch_random_max);
		Con_Printf ("  loop %s\n", layer->loop ? "true" : "false");
		Con_Printf ("  chance %.3f\n", layer->chance);
		Con_Printf ("  delay_ms %d\n", layer->delay_ms);
		Con_Printf ("  start_offset_ms %d\n", layer->start_offset_ms);

		for (sample_index = 0; sample_index < layer->sample_count; sample_index++)
			Con_Printf ("  sample sound/%s\n", layer->samples[sample_index].path);
	}
}

static void SoundDef_Reload_f (void)
{
	Con_Printf ("Reloading sound defs\n");
	SoundDef_LoadAll ();
}
