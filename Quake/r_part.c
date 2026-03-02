/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "r_part_q3p.h"

#define MAX_PARTICLES			16384	// default max # of particles at one
										//  time
#define ABSOLUTE_MIN_PARTICLES	512		// no fewer than this no matter what's
										//  on the command line

static int	ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
static int	ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
static int	ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

particle_t	*particles;
int			r_numparticles, r_numactiveparticles;

static float uvscale;
static float texturescalefactor; //johnfitz -- compensate for apparent size of different particle textures

cvar_t	r_particles = {"r_particles","2", CVAR_ARCHIVE}; //johnfitz
cvar_t	r_particles_mode = {"r_particles_mode", "glquake", CVAR_ARCHIVE};
cvar_t	r_particles_max = {"r_particles_max", "8192", CVAR_ARCHIVE};
cvar_t	r_particles_sort = {"r_particles_sort", "0", CVAR_ARCHIVE};
cvar_t	r_particles_cull_dist = {"r_particles_cull_dist", "4096", CVAR_ARCHIVE};
cvar_t	r_particles_collision = {"r_particles_collision", "1", CVAR_ARCHIVE};
cvar_t	r_particles_spawn_max = { "r_particles_spawn_max", "2048", CVAR_ARCHIVE};

typedef enum
{
	PARTICLEMODE_CLASSIC,
	PARTICLEMODE_GLQUAKE,
	PARTICLEMODE_Q3P,
	PARTICLEMODE_HYBRID
} particlemode_t;

static particlemode_t R_GetParticleMode (void)
{
	if (!q_strcasecmp (r_particles_mode.string, "classic"))
		return PARTICLEMODE_CLASSIC;
	if (!q_strcasecmp (r_particles_mode.string, "q3p"))
		return PARTICLEMODE_Q3P;
	if (!q_strcasecmp (r_particles_mode.string, "hybrid"))
		return PARTICLEMODE_HYBRID;
	return PARTICLEMODE_GLQUAKE;
}

static void R_ParticlesMode_Changed_f (cvar_t *var)
{
	if (q_strcasecmp (var->string, "classic")
		&& q_strcasecmp (var->string, "glquake")
		&& q_strcasecmp (var->string, "q3p")
		&& q_strcasecmp (var->string, "hybrid"))
	{
		Con_Printf ("Unknown r_particles_mode '%s', reverting to glquake\n", var->string);
		Cvar_SetQuick (var, "glquake");
	}
}

static void R_Q3P_TestSpawn_f (void)
{
	int i;
	int count = 64;
	entity_t *viewent;
	vec3_t right, up;

	if (Cmd_Argc () > 1)
		count = q_max (1, Q_atoi (Cmd_Argv (1)));

	viewent = &cl_entities[cl.viewentity];
	AngleVectors (cl.viewangles, NULL, right, up);

	for (i = 0; i < count; ++i)
	{
		q3p_particle_t particle;
		float jitter;

		memset (&particle, 0, sizeof(particle));
		particle.spawn_time = cl.time;
		particle.lifetime = 1.0f + (rand() & 127) / 255.0f;
		particle.size = 1.0f;
		particle.size_ramp = 24.0f;
		particle.alpha = 1.0f;
		particle.alpha_ramp = -0.8f;
		particle.color = 0x6f;
		particle.gravity = 300.0f;
		particle.drag = 0.5f;
		particle.restitution = 0.35f;
		particle.min_bounce_speed = 30.0f;
		particle.flags = Q3P_PARTICLE_COLLIDE_WORLD;
		q_strlcpy (particle.material, "*particle", sizeof(particle.material));

		VectorCopy (viewent->origin, particle.org);
		particle.org[2] += 20.0f;

		jitter = ((rand() % 100) - 50) * 0.5f;
		VectorScale (right, jitter, particle.vel);
		jitter = ((rand() % 100) - 50) * 0.5f;
		VectorMA (particle.vel, jitter, up, particle.vel);
		particle.vel[2] += 120.0f + (rand() % 120);

		if (!Q3P_Spawn (&particle))
			break;
	}

	Con_Printf ("q3p_spawned %d particles (%d active)\n", i, Q3P_ActiveCount ());
}


