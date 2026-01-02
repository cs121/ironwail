#include "quakedef.h"
#include "rtlight.h"
#include <ctype.h>

#define RTLIGHT_MAX_TOKENS 64

static void RTLight_Warnf (rtlight_list_t *out, const char *fmt, ...)
{
	va_list argptr;
	char msg[1024];

	if (out)
		out->warnings++;

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_Warning ("%s", msg);
}

static void RTLight_SkipLine (rtlight_list_t *out)
{
	if (out)
		out->skipped_lines++;
}

static void RTLight_StripInlineComment (char *line)
{
	qboolean in_quote = false;
	char *p = line;

	for (; *p; p++)
	{
		if (*p == '"')
		{
			in_quote = !in_quote;
			continue;
		}
		if (!in_quote && p[0] == '/' && p[1] == '/')
		{
			*p = '\0';
			return;
		}
	}
}

static int RTLight_Tokenize (char *line, char **tokens, int max_tokens)
{
	int count = 0;
	char *p = line;

	while (*p && count < max_tokens)
	{
		qboolean in_quote = false;
		char *start;

		while (*p && isspace ((unsigned char)*p))
			p++;
		if (!*p)
			break;

		start = p;
		while (*p)
		{
			if (*p == '"')
				in_quote = !in_quote;
			else if (!in_quote && isspace ((unsigned char)*p))
				break;
			p++;
		}

		if (*p)
		{
			*p = '\0';
			p++;
		}

		tokens[count++] = start;
	}

	return count;
}

static void RTLight_Defaults (rtlight_t *light)
{
	VectorClear (light->origin);
	light->radius = 0.0f;
	VectorClear (light->color);
	light->intensity = 0.0f;
	light->style = 0;
	light->type = RTL_POINT;
	VectorSet (light->dir, 0.0f, 0.0f, -1.0f);
	light->angle_deg = 45.0f;
	light->soft = 0.0f;
	light->casts_shadows = true;
	light->corona = false;
	light->corona_size = 0.0f;
	q_strlcpy (light->corona_tex, "textures/effects/corona", sizeof(light->corona_tex));
	light->corona_alpha = 0.6f;
	light->envmap = false;
	light->envmap_tex[0] = '\0';
	light->envmap_strength = 0.3f;
	light->group[0] = '\0';
	light->name[0] = '\0';
	light->id = 0;
}

static qboolean RTLight_ParseVec3 (const char *value, vec3_t out)
{
	float x, y, z;

	if (sscanf (value, "%f,%f,%f", &x, &y, &z) != 3)
		return false;

	out[0] = x;
	out[1] = y;
	out[2] = z;
	return true;
}

static void RTLight_StripQuotes (char *value)
{
	size_t len;

	if (!value || !value[0])
		return;

	len = strlen (value);
	if (len >= 2 && value[0] == '"' && value[len - 1] == '"')
	{
		value[len - 1] = '\0';
		memmove (value, value + 1, len - 1);
	}
}

