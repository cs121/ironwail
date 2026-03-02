#ifndef _QUAKE_R_PARTICLES_IW_H
#define _QUAKE_R_PARTICLES_IW_H

#include "q_stdinc.h"
#include "mathlib.h"
#include "cvar.h"

typedef enum particle_mode_e
{
	PARTICLE_MODE_CLASSIC = 0,
	PARTICLE_MODE_Q3P,
	PARTICLE_MODE_HYBRID
} particle_mode_t;

typedef struct particle_def_s
{
	char	material[64];
	float	lifetime;
	float	size_start;
	float	size_end;
	vec4_t	color_start;
	vec4_t	color_end;
	float	gravity;
	float	drag;
	int	collision_flags;
	int	lighting_flags;
} particle_def_t;

typedef struct emitter_def_s
{
	float	rate;
	int	burst;
	qboolean triggered;
	qboolean world_space;
	int	attach_entity;
	int	attach_bone;
} emitter_def_t;

typedef struct particle_spawn_context_s
{
	vec3_t	origin;
	vec3_t	direction;
	int	entity;
	double	time;
} particle_spawn_context_t;

typedef struct particle_instance_s
{
	const particle_def_t	*def;
	particle_spawn_context_t	spawn;
	vec3_t	velocity;
	float	age;
	qboolean active;
} particle_instance_t;

typedef struct emitter_instance_s
{
	const emitter_def_t	*def;
	int	entity;
	double	next_emit;
	qboolean active;
} emitter_instance_t;

extern cvar_t r_particles_mode;
extern cvar_t r_particles_max;
extern cvar_t r_particles_sort;

void R_IWParticles_Init (void);
void R_IWParticles_Shutdown (void);
void R_IWParticles_NewMap (void);
void R_IWParticles_Clear (void);
void R_IWParticles_Draw (void);

particle_mode_t R_ParticlesMode (void);
qboolean R_ParticlesUseClassic (void);

#endif /* _QUAKE_R_PARTICLES_IW_H */