static void R_Q3P_DebugStats_f (void)
{
	q3p_debug_stats_t stats;

	Q3P_GetDebugStats (&stats);
	Con_Printf ("q3p: active=%d spawned=%d dropped=%d culled=%d\n",
		stats.active, stats.spawned, stats.dropped, stats.culled);
}

static void R_Q3P_ResetStats_f (void)
{
	Q3P_ResetDebugStats ();
	Con_Printf ("q3p: debug stats reset\n");
}

static void R_Q3P_AddWorldEmitter_f (void)
{
	q3p_emitter_t e;

	if (Cmd_Argc () < 4)
	{
		Con_Printf ("usage: q3p_emitter_add <x> <y> <z> [rate] [spread] [material] [color] [lifetime] [cull_dist]\n");
		return;
	}

	memset (&e, 0, sizeof (e));
	e.org[0] = Q_atof (Cmd_Argv (1));
	e.org[1] = Q_atof (Cmd_Argv (2));
	e.org[2] = Q_atof (Cmd_Argv (3));
	e.rate = (Cmd_Argc () > 4) ? q_max (0.f, Q_atof (Cmd_Argv (4))) : 48.f;
	e.spread = (Cmd_Argc () > 5) ? q_max (0.f, Q_atof (Cmd_Argv (5))) : 128.f;
	q_strlcpy (e.material, (Cmd_Argc () > 6) ? Cmd_Argv (6) : "fog_dust", sizeof (e.material));
	e.color = (Cmd_Argc () > 7) ? Q_atoi (Cmd_Argv (7)) : 0x6f;
	e.lifetime = (Cmd_Argc () > 8) ? q_max (0.1f, Q_atof (Cmd_Argv (8))) : 3.5f;
	e.cull_dist = (Cmd_Argc () > 9) ? q_max (0.f, Q_atof (Cmd_Argv (9))) : q_max (r_particles_cull_dist.value, 1024.f);
	e.size = 14.f;
	e.alpha = 0.45f;
	e.gravity = -2.f;
	e.drag = 0.2f;

	if (Q3P_AddWorldEmitter (&e) < 0)
	{
		Con_Warning ("q3p: failed to add world emitter (limit reached)\n");
		return;
	}

	Con_Printf ("q3p: world emitter added at %.1f %.1f %.1f\n", e.org[0], e.org[1], e.org[2]);
}

static qboolean R_CanUseQ3PForLegacyParticleEffect (void)
{
	particlemode_t mode = R_GetParticleMode ();

	return mode == PARTICLEMODE_Q3P || mode == PARTICLEMODE_HYBRID;
}

static qboolean R_TrySpawnQ3PLegacyParticleEffect (const vec3_t org, const vec3_t dir, int color, int count)
{
	int i;
	const qboolean rocket_explosion = (count == 1024);
	const char *material = rocket_explosion ? "explosion" : "bullet";
	const float lifetime_min = rocket_explosion ? 0.45f : 0.10f;
	const float lifetime_rand = rocket_explosion ? 0.20f : 0.40f;
	const float size = rocket_explosion ? 8.0f : 1.25f;
	const float size_rand = rocket_explosion ? 4.0f : 0.75f;
	const float alpha_ramp = rocket_explosion ? -2.0f : -3.0f;
	const float gravity = rocket_explosion ? 100.f : 300.f;
	const float drag = rocket_explosion ? 0.5f : 0.2f;

	if (!R_CanUseQ3PForLegacyParticleEffect ())
		return false;

	for (i = 0; i < count; ++i)
	{
		q3p_particle_t p;

		memset (&p, 0, sizeof(p));
		p.spawn_time = cl.time;
		p.lifetime = lifetime_min + ((rand() & 255) / 255.0f) * lifetime_rand;
		p.size = size + ((rand() & 255) / 255.0f) * size_rand;
		p.size_ramp = p.size * 8.0f;
		p.alpha = 1.0f;
		p.alpha_ramp = alpha_ramp;
		p.color = rocket_explosion ? ramp1[0] : color;
		p.gravity = gravity;
		p.drag = drag;
		p.restitution = rocket_explosion ? 0.30f : 0.25f;
		p.min_bounce_speed = rocket_explosion ? 25.f : 40.f;
		p.flags = Q3P_PARTICLE_COLLIDE_WORLD;

		q_strlcpy (p.material, material, sizeof(p.material));

		if (rocket_explosion)
		{
			p.org[0] = org[0] + ((rand() % 32) - 16);
			p.org[1] = org[1] + ((rand() % 32) - 16);
			p.org[2] = org[2] + ((rand() % 32) - 16);

			p.vel[0] = (float)((rand() % 512) - 256);
			p.vel[1] = (float)((rand() % 512) - 256);
			p.vel[2] = (float)((rand() % 512) - 256);
		}
		else
		{
			p.org[0] = org[0] + ((rand() & 15) - 8);
			p.org[1] = org[1] + ((rand() & 15) - 8);
			p.org[2] = org[2] + ((rand() & 15) - 8);

			p.vel[0] = dir[0] * 15.f;
			p.vel[1] = dir[1] * 15.f;
			p.vel[2] = dir[2] * 15.f;
		}

		if (!Q3P_Spawn (&p))
			return true;
	}

	return true;
}

