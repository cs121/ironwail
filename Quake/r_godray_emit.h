#ifndef R_GODRAY_EMIT_H
#define R_GODRAY_EMIT_H

#define MAX_GODRAY_EMITTERS 256
#define MAX_GODRAY_POSTFX_EMITTERS 8

typedef struct godray_emitter_s
{
	vec3_t center_ws;
	vec3_t normal_ws;
	vec3_t ray_dir_ws;
	float ray_length;
	float density;
	float intensity;
	vec3_t color;
	unsigned int flags;
	vec3_t axis_t;
	vec3_t axis_b;
	vec3_t axis_r;
	vec3_t mins;
	vec3_t maxs;
} godray_emitter_t;

typedef struct godray_emitter_stats_s
{
	int found;
	int culled;
	int clamped;
	int total;
	int rendered_volume;
	int rendered_postfx;
} godray_emitter_stats_t;

extern cvar_t r_godrays_mode;
extern cvar_t r_godrays_max_emitters;
extern cvar_t r_godrays_hero_emitters;
extern cvar_t r_godray_volume_length;
extern cvar_t r_godray_volume_density;
extern cvar_t r_godray_volume_intensity;
extern cvar_t r_godray_volume_color;
extern cvar_t r_godray_volume_dir;

void R_GodrayEmitters_Init (void);
void R_GodrayEmitters_BuildList (void);
const godray_emitter_t *R_GodrayEmitters_GetList (int *out_count);
const godray_emitter_stats_t *R_GodrayEmitters_GetStats (void);
void R_GodrayEmitters_GetRenderCounts (int *out_volume_count, int *out_postfx_count);
void R_GodrayEmitters_SetRenderedCounts (int volume_count, int postfx_count);

#endif
