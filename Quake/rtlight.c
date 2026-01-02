#include "quakedef.h"
#include "rtlight.h"

cvar_t r_rtlights = {"r_rtlights", "1", CVAR_ARCHIVE};
cvar_t r_rtlights_max = {"r_rtlights_max", "1024", CVAR_ARCHIVE};
cvar_t r_rtlights_debug = {"r_rtlights_debug", "0"};
cvar_t r_coronas = {"r_coronas", "1", CVAR_ARCHIVE};
cvar_t r_coronas_debug = {"r_coronas_debug", "0"};
cvar_t r_envmap_lights = {"r_envmap_lights", "0", CVAR_ARCHIVE};
cvar_t r_envmap_lights_debug = {"r_envmap_lights_debug", "0"};

static rtlight_list_t r_rtlight_list;

void RTLight_ParseFile (const char *mapname, rtlight_list_t *out);

void RTLight_Clear (rtlight_list_t *list)
{
	if (!list)
		return;

	list->lights = NULL;
	list->count = 0;
	list->capacity = 0;
	list->skipped_lines = 0;
	list->warnings = 0;
}

const rtlight_list_t *RTLight_GetActive (void)
{
	return &r_rtlight_list;
}

static void RTLight_StableSortById (rtlight_t *lights, int count)
{
	int i, j;

	for (i = 1; i < count; i++)
	{
		rtlight_t key = lights[i];
		j = i - 1;
		while (j >= 0 && lights[j].id > key.id)
		{
			lights[j + 1] = lights[j];
			j--;
		}
		lights[j + 1] = key;
	}
}

void RTLight_LoadForMap (const char *mapname, rtlight_list_t *out)
{
	if (!out)
		return;

	RTLight_Clear (out);
	RTLight_Clear (&r_rtlight_list);

	if (!r_rtlights.value)
		return;

	RTLight_ParseFile (mapname, out);

	if (out->count > 1)
		RTLight_StableSortById (out->lights, out->count);

	r_rtlight_list = *out;
}