typedef struct particlevert_t {
	vec3_t		pos;
	GLubyte		color[4];
} particlevert_t;

static particlevert_t partverts[MAX_PARTICLES];
static int numpartverts = 0;

/*
===============
R_SetParticleTexture_f -- johnfitz
===============
*/
static void R_SetParticleTexture_f (cvar_t *var)
{
	switch ((int)(r_particles.value))
	{
	case 1:
		uvscale = 1;
		texturescalefactor = 1.27;
		break;
	case 2:
		uvscale = 0.25;
		texturescalefactor = 1.0;
		break;
	}
}

/*
===============
R_AllocParticle
===============
*/
particle_t *R_AllocParticle (void)
{
	if (r_numactiveparticles < r_numparticles)
	{
		particle_t *p = &particles[r_numactiveparticles++];
		p->spawn = cl.time - 0.001;
		return p;
	}
	return NULL;
}

/*
===============
R_InitParticles
===============
*/
void R_InitParticles (void)
{
	int		i;

	i = COM_CheckParm ("-particles");

	if (i && i < com_argc - 1)
	{
		r_numparticles = atoi(com_argv[i + 1]);
		if (r_numparticles < ABSOLUTE_MIN_PARTICLES)
			r_numparticles = ABSOLUTE_MIN_PARTICLES;
	}
	else
	{
		r_numparticles = MAX_PARTICLES;
	}

	particles = (particle_t *)
			Hunk_AllocName (r_numparticles * sizeof(particle_t), "particles");
	r_numactiveparticles = 0;

	Cvar_RegisterVariable (&r_particles); //johnfitz
	Cvar_SetCallback (&r_particles, R_SetParticleTexture_f);
	R_SetParticleTexture_f (&r_particles); // set default
	Cvar_RegisterVariable (&r_particles_mode);
	Cvar_SetCallback (&r_particles_mode, R_ParticlesMode_Changed_f);
	R_ParticlesMode_Changed_f (&r_particles_mode);
	Cvar_RegisterVariable (&r_particles_max);
	Cvar_RegisterVariable (&r_particles_sort);
	Cvar_RegisterVariable (&r_particles_cull_dist);
	Cvar_RegisterVariable (&r_particles_collision);
	Cvar_RegisterVariable (&r_particles_spawn_max);

	Q3P_Init ();
	Cmd_AddCommand ("q3p_spawn", R_Q3P_TestSpawn_f);
	Cmd_AddCommand ("q3p_stats", R_Q3P_DebugStats_f);
	Cmd_AddCommand ("q3p_stats_reset", R_Q3P_ResetStats_f);
	Cmd_AddCommand ("q3p_emitter_add", R_Q3P_AddWorldEmitter_f);
	Cmd_AddCommand ("q3p_emitter_clear", Q3P_ClearWorldEmitters);
}

/*
===============
R_EntityParticles
===============
*/
static vec3_t	avelocities[NUMVERTEXNORMALS];
static float	beamlength = 16;

