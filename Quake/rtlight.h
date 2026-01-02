#ifndef __RTLIGHT_H
#define __RTLIGHT_H

#include "quakedef.h"

typedef enum
{
	RTL_POINT = 0,
	RTL_SPOT = 1
} rtlight_type_t;

typedef struct rtlight_s
{
	int id;
	vec3_t origin;
	float radius;

	vec3_t color;      // linear
	float intensity;   // multiplier

	int style;
	rtlight_type_t type;
	vec3_t dir;
	float angle_deg;
	float soft;

	qboolean casts_shadows;

	// corona
	qboolean corona;
	float corona_size;
	char corona_tex[MAX_QPATH];
	float corona_alpha;

	// envmap
	qboolean envmap;
	char envmap_tex[MAX_QPATH];
	float envmap_strength;

	char group[64];
	char name[64];
} rtlight_t;

typedef struct rtlight_list_s
{
	rtlight_t *lights;
	int count;
	int capacity;
	int skipped_lines;
	int warnings;
} rtlight_list_t;

extern cvar_t r_rtlights;
extern cvar_t r_rtlights_max;
extern cvar_t r_rtlights_debug;
extern cvar_t r_coronas;
extern cvar_t r_coronas_debug;
extern cvar_t r_envmap_lights;
extern cvar_t r_envmap_lights_debug;

void RTLight_Clear (rtlight_list_t *list);
void RTLight_LoadForMap (const char *mapname, rtlight_list_t *out);
const rtlight_list_t *RTLight_GetActive (void);

#endif
