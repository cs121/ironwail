#include "quakedef.h"
#include "sounddef.h"
#include "miniz.h"

typedef struct sounddef_parse_state_s
{
	const char	*source_file;
	const char	*current_def_name;
	unsigned int	line;
	unsigned int	token_line;
	size_t		warnings;
	size_t		errors;
	size_t		loaded_files;
} sounddef_parse_state_t;

static const char *SoundDef_ParseToken (const char *data, sounddef_parse_state_t *state);
static void SoundDef_TrackTokenLine (const char *data, sounddef_parse_state_t *state, const char **token_start);
static void SoundDef_Warn (sounddef_parse_state_t *state, const char *fmt, ...) FUNC_PRINTF(2, 3);
static void SoundDef_Error (sounddef_parse_state_t *state, const char *fmt, ...) FUNC_PRINTF(2, 3);
static const char *SoundDef_SkipBlockContents (const char *cursor, sounddef_parse_state_t *state);
static qboolean SoundDef_ParseBoolToken (const char *token, qboolean *out_value);
static qboolean SoundDef_ParseIntToken (const char *token, int *out_value);
static qboolean SoundDef_ParseFloatToken (const char *token, float *out_value);
static qboolean SoundDef_IsTopLevelLayerKey (const char *token);
static qboolean SoundDef_ParseLayerProperty (const char *key, sound_def_layer_desc_t *layer, const char **cursor, sounddef_parse_state_t *state);
static qboolean SoundDef_ParseDefProperty (const char *key, sound_def_desc_t *desc, const char **cursor, sounddef_parse_state_t *state);
static qboolean SoundDef_ParseLayerBlock (sound_def_desc_t *desc, const char **cursor, sounddef_parse_state_t *state);
static qboolean SoundDef_EnsureImplicitLayer (sound_def_desc_t *desc, qboolean explicit_layers, sound_def_layer_desc_t **out_layer, sounddef_parse_state_t *state);
static int SoundDef_ParseFileBuffer (const char *path, char *buffer, const char *label, sounddef_parse_state_t *totals);
static int SoundDef_ReadFile (const char *path, const byte *data, size_t size, const char *label, sounddef_parse_state_t *totals);
static size_t SoundDef_LoadFromDirectory (const searchpath_t *search, sounddef_parse_state_t *totals);
static size_t SoundDef_LoadFromPack (const searchpath_t *search, sounddef_parse_state_t *totals);

static void SoundDef_TrackTokenLine (const char *data, sounddef_parse_state_t *state, const char **token_start)
{
	const char *cursor;
	unsigned int line;
	int c;

	if (token_start)
		*token_start = data;

	if (!state || !data)
		return;

	line = state->line ? state->line : 1;
	cursor = data;

skipwhite:
	while ((c = *cursor) <= ' ')
	{
		if (c == 0)
		{
			state->line = line;
			state->token_line = line;
			if (token_start)
				*token_start = cursor;
			return;
		}
		if (c == '\n')
			line++;
		cursor++;
	}

	if (c == '/' && cursor[1] == '/')
	{
		while (*cursor && *cursor != '\n')
			cursor++;
		goto skipwhite;
	}

	if (c == '/' && cursor[1] == '*')
	{
		cursor += 2;
		while (*cursor && !(*cursor == '*' && cursor[1] == '/'))
		{
			if (*cursor == '\n')
				line++;
			cursor++;
		}
		if (*cursor)
			cursor += 2;
		goto skipwhite;
	}

	state->line = line;
	state->token_line = line;
	if (token_start)
		*token_start = cursor;
}

static const char *SoundDef_ParseToken (const char *data, sounddef_parse_state_t *state)
{
	const char *token_start = data;
	const char *cursor;

	if (state)
		SoundDef_TrackTokenLine (data, state, &token_start);

	cursor = COM_Parse (data);
	if (!cursor)
		return NULL;

	if (state)
	{
		const char *scan;
		for (scan = token_start; scan < cursor; ++scan)
		{
			if (*scan == '\n')
				state->line++;
		}
	}

	return cursor;
}