void R_EntityParticles (entity_t *ent)
{
	int		i;
	particle_t	*p;
	float		angle;
	float		sp, sy, cp, cy;
//	float		sr, cr;
//	int		count;
	vec3_t		forward;
	float		dist;

	dist = 64;
//	count = 50;

	if (!avelocities[0][0])
	{
		for (i = 0; i < NUMVERTEXNORMALS; i++)
		{
			avelocities[i][0] = (rand() & 255) * 0.01;
			avelocities[i][1] = (rand() & 255) * 0.01;
			avelocities[i][2] = (rand() & 255) * 0.01;
		}
	}

	for (i = 0; i < NUMVERTEXNORMALS; i++)
	{
		angle = cl.time * avelocities[i][0];
		sy = sin(angle);
		cy = cos(angle);
		angle = cl.time * avelocities[i][1];
		sp = sin(angle);
		cp = cos(angle);
		angle = cl.time * avelocities[i][2];
	//	sr = sin(angle);
	//	cr = cos(angle);

		forward[0] = cp*cy;
		forward[1] = cp*sy;
		forward[2] = -sp;

		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 0.01;
		p->color = 0x6f;
		p->type = pt_explode;

		p->org[0] = ent->origin[0] + r_avertexnormals[i][0]*dist + forward[0]*beamlength;
		p->org[1] = ent->origin[1] + r_avertexnormals[i][1]*dist + forward[1]*beamlength;
		p->org[2] = ent->origin[2] + r_avertexnormals[i][2]*dist + forward[2]*beamlength;
	}
}

/*
===============
R_ClearParticles
===============
*/
void R_ClearParticles (void)
{
	r_numactiveparticles = 0;
	Q3P_Clear ();
	Q3P_ClearWorldEmitters ();
}

/*
===============
R_ParseParticleEffect

Parse an effect out of the server message
===============
*/
void R_ParseParticleEffect (void)
{
	vec3_t		org, dir;
	int			i, count, msgcount, color;

	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord (cl.protocolflags);
	for (i=0 ; i<3 ; i++)
		dir[i] = MSG_ReadChar () * (1.0/16);
	msgcount = MSG_ReadByte ();
	color = MSG_ReadByte ();

	if (msgcount == 255)
		count = 1024;
	else
		count = msgcount;

	if (R_TrySpawnQ3PLegacyParticleEffect (org, dir, color, count))
		return;

	R_RunParticleEffect (org, dir, color, count);
}

/*
===============
R_ParticleExplosion
===============
*/
void R_ParticleExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<1024 ; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 5;
		p->color = ramp1[0];
		p->ramp = rand()&3;
		if (i & 1)
		{
			p->type = pt_explode;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_explode2;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_ParticleExplosion2
===============
*/
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength)
{
	int			i, j;
	particle_t	*p;
	int			colorMod = 0;

	for (i=0; i<512; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 0.3;
		p->color = colorStart + (colorMod % colorLength);
		colorMod++;

		p->type = pt_blob;
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + ((rand()%32)-16);
			p->vel[j] = (rand()%512)-256;
		}
	}
}

/*
===============
R_BlobExplosion
===============
*/
void R_BlobExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<1024 ; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 1 + (rand()&8)*0.05;

		if (i & 1)
		{
			p->type = pt_blob;
			p->color = 66 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_blob2;
			p->color = 150 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_RunParticleEffect
===============
*/
void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<count ; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		if (count == 1024)
		{	// rocket explosion
			p->die = cl.time + 5;
			p->color = ramp1[0];
			p->ramp = rand()&3;
			if (i & 1)
			{
				p->type = pt_explode;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
			else
			{
				p->type = pt_explode2;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
		}
		else
		{
			p->die = cl.time + 0.1*(rand()%5);
			p->color = (color&~7) + (rand()&7);
			p->type = pt_slowgrav;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()&15)-8);
				p->vel[j] = dir[j]*15;// + (rand()%300)-150;
			}
		}
	}
}