static qboolean RTLight_ParseLine (const char *line, rtlight_t *out_light, int default_id, rtlight_list_t *out)
{
	char buffer[1024];
	char *tokens[RTLIGHT_MAX_TOKENS];
	int num_tokens;
	int i;

	q_strlcpy (buffer, line, sizeof(buffer));
	RTLight_StripInlineComment (buffer);

	for (i = 0; buffer[i]; i++)	
	{
		if (!isspace ((unsigned char)buffer[i]))
			break;
	}
	if (!buffer[i])
		return false;
	if (buffer[i] == '#')
		return false;
	if (buffer[i] == '/' && buffer[i + 1] == '/')
		return false;

	num_tokens = RTLight_Tokenize (buffer, tokens, RTLIGHT_MAX_TOKENS);
	if (num_tokens == 0)
		return false;
	if (num_tokens < 8)
	{
		RTLight_Warnf (out, "RTLight_Parse: skipping line (not enough fields): %s\n", line);
		RTLight_SkipLine (out);
		return false;
	}

	RTLight_Defaults (out_light);
	out_light->id = default_id;

	out_light->origin[0] = Q_atof (tokens[0]);
	out_light->origin[1] = Q_atof (tokens[1]);
	out_light->origin[2] = Q_atof (tokens[2]);
	out_light->radius = Q_atof (tokens[3]);
	out_light->color[0] = Q_atof (tokens[4]);
	out_light->color[1] = Q_atof (tokens[5]);
	out_light->color[2] = Q_atof (tokens[6]);
	out_light->intensity = Q_atof (tokens[7]);

	if (out_light->radius <= 0.0f)
	{
		RTLight_Warnf (out, "RTLight_Parse: skipping line (radius <= 0): %s\n", line);
		RTLight_SkipLine (out);
		return false;
	}

	if (out_light->color[0] < 0.0f || out_light->color[1] < 0.0f || out_light->color[2] < 0.0f || out_light->intensity < 0.0f)
	{
		RTLight_Warnf (out, "RTLight_Parse: skipping line (negative color/intensity): %s\n", line);
		RTLight_SkipLine (out);
		return false;
	}

	out_light->radius = CLAMP (0.001f, out_light->radius, 100000.0f);
	out_light->color[0] = q_max (0.0f, out_light->color[0]);
	out_light->color[1] = q_max (0.0f, out_light->color[1]);
	out_light->color[2] = q_max (0.0f, out_light->color[2]);
	out_light->intensity = q_max (0.0f, out_light->intensity);

	for (i = 8; i < num_tokens; i++)	
	{
		char *token = tokens[i];
		char *equals = strchr (token, '=');
		char key[64];
		char value[256];
		int key_len;

		if (!equals)
		{
			RTLight_Warnf (out, "RTLight_Parse: ignoring token without key=value: %s\n", token);
			continue;
		}

		key_len = (int) (equals - token);
		if (key_len <= 0 || key_len >= (int)sizeof(key))
		{
			RTLight_Warnf (out, "RTLight_Parse: ignoring malformed key token: %s\n", token);
			continue;
		}

		memcpy (key, token, key_len);
		key[key_len] = '\0';
		q_strlcpy (value, equals + 1, sizeof(value));
		q_strlwr (key);
		RTLight_StripQuotes (value);

		if (!strcmp (key, "style"))
			out_light->style = Q_atoi (value);
		else if (!strcmp (key, "type"))
		{
			q_strlwr (value);
			if (!strcmp (value, "point"))
				out_light->type = RTL_POINT;
			else if (!strcmp (value, "spot"))
				out_light->type = RTL_SPOT;
			else
				RTLight_Warnf (out, "RTLight_Parse: unknown type '%s'\n", value);
		}
		else if (!strcmp (key, "dir"))
		{
			vec3_t dir;
			if (RTLight_ParseVec3 (value, dir))
				VectorCopy (dir, out_light->dir);
			else
				RTLight_Warnf (out, "RTLight_Parse: invalid dir '%s'\n", value);
		}
		else if (!strcmp (key, "angle"))
			out_light->angle_deg = Q_atof (value);
		else if (!strcmp (key, "soft"))
			out_light->soft = Q_atof (value);
		else if (!strcmp (key, "casts_shadows"))
			out_light->casts_shadows = Q_atoi (value) ? true : false;
		else if (!strcmp (key, "corona"))
			out_light->corona = Q_atoi (value) ? true : false;
		else if (!strcmp (key, "corona_size"))
			out_light->corona_size = Q_atof (value);
		else if (!strcmp (key, "corona_tex"))
		{
			q_strlcpy (out_light->corona_tex, value, sizeof(out_light->corona_tex));
			q_strlwr (out_light->corona_tex);
		}
		else if (!strcmp (key, "corona_alpha"))
			out_light->corona_alpha = Q_atof (value);
		else if (!strcmp (key, "envmap"))
			out_light->envmap = Q_atoi (value) ? true : false;
		else if (!strcmp (key, "envmap_tex"))
		{
			q_strlcpy (out_light->envmap_tex, value, sizeof(out_light->envmap_tex));
			q_strlwr (out_light->envmap_tex);
		}
		else if (!strcmp (key, "envmap_strength"))
			out_light->envmap_strength = Q_atof (value);
		else if (!strcmp (key, "group"))
			q_strlcpy (out_light->group, value, sizeof(out_light->group));
		else if (!strcmp (key, "name"))
			q_strlcpy (out_light->name, value, sizeof(out_light->name));
		else if (!strcmp (key, "id"))
			out_light->id = Q_atoi (value);
		else
			RTLight_Warnf (out, "RTLight_Parse: unknown key '%s'\n", key);
	}

	out_light->angle_deg = CLAMP (1.0f, out_light->angle_deg, 179.0f);
	out_light->soft = CLAMP (0.0f, out_light->soft, 1.0f);
	out_light->corona_alpha = CLAMP (0.0f, out_light->corona_alpha, 1.0f);
	out_light->envmap_strength = CLAMP (0.0f, out_light->envmap_strength, 1.0f);
	out_light->style = CLAMP (0, out_light->style, 255);

	if (out_light->corona_size <= 0.0f)
		out_light->corona_size = out_light->radius * 0.05f;

	return true;
}

void RTLight_ParseFile (const char *mapname, rtlight_list_t *out)
{
	char filename[MAX_QPATH];
	char *data;
	char *line_start;
	char *line_end;
	int line_index = 0;
	int parsed = 0;
	int max_lights;

	if (!mapname || !mapname[0])
		return;

	q_snprintf (filename, sizeof(filename), "maps/%s.rtlight", mapname);
	data = (char *) COM_LoadMallocFile (filename, NULL);
	if (!data)
		return;

	max_lights = (int) r_rtlights_max.value;
	if (max_lights <= 0)
		max_lights = 1024;

	out->lights = (rtlight_t *) Hunk_AllocName ((size_t)max_lights * sizeof(rtlight_t), "rtlights");
	out->capacity = max_lights;

	line_start = data;
	while (*line_start)
	{
		rtlight_t light;

		line_end = strchr (line_start, '\n');
		if (line_end)
			*line_end = '\0';

		if (RTLight_ParseLine (line_start, &light, line_index + 1, out))
		{
			if (parsed >= out->capacity)
			{
				RTLight_Warnf (out, "RTLight_Parse: exceeded max lights (%d), skipping remainder\n", out->capacity);
				break;
			}
			out->lights[parsed++] = light;
		}

		line_index++;
		if (!line_end)
			break;
		line_start = line_end + 1;
	}

	out->count = parsed;

	Con_Printf ("Loaded %d rtlights from %s (skipped %d, warnings %d)\n",
		out->count, filename, out->skipped_lines, out->warnings);

	free (data);
}