static void SoundDef_VLog (sounddef_parse_state_t *state, qboolean error, const char *fmt, va_list ap)
{
	char message[1024];

	q_vsnprintf (message, sizeof (message), fmt, ap);
	if (state && state->current_def_name && state->current_def_name[0])
		Con_Warning ("sounddef %s:%u: %s '%s': %s\n",
			state->source_file ? state->source_file : "<unknown>",
			state->token_line ? state->token_line : state->line,
			error ? "error in" : "warning in",
			state->current_def_name, message);
	else
		Con_Warning ("sounddef %s:%u: %s\n",
			state && state->source_file ? state->source_file : "<unknown>",
			state ? (state->token_line ? state->token_line : state->line) : 0,
			message);
}

static void SoundDef_Warn (sounddef_parse_state_t *state, const char *fmt, ...)
{
	va_list ap;

	if (state)
		state->warnings++;

	va_start (ap, fmt);
	SoundDef_VLog (state, false, fmt, ap);
	va_end (ap);
}

static void SoundDef_Error (sounddef_parse_state_t *state, const char *fmt, ...)
{
	va_list ap;

	if (state)
		state->errors++;

	va_start (ap, fmt);
	SoundDef_VLog (state, true, fmt, ap);
	va_end (ap);
}

static const char *SoundDef_SkipBlockContents (const char *cursor, sounddef_parse_state_t *state)
{
	int depth = 1;

	while ((cursor = SoundDef_ParseToken (cursor, state)) != NULL)
	{
		if (!strcmp (com_token, "{"))
			depth++;
		else if (!strcmp (com_token, "}"))
		{
			depth--;
			if (depth == 0)
				return cursor;
		}
	}

	return NULL;
}

static qboolean SoundDef_ParseBoolToken (const char *token, qboolean *out_value)
{
	if (!token || !token[0] || !out_value)
		return false;

	if (!q_strcasecmp (token, "true") || !q_strcasecmp (token, "yes") || !q_strcasecmp (token, "on") || !strcmp (token, "1"))
	{
		*out_value = true;
		return true;
	}

	if (!q_strcasecmp (token, "false") || !q_strcasecmp (token, "no") || !q_strcasecmp (token, "off") || !strcmp (token, "0"))
	{
		*out_value = false;
		return true;
	}

	return false;
}

static qboolean SoundDef_ParseIntToken (const char *token, int *out_value)
{
	char *end = NULL;
	long value;

	if (!token || !token[0] || !out_value)
		return false;

	value = strtol (token, &end, 10);
	if (!end || *end)
		return false;

	*out_value = (int) value;
	return true;
}

static qboolean SoundDef_ParseFloatToken (const char *token, float *out_value)
{
	char *end = NULL;
	float value;

	if (!token || !token[0] || !out_value)
		return false;

	value = (float) strtod (token, &end);
	if (!end || *end)
		return false;

	*out_value = value;
	return true;
}

static qboolean SoundDef_IsTopLevelLayerKey (const char *token)
{
	return !q_strcasecmp (token, "sample")
		|| !q_strcasecmp (token, "bus")
		|| !q_strcasecmp (token, "volume")
		|| !q_strcasecmp (token, "volume_random")
		|| !q_strcasecmp (token, "pitch")
		|| !q_strcasecmp (token, "pitch_random")
		|| !q_strcasecmp (token, "loop")
		|| !q_strcasecmp (token, "chance")
		|| !q_strcasecmp (token, "delay_ms")
		|| !q_strcasecmp (token, "start_offset_ms");
}

static qboolean SoundDef_ReadValueToken (const char **cursor, sounddef_parse_state_t *state, const char *what)
{
	*cursor = SoundDef_ParseToken (*cursor, state);
	if (!*cursor)
	{
		SoundDef_Error (state, "expected %s, got <eof>", what);
		return false;
	}
	if (!strcmp (com_token, "{") || !strcmp (com_token, "}"))
	{
		SoundDef_Error (state, "expected %s, got '%s'", what, com_token);
		return false;
	}
	return true;
}

