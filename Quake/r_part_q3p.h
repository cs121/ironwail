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
	int		flags;
	char	material[64];
} q3p_particle_t;

typedef struct q3p_emitter_s {
	float	rate;
	int		burst;
	qboolean triggered;
	int		entity_num;
	qboolean local_space;
} q3p_emitter_t;

void Q3P_Init (void);
void Q3P_Shutdown (void);
void Q3P_Clear (void);
qboolean Q3P_Spawn (const q3p_particle_t *particle);
void Q3P_Update (float frametime);
void Q3P_Draw (qboolean alpha, qboolean showtris);
int Q3P_ActiveCount (void);

#endif
