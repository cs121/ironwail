#ifndef R_PART_Q3P_H
#define R_PART_Q3P_H

#include "quakedef.h"

typedef struct q3p_particle_s {
	float	spawn_time;
	float	lifetime;
	float	size;
	float	size_ramp;
	float	alpha;
	float	alpha_ramp;
	int		color;
	vec3_t	org;
	vec3_t	vel;
	float	gravity;
	float	drag;
	float	restitution;
	float	min_bounce_speed;
	int		flags;
	char	material[64];
	unsigned material_id;
} q3p_particle_t;

enum
{
	Q3P_PARTICLE_COLLIDE_WORLD = 1 << 0
};

typedef struct q3p_emitter_s {
	qboolean active;
	vec3_t	org;
	float	spread;
	float	cull_dist;
	float	time_accum;
	float	lifetime;
	float	size;
	float	alpha;
	float	gravity;
	float	drag;
	int		color;
	char	material[64];
	float	rate;
	int		burst;
} q3p_emitter_t;

typedef struct q3p_debug_stats_s {
	int active;
	int spawned;
	int dropped;
	int culled;
} q3p_debug_stats_t;

typedef struct q3p_effectdef_s {
	qboolean loaded;
	int		count;
	float	lifetime_min;
	float	lifetime_rand;
	float	size;
	float	size_rand;
	float	size_ramp;
	float	alpha;
	float	alpha_ramp;
	int		color;
	float	gravity;
	float	drag;
	float	restitution;
	float	min_bounce_speed;
	float	org_jitter;
	float	vel_jitter;
	float	vel_scale;
	qboolean collide_world;
	char	material[64];
} q3p_effectdef_t;

void Q3P_Init (void);
void Q3P_Shutdown (void);
void Q3P_Clear (void);
qboolean Q3P_Spawn (const q3p_particle_t *particle);
void Q3P_Update (float frametime);
void Q3P_Draw (qboolean alpha, qboolean showtris);
int Q3P_ActiveCount (void);
void Q3P_GetDebugStats (q3p_debug_stats_t *stats);
void Q3P_ResetDebugStats (void);
int Q3P_AddWorldEmitter (const q3p_emitter_t *emitter);
void Q3P_ClearWorldEmitters (void);
qboolean Q3P_GetEffectDef (const char *name, q3p_effectdef_t *out_def);
qboolean Q3P_DebugStressMaterialCache (int iterations, int unique_names, int *before_out, int *after_out);

#endif