static qboolean SoundDef_ParseLayerProperty (const char *key, sound_def_layer_desc_t *layer, const char **cursor, sounddef_parse_state_t *state)
{
	float min_value, max_value, value;
	int int_value;
	qboolean bool_value;
	sound_bus_id_t bus_id;

	if (!q_strcasecmp (key, "sample"))
	{
		if (layer->sample_count >= SOUNDDEF_MAX_SAMPLES_PER_LAYER)
		{
			SoundDef_Error (state, "too many samples in layer (max %d)", SOUNDDEF_MAX_SAMPLES_PER_LAYER);
			return false;
		}
		if (!SoundDef_ReadValueToken (cursor, state, "sample path"))
			return false;
		if (strlen (com_token) >= MAX_QPATH)
		{
			SoundDef_Error (state, "sample path '%s' exceeds MAX_QPATH", com_token);
			return false;
		}
		q_strlcpy (layer->samples[layer->sample_count], com_token, sizeof (layer->samples[layer->sample_count]));
		layer->sample_count++;
		return true;
	}

	if (!q_strcasecmp (key, "bus"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "bus name"))
			return false;
		if (!SoundDef_BusFromName (com_token, &bus_id))
		{
			SoundDef_Error (state, "unknown bus '%s'", com_token);
			return false;
		}
		layer->bus_id = bus_id;
		return true;
	}

	if (!q_strcasecmp (key, "volume"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "volume value"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &value))
		{
			SoundDef_Error (state, "invalid volume value '%s'", com_token);
			return false;
		}
		layer->volume = value;
		return true;
	}

	if (!q_strcasecmp (key, "volume_random"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "volume_random minimum"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &min_value))
		{
			SoundDef_Error (state, "invalid volume_random minimum '%s'", com_token);
			return false;
		}
		if (!SoundDef_ReadValueToken (cursor, state, "volume_random maximum"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &max_value))
		{
			SoundDef_Error (state, "invalid volume_random maximum '%s'", com_token);
			return false;
		}
		layer->volume_random_min = min_value;
		layer->volume_random_max = max_value;
		return true;
	}

	if (!q_strcasecmp (key, "pitch"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "pitch value"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &value))
		{
			SoundDef_Error (state, "invalid pitch value '%s'", com_token);
			return false;
		}
		layer->pitch = value;
		return true;
	}

	if (!q_strcasecmp (key, "pitch_random"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "pitch_random minimum"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &min_value))
		{
			SoundDef_Error (state, "invalid pitch_random minimum '%s'", com_token);
			return false;
		}
		if (!SoundDef_ReadValueToken (cursor, state, "pitch_random maximum"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &max_value))
		{
			SoundDef_Error (state, "invalid pitch_random maximum '%s'", com_token);
			return false;
		}
		layer->pitch_random_min = min_value;
		layer->pitch_random_max = max_value;
		return true;
	}

	if (!q_strcasecmp (key, "loop"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "loop boolean"))
			return false;
		if (!SoundDef_ParseBoolToken (com_token, &bool_value))
		{
			SoundDef_Error (state, "invalid loop value '%s'", com_token);
			return false;
		}
		layer->loop = bool_value;
		return true;
	}

	if (!q_strcasecmp (key, "chance"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "chance value"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &value))
		{
			SoundDef_Error (state, "invalid chance value '%s'", com_token);
			return false;
		}
		layer->chance = value;
		return true;
	}

	if (!q_strcasecmp (key, "delay_ms"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "delay_ms value"))
			return false;
		if (!SoundDef_ParseIntToken (com_token, &int_value))
		{
			SoundDef_Error (state, "invalid delay_ms value '%s'", com_token);
			return false;
		}
		layer->delay_ms = int_value;
		return true;
	}

	if (!q_strcasecmp (key, "start_offset_ms"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "start_offset_ms value"))
			return false;
		if (!SoundDef_ParseIntToken (com_token, &int_value))
		{
			SoundDef_Error (state, "invalid start_offset_ms value '%s'", com_token);
			return false;
		}
		layer->start_offset_ms = int_value;
		return true;
	}

	SoundDef_Error (state, "unknown layer key '%s'", key);
	return false;
}