/*
===============
R_LavaSplash
===============
*/
void R_LavaSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i++)
		for (j=-16 ; j<16 ; j++)
			for (k=0 ; k<1 ; k++)
			{
				if (!(p = R_AllocParticle ()))
					return;

				p->die = cl.time + 2 + (rand()&31) * 0.02;
				p->color = 224 + (rand()&7);
				p->type = pt_slowgrav;

				dir[0] = j*8 + (rand()&7);
				dir[1] = i*8 + (rand()&7);
				dir[2] = 256;

				p->org[0] = org[0] + dir[0];
				p->org[1] = org[1] + dir[1];
				p->org[2] = org[2] + (rand()&63);

				VectorNormalize (dir);
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_TeleportSplash
===============
*/
void R_TeleportSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i+=4)
	{
		for (j=-16 ; j<16 ; j+=4)
		{
			for (k=-24 ; k<32 ; k+=4)
			{
				if (!(p = R_AllocParticle ()))
					return;

				p->die = cl.time + 0.2 + (rand()&7) * 0.02;
				p->color = 7 + (rand()&7);
				p->type = pt_slowgrav;

				dir[0] = j*8;
				dir[1] = i*8;
				dir[2] = k*8;

				p->org[0] = org[0] + i + (rand()&3);
				p->org[1] = org[1] + j + (rand()&3);
				p->org[2] = org[2] + k + (rand()&3);

				VectorNormalize (dir);
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
		}
	}
}

/*
===============
R_RocketTrail

FIXME -- rename function and use #defined types instead of numbers
===============
*/
void R_RocketTrail (vec3_t start, vec3_t end, int type)
{
	vec3_t		vec;
	float		len;
	int			j;
	particle_t	*p;
	int			dec;
	static int	tracercount;
	qboolean	use_q3p;
	const char	*trail_material = NULL;
	float		trail_lifetime = 0.f;
	float		trail_size = 0.f;
	float		trail_size_ramp = 0.f;
	float		trail_alpha_ramp = 0.f;
	float		trail_gravity = 0.f;
	float		trail_drag = 0.f;
	int			trail_color = 0;

	VectorSubtract (end, start, vec);
	len = VectorNormalize (vec);
	if (type < 128)
		dec = 3;
	else
	{
		dec = 1;
		type -= 128;
	}

	use_q3p = R_CanUseQ3PForLegacyParticleEffect ();
	if (use_q3p)
	{
		switch (type)
		{
		case 0:
			trail_material = "smoke";
			trail_lifetime = 0.65f;
			trail_size = 2.8f;
			trail_size_ramp = 10.0f;
			trail_alpha_ramp = -1.35f;
			trail_drag = 0.25f;
			trail_color = ramp3[0];
			break;
		case 1:
			trail_material = "smoke";
			trail_lifetime = 0.85f;
			trail_size = 2.5f;
			trail_size_ramp = 9.5f;
			trail_alpha_ramp = -1.1f;
			trail_drag = 0.35f;
			trail_color = ramp3[2];
			break;
		case 2:
			trail_material = "blood_heavy";
			trail_lifetime = 0.45f;
			trail_size = 1.35f;
			trail_size_ramp = 1.5f;
			trail_alpha_ramp = -2.3f;
			trail_gravity = 240.f;
			trail_drag = 0.05f;
			trail_color = 67;
			break;
		case 4:
			trail_material = "blood_light";
			trail_lifetime = 0.3f;
			trail_size = 1.1f;
			trail_size_ramp = 1.0f;
			trail_alpha_ramp = -2.8f;
			trail_gravity = 220.f;
			trail_drag = 0.05f;
			trail_color = 67;
			break;
		case 6:
			trail_material = "voor";
			trail_lifetime = 0.25f;
			trail_size = 2.0f;
			trail_size_ramp = 2.0f;
			trail_alpha_ramp = -3.0f;
			trail_drag = 0.05f;
			trail_color = 9*16 + 9;
			break;
		default:
			break;
		}
	}

	while (len > 0)
	{
		qboolean spawned_q3p = false;
		len -= dec;

		if (trail_material)
		{
			q3p_particle_t qp;
			memset (&qp, 0, sizeof(qp));
			qp.spawn_time = cl.time;
			qp.lifetime = trail_lifetime;
			qp.size = trail_size + ((rand() & 7) - 3) * 0.05f;
			qp.size_ramp = trail_size_ramp;
			qp.alpha = 1.0f;
			qp.alpha_ramp = trail_alpha_ramp;
			qp.color = trail_color;
			qp.gravity = trail_gravity;
			qp.drag = trail_drag;
			if (type == 2 || type == 4)
			{
				qp.restitution = 0.2f;
				qp.min_bounce_speed = 20.f;
				qp.flags = Q3P_PARTICLE_COLLIDE_WORLD;
			}
			q_strlcpy (qp.material, trail_material, sizeof(qp.material));
			for (j = 0; j < 3; ++j)
				qp.org[j] = start[j] + ((rand() & 7) - 3);
			if (Q3P_Spawn (&qp))
				spawned_q3p = true;
		}

		if (!spawned_q3p)
		{
			if (!(p = R_AllocParticle ()))
				return;

			VectorCopy (vec3_origin, p->vel);
			p->die = cl.time + 2;

			switch (type)
			{
				case 0:	// rocket trail
					p->ramp = (rand()&3);
					p->color = ramp3[(int)p->ramp];
					p->type = pt_fire;
					for (j=0 ; j<3 ; j++)
						p->org[j] = start[j] + ((rand()%6)-3);
					break;

				case 1:	// smoke smoke
					p->ramp = (rand()&3) + 2;
					p->color = ramp3[(int)p->ramp];
					p->type = pt_fire;
					for (j=0 ; j<3 ; j++)
						p->org[j] = start[j] + ((rand()%6)-3);
					break;

				case 2:	// blood
					p->type = pt_grav;
					p->color = 67 + (rand()&3);
					for (j=0 ; j<3 ; j++)
						p->org[j] = start[j] + ((rand()%6)-3);
					break;

				case 3:
				case 5:	// tracer
					p->die = cl.time + 0.5;
					p->type = pt_static;
					if (type == 3)
						p->color = 52 + ((tracercount&4)<<1);
					else
						p->color = 230 + ((tracercount&4)<<1);

					tracercount++;

					VectorCopy (start, p->org);
					if (tracercount & 1)
					{
						p->vel[0] = 30*vec[1];
						p->vel[1] = 30*-vec[0];
					}
					else
					{
						p->vel[0] = 30*-vec[1];
						p->vel[1] = 30*vec[0];
					}
					break;

				case 4:	// slight blood
					p->type = pt_grav;
					p->color = 67 + (rand()&3);
					for (j=0 ; j<3 ; j++)
						p->org[j] = start[j] + ((rand()%6)-3);
					len -= 3;
					break;

				case 6:	// voor trail
					p->color = 9*16 + 8 + (rand()&3);
					p->type = pt_static;
					p->die = cl.time + 0.3;
					for (j=0 ; j<3 ; j++)
						p->org[j] = start[j] + ((rand()&15)-8);
					break;
			}
		}

		VectorAdd (start, vec, start);
	}
}

