#include "quakedef.h"
#include "r_part_q3p.h"

extern cvar_t r_particles_max;

static q3p_particle_t *q3p_particles;
static int q3p_maxparticles;
static int q3p_numactive;

void Q3P_Init (void)
{
	if (q3p_particles)
		return;

	q3p_maxparticles = q_max ((int)r_particles_max.value, 0);
	if (q3p_maxparticles <= 0)
		q3p_maxparticles = 1;

	q3p_particles = (q3p_particle_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_particles), "q3particles");
	q3p_numactive = 0;
}

void Q3P_Shutdown (void)
{
	q3p_particles = NULL;
	q3p_maxparticles = 0;
	q3p_numactive = 0;
}

void Q3P_Clear (void)
{
	q3p_numactive = 0;
}

qboolean Q3P_Spawn (const q3p_particle_t *particle)
{
	if (!particle)
		return false;
	if (!q3p_particles)
		Q3P_Init ();
	if (q3p_numactive >= q3p_maxparticles)
		return false;

	q3p_particles[q3p_numactive++] = *particle;
	return true;
}

void Q3P_Update (float frametime)
{
	int i, active;
	q3p_particle_t *p;

	if (!q3p_particles || q3p_numactive <= 0)
		return;

	for (i = active = 0, p = q3p_particles; i < q3p_numactive; ++i, ++p)
	{
		float age = cl.time - p->spawn_time;

		if (age < 0 || age >= p->lifetime)
			continue;

		p->size += p->size_ramp * frametime;
		p->alpha += p->alpha_ramp * frametime;
		p->alpha = CLAMP (0, p->alpha, 1);

		VectorMA (p->org, frametime, p->vel, p->org);
		p->vel[2] -= p->gravity * frametime;
		VectorScale (p->vel, q_max (0.f, 1.f - p->drag * frametime), p->vel);

		if (i != active)
			q3p_particles[active] = *p;
		++active;
	}

	q3p_numactive = active;
}

void Q3P_Draw (qboolean alpha, qboolean showtris)
{
	(void)alpha;
	(void)showtris;
	/* Placeholder for future renderer integration. */
}

int Q3P_ActiveCount (void)
{
	return q3p_numactive;
}