static qboolean SoundDef_ParseDefProperty (const char *key, sound_def_desc_t *desc, const char **cursor, sounddef_parse_state_t *state)
{
	float value;
	int int_value;
	qboolean bool_value;

	if (!q_strcasecmp (key, "priority"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "priority value"))
			return false;
		if (!SoundDef_ParseIntToken (com_token, &int_value))
		{
			SoundDef_Error (state, "invalid priority value '%s'", com_token);
			return false;
		}
		desc->priority = int_value;
		return true;
	}

	if (!q_strcasecmp (key, "max_instances"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "max_instances value"))
			return false;
		if (!SoundDef_ParseIntToken (com_token, &int_value))
		{
			SoundDef_Error (state, "invalid max_instances value '%s'", com_token);
			return false;
		}
		desc->max_instances = int_value;
		return true;
	}

	if (!q_strcasecmp (key, "spatialize"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "spatialize boolean"))
			return false;
		if (!SoundDef_ParseBoolToken (com_token, &bool_value))
		{
			SoundDef_Error (state, "invalid spatialize value '%s'", com_token);
			return false;
		}
		desc->spatialize = bool_value;
		return true;
	}

	if (!q_strcasecmp (key, "doppler"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "doppler boolean"))
			return false;
		if (!SoundDef_ParseBoolToken (com_token, &bool_value))
		{
			SoundDef_Error (state, "invalid doppler value '%s'", com_token);
			return false;
		}
		desc->doppler = bool_value;
		return true;
	}

	if (!q_strcasecmp (key, "lowpass_by_distance"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "lowpass_by_distance boolean"))
			return false;
		if (!SoundDef_ParseBoolToken (com_token, &bool_value))
		{
			SoundDef_Error (state, "invalid lowpass_by_distance value '%s'", com_token);
			return false;
		}
		desc->lowpass_by_distance = bool_value;
		return true;
	}

	if (!q_strcasecmp (key, "reverb_send"))
	{
		if (!SoundDef_ReadValueToken (cursor, state, "reverb_send value"))
			return false;
		if (!SoundDef_ParseFloatToken (com_token, &value))
		{
			SoundDef_Error (state, "invalid reverb_send value '%s'", com_token);
			return false;
		}
		desc->reverb_send = value;
		return true;
	}

	SoundDef_Error (state, "unknown sound key '%s'", key);
	return false;
}

static qboolean SoundDef_ParseLayerBlock (sound_def_desc_t *desc, const char **cursor, sounddef_parse_state_t *state)
{
	sound_def_layer_desc_t *layer;
	const char *local = *cursor;

	if (desc->layer_count >= SOUNDDEF_MAX_LAYERS)
	{
		SoundDef_Error (state, "too many layers in '%s' (max %d)", desc->name, SOUNDDEF_MAX_LAYERS);
		return false;
	}

	local = SoundDef_ParseToken (local, state);
	if (!local || strcmp (com_token, "{"))
	{
		SoundDef_Error (state, "expected '{' after layer");
		return false;
	}

	layer = &desc->layers[desc->layer_count];
	SoundDef_InitLayerDesc (layer);
	desc->layer_count++;

	while ((local = SoundDef_ParseToken (local, state)) != NULL)
	{
		if (!strcmp (com_token, "}"))
		{
			*cursor = local;
			return true;
		}

		if (!SoundDef_ParseLayerProperty (com_token, layer, &local, state))
		{
			SoundDef_SkipBlockContents (local, state);
			return false;
		}
	}

	SoundDef_Error (state, "unexpected end of file in layer block");
	return false;
}

static qboolean SoundDef_EnsureImplicitLayer (sound_def_desc_t *desc, qboolean explicit_layers, sound_def_layer_desc_t **out_layer, sounddef_parse_state_t *state)
{
	if (explicit_layers)
	{
		SoundDef_Error (state, "top-level layer key cannot be mixed with explicit layer blocks");
		return false;
	}

	if (desc->layer_count == 0)
	{
		SoundDef_InitLayerDesc (&desc->layers[0]);
		desc->layer_count = 1;
	}

	*out_layer = &desc->layers[0];
	return true;
}