/*
===============
CL_RunParticles -- johnfitz -- all the particle behavior, separated from R_DrawParticles
===============
*/
void CL_RunParticles (void)
{
	particle_t		*p;
	int				i, cur, active;
	float			time1, time2, time3, dvel, frametime, grav;
	extern	cvar_t	sv_gravity;

	frametime = cl.time - cl.oldtime;

	if (R_GetParticleMode () != PARTICLEMODE_CLASSIC && R_GetParticleMode () != PARTICLEMODE_GLQUAKE)
		Q3P_Update (frametime);

	if (R_GetParticleMode () == PARTICLEMODE_Q3P)
		return;

	time3 = frametime * 15;
	time2 = frametime * 10;
	time1 = frametime * 5;
	grav = frametime * sv_gravity.value * 0.05;
	dvel = 4*frametime;

	for (cur = active = 0, p = particles; cur < r_numactiveparticles; cur++, p++)
	{
		if (p->die < cl.time || p->spawn > cl.time)
			continue;

		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;

		switch (p->type)
		{
		case pt_static:
			break;
		case pt_fire:
			p->ramp += time1;
			if (p->ramp >= 6)
				p->die = -1;
			else
				p->color = ramp3[(int)p->ramp];
			p->vel[2] += grav;
			break;

		case pt_explode:
			p->ramp += time2;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp1[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_explode2:
			p->ramp += time3;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp2[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] -= p->vel[i]*frametime;
			p->vel[2] -= grav;
			break;

		case pt_blob:
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_blob2:
			for (i=0 ; i<2 ; i++)
				p->vel[i] -= p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_grav:
		case pt_slowgrav:
			p->vel[2] -= grav;
			break;
		}

		if (cur != active)
			particles[active] = *p;
		active++;
	}

	r_numactiveparticles = active;
}

/*
===============
R_FlushParticleBatch
===============
*/
static void R_FlushParticleBatch (void)
{
	GLuint buf;
	GLbyte *ofs;

	if (!numpartverts)
		return;

	GL_Upload (GL_ARRAY_BUFFER, partverts, sizeof(partverts[0]) * numpartverts, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(partverts[0]), ofs + offsetof(particlevert_t, pos));
	GL_VertexAttribPointerFunc (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(partverts[0]), ofs + offsetof(particlevert_t, color));

	GL_DrawArraysInstancedFunc (GL_TRIANGLE_STRIP, 0, 4, numpartverts);

	numpartverts = 0;
}

/*
===============
R_DrawParticles_Real -- johnfitz -- moved all non-drawing code to CL_RunParticles
===============
*/
static void R_DrawParticles_Real (qboolean alpha, qboolean showtris)
{
	particle_t		*p;
	particlevert_t	*v;
	GLubyte			color[4] = {255, 255, 255, 255}, *c; //johnfitz -- particle transparency
	extern	cvar_t	r_particles; //johnfitz
	//float			alpha; //johnfitz -- particle transparency
	float			scalex, scaley;
	qboolean		dither, oit;
	int				i;

	if (R_GetParticleMode () == PARTICLEMODE_Q3P || R_GetParticleMode () == PARTICLEMODE_HYBRID)
		Q3P_Draw (alpha, showtris);

	if (R_GetParticleMode () == PARTICLEMODE_Q3P)
		return;

	if (!r_particles.value)
		return;

	if (!r_numactiveparticles)
		return;

	// square particles are drawn opaque (avoiding alpha sorting issues)
	if (!showtris && alpha != ((int)r_particles.value != 2))
		return;

	GL_BeginGroup ("Particles");

	dither = (softemu == SOFTEMU_COARSE && !showtris);
	oit = (alpha && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT);
	GL_UseProgram (glprogs.particles[oit][dither]);

	// compensate for apparent size of different particle textures
	// this bakes in the additional scaling of vup and vright by 1.5f for billboarding,
	// then down by 0.25f for quad particles
	scalex = scaley = texturescalefactor * 0.375f;
	// projection factors (see GL_FrustumMatrix), negated to make things easier in the shader
	scalex *=  r_matproj[1*4 + 0]; // -1 / tan (fovx/2)
	scaley *= -r_matproj[2*4 + 1]; // -1 / tan (fovy/2)
	GL_Uniform3fFunc (0, scalex, scaley, uvscale);

	if (alpha)
		GL_SetState (GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));
	else
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));

	numpartverts = 0;
	for (i = 0, p = particles; i < r_numactiveparticles; i++, p++)
	{
		if (numpartverts == countof(partverts))
			R_FlushParticleBatch ();

		v = &partverts[numpartverts++];
		VectorCopy (p->org, v->pos);

		//johnfitz -- particle transparency and fade out
		c = showtris ? color : (GLubyte *) &d_8to24table[(int)p->color];
		*(uint32_t*)&v->color = *(uint32_t*)c;
		v->color[0] = c[0];
		v->color[1] = c[1];
		v->color[2] = c[2];
		//alpha = CLAMP(0, p->die + 0.5 - cl.time, 1);
		v->color[3] = c[3]; //(int)(alpha * 255);
		//johnfitz
	}

	R_FlushParticleBatch ();

	GL_EndGroup ();
}

/*
===============
R_DrawParticles -- johnfitz -- moved all non-drawing code to CL_RunParticles
===============
*/
void R_DrawParticles (qboolean alpha)
{
	R_DrawParticles_Real (alpha, false);
}
/*
===============
R_DrawParticles_ShowTris -- johnfitz
===============
*/
void R_DrawParticles_ShowTris (void)
{
	R_DrawParticles_Real (false, true);
}