static int SoundDef_ParseFileBuffer (const char *path, char *buffer, const char *label, sounddef_parse_state_t *totals)
{
	const char *cursor = buffer;
	int parsed_defs = 0;
	sounddef_parse_state_t state;

	memset (&state, 0, sizeof (state));
	state.source_file = label ? label : path;
	state.line = 1;
	if (totals)
		totals->loaded_files++;

	while ((cursor = SoundDef_ParseToken (cursor, &state)) != NULL)
	{
		sound_def_desc_t desc;
		qboolean explicit_layers = false;

		if (q_strcasecmp (com_token, "sound"))
		{
			SoundDef_Warn (&state, "unexpected top-level token '%s' (expected 'sound')", com_token);
			continue;
		}

		SoundDef_InitDesc (&desc);
		cursor = SoundDef_ParseToken (cursor, &state);
		if (!cursor)
		{
			SoundDef_Error (&state, "expected sound definition name, got <eof>");
			break;
		}
		if (!com_token[0] || !strcmp (com_token, "{") || !strcmp (com_token, "}"))
		{
			SoundDef_Error (&state, "expected sound definition name, got '%s'", com_token[0] ? com_token : "<empty>");
			continue;
		}
		if (strlen (com_token) >= sizeof (desc.name))
		{
			SoundDef_Error (&state, "definition name '%s' exceeds MAX_QPATH", com_token);
			cursor = SoundDef_ParseToken (cursor, &state);
			if (cursor && !strcmp (com_token, "{"))
				cursor = SoundDef_SkipBlockContents (cursor, &state);
			continue;
		}
		q_strlcpy (desc.name, com_token, sizeof (desc.name));
		state.current_def_name = desc.name;

		cursor = SoundDef_ParseToken (cursor, &state);
		if (!cursor || strcmp (com_token, "{"))
		{
			SoundDef_Error (&state, "expected '{' after sound '%s'", desc.name);
			state.current_def_name = NULL;
			continue;
		}

		while ((cursor = SoundDef_ParseToken (cursor, &state)) != NULL)
		{
			if (!strcmp (com_token, "}"))
			{
				if (SoundDef_Register (&desc, state.source_file, (int) state.token_line))
					parsed_defs++;
				else
					state.errors++;
				break;
			}

			if (!q_strcasecmp (com_token, "layer"))
			{
				if (desc.layer_count > 0 && !explicit_layers)
				{
					SoundDef_Error (&state, "explicit layer blocks cannot be mixed with top-level sample/layer properties");
					cursor = SoundDef_SkipBlockContents (cursor, &state);
					break;
				}

				explicit_layers = true;
				if (!SoundDef_ParseLayerBlock (&desc, &cursor, &state))
				{
					cursor = SoundDef_SkipBlockContents (cursor, &state);
					break;
				}
				continue;
			}

			if (SoundDef_IsTopLevelLayerKey (com_token))
			{
				sound_def_layer_desc_t *layer = NULL;

				if (!SoundDef_EnsureImplicitLayer (&desc, explicit_layers, &layer, &state))
				{
					cursor = SoundDef_SkipBlockContents (cursor, &state);
					break;
				}
				if (!SoundDef_ParseLayerProperty (com_token, layer, &cursor, &state))
				{
					cursor = SoundDef_SkipBlockContents (cursor, &state);
					break;
				}
				continue;
			}

			if (!SoundDef_ParseDefProperty (com_token, &desc, &cursor, &state))
			{
				cursor = SoundDef_SkipBlockContents (cursor, &state);
				break;
			}
		}

		if (!cursor)
		{
			SoundDef_Error (&state, "unexpected end of file in sound '%s'", desc.name);
			break;
		}

		state.current_def_name = NULL;
	}

	if (totals)
	{
		totals->warnings += state.warnings;
		totals->errors += state.errors;
	}

	return parsed_defs;
}

static int SoundDef_ReadFile (const char *path, const byte *data, size_t size, const char *label, sounddef_parse_state_t *totals)
{
	char *buffer;
	int parsed;

	if (!data || size == 0)
		return 0;

	buffer = (char *) q_malloc (size + 1);
	if (!buffer)
	{
		Con_Warning ("sounddef %s: out of memory while reading file\n", label ? label : path);
		return 0;
	}

	memcpy (buffer, data, size);
	buffer[size] = '\0';
	parsed = SoundDef_ParseFileBuffer (path, buffer, label, totals);
	q_free (buffer);
	return parsed;
}

static size_t SoundDef_LoadFromDirectory (const searchpath_t *search, sounddef_parse_state_t *totals)
{
	char script_dir[MAX_OSPATH];
	findfile_t *find;
	size_t parsed_total = 0;

	if ((size_t) q_snprintf (script_dir, sizeof (script_dir), "%s/sounddefs", search->filename) >= sizeof (script_dir))
		return 0;

	for (find = Sys_FindFirst (script_dir, "sndshd"); find; find = Sys_FindNext (find))
	{
		char fullpath[MAX_OSPATH];
		char relpath[MAX_QPATH];
		byte *buffer;
		int handle;
		int size;

		if (find->attribs & FA_DIRECTORY)
			continue;

		q_snprintf (relpath, sizeof (relpath), "sounddefs/%s", find->name);
		q_snprintf (fullpath, sizeof (fullpath), "%s/%s", script_dir, find->name);

		size = (int) Sys_FileOpenRead (fullpath, &handle);
		if (size <= 0)
		{
			if (handle >= 0)
				Sys_FileClose (handle);
			continue;
		}

		buffer = (byte *) q_malloc (size + 1);
		if (!buffer)
		{
			Sys_FileClose (handle);
			continue;
		}

		if (Sys_FileRead (handle, buffer, size) != size)
		{
			q_free (buffer);
			Sys_FileClose (handle);
			continue;
		}

		Sys_FileClose (handle);
		buffer[size] = '\0';
		parsed_total += (size_t) SoundDef_ReadFile (relpath, buffer, (size_t) size, relpath, totals);
		q_free (buffer);
	}

	return parsed_total;
}

static size_t SoundDef_LoadFromPack (const searchpath_t *search, sounddef_parse_state_t *totals)
{
	int i;
	size_t parsed_total = 0;
	pack_t *pak = search->pack;

	for (i = 0; i < pak->numfiles; ++i)
	{
		const char *name = pak->files[i].name;
		size_t size = (size_t) pak->files[i].filelen;
		byte *buffer = NULL;

		if (q_strncasecmp (name, "sounddefs/", 10))
			continue;
		if (q_strcasecmp (COM_FileGetExtension (name), "sndshd"))
			continue;

		if (pak->is_pk3)
		{
			size_t extracted_size = 0;
			void *extracted = mz_zip_reader_extract_to_heap ((mz_zip_archive *) pak->zip, pak->files[i].filepos, &extracted_size, 0);
			if (!extracted || extracted_size == 0)
				continue;
			buffer = (byte *) extracted;
			size = extracted_size;
		}
		else
		{
			buffer = (byte *) q_malloc (size + 1);
			if (!buffer)
				continue;
			Sys_FileSeek (pak->handle, pak->files[i].filepos);
			if (Sys_FileRead (pak->handle, buffer, (int) size) != (int) size)
			{
				q_free (buffer);
				continue;
			}
		}

		if (buffer)
		{
			buffer[size] = '\0';
			parsed_total += (size_t) SoundDef_ReadFile (name, buffer, size, name, totals);
			if (pak->is_pk3)
				MZ_FREE (buffer);
			else
				q_free (buffer);
		}
	}

	return parsed_total;
}

void SoundDef_LoadAll (void)
{
	searchpath_t *search;
	searchpath_t **paths = NULL;
	sounddef_parse_state_t totals;
	size_t count;
	size_t i;
	size_t parsed_total = 0;
	int previous_count;
	qboolean committed;

	memset (&totals, 0, sizeof (totals));
	previous_count = SoundDef_Count ();
	SoundDef_BeginLoad ();

	for (search = com_searchpaths; search; search = search->next)
		VEC_PUSH (paths, search);

	count = VEC_SIZE (paths);
	for (i = count; i > 0; --i)
	{
		search = paths[i - 1];
		if (*search->filename)
			parsed_total += SoundDef_LoadFromDirectory (search, &totals);
		else if (search->pack)
			parsed_total += SoundDef_LoadFromPack (search, &totals);
	}

	VEC_FREE (paths);

	committed = (totals.errors == 0);
	SoundDef_FinishLoad (committed);

	Con_Printf ("Sound defs: loaded %zu defs from %zu file%s (%zu warning%s, %zu error%s)\n",
		parsed_total,
		totals.loaded_files,
		PLURAL (totals.loaded_files),
		totals.warnings,
		PLURAL (totals.warnings),
		totals.errors,
		PLURAL (totals.errors));

	if (!committed && previous_count > 0)
		Con_Warning ("Sound defs: keeping previous registry (%d defs) because reload had errors\n", previous_count);
}
