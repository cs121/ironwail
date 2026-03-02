#include "quakedef.h"
#include "r_part_q3p.h"

extern cvar_t r_particles_max;
extern cvar_t r_particles_sort;

static q3p_particle_t *q3p_particles;
static int q3p_maxparticles;
static int q3p_numactive;

typedef struct q3p_drawitem_s {
	int particle_index;
	int blend_group;
	unsigned material_id;
	float depth;
} q3p_drawitem_t;

typedef struct q3p_particlevert_s {
	vec3_t		pos;
	GLubyte		color[4];
} q3p_particlevert_t;

static q3p_drawitem_t *q3p_drawitems;
static q3p_particlevert_t *q3p_partverts;
static int q3p_numdrawitems;
static int q3p_numpartverts;

static unsigned Q3P_MaterialId (const char *material)
{
	unsigned id = 2166136261u;

	if (!material || !*material)
		return 0;

	while (*material)
	{
		id ^= (unsigned char)*material++;
		id *= 16777619u;
	}

	return id;
}

static int Q3P_DrawSortCmp (const void *a, const void *b)
{
	const q3p_drawitem_t *da = (const q3p_drawitem_t *)a;
	const q3p_drawitem_t *db = (const q3p_drawitem_t *)b;

	if (da->blend_group != db->blend_group)
		return da->blend_group - db->blend_group;
	if (da->material_id < db->material_id)
		return -1;
	if (da->material_id > db->material_id)
		return 1;
	if (r_particles_sort.value > 0.f)
	{
		if (da->depth < db->depth)
			return 1;
		if (da->depth > db->depth)
			return -1;
	}

	return da->particle_index - db->particle_index;
}

void Q3P_Init (void)
{
	if (q3p_particles)
		return;

	q3p_maxparticles = q_max ((int)r_particles_max.value, 0);
	if (q3p_maxparticles <= 0)
		q3p_maxparticles = 1;

	q3p_particles = (q3p_particle_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_particles), "q3particles");
	q3p_drawitems = (q3p_drawitem_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_drawitems), "q3pdrawitems");
	q3p_partverts = (q3p_particlevert_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_partverts), "q3ppartverts");
	q3p_numactive = 0;
	q3p_numdrawitems = 0;
	q3p_numpartverts = 0;
}

void Q3P_Shutdown (void)
{
	q3p_particles = NULL;
	q3p_drawitems = NULL;
	q3p_partverts = NULL;
	q3p_maxparticles = 0;
	q3p_numactive = 0;
	q3p_numdrawitems = 0;
	q3p_numpartverts = 0;
}

void Q3P_Clear (void)
{
	q3p_numactive = 0;
	q3p_numdrawitems = 0;
	q3p_numpartverts = 0;
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

static void Q3P_FlushParticleBatch (void)
{
	GLuint buf;
	GLbyte *ofs;

	if (!q3p_numpartverts)
		return;

	GL_Upload (GL_ARRAY_BUFFER, q3p_partverts, sizeof(q3p_partverts[0]) * q3p_numpartverts, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(q3p_partverts[0]), ofs + offsetof(q3p_particlevert_t, pos));
	GL_VertexAttribPointerFunc (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(q3p_partverts[0]), ofs + offsetof(q3p_particlevert_t, color));
	GL_DrawArraysInstancedFunc (GL_TRIANGLE_STRIP, 0, 4, q3p_numpartverts);

	q3p_numpartverts = 0;
}

void Q3P_Draw (qboolean alpha, qboolean showtris)
{
	int i;
	float scalex, scaley;
	qboolean dither, oit;
	unsigned saved_state;
	GLubyte white[4] = {255, 255, 255, 255};

	if (!q3p_particles || q3p_numactive <= 0)
		return;

	q3p_numdrawitems = 0;
	for (i = 0; i < q3p_numactive; ++i)
	{
		q3p_particle_t *p = &q3p_particles[i];
		int blend_group = p->alpha < 1.f;

		if (!showtris && alpha != blend_group)
			continue;

		q3p_drawitems[q3p_numdrawitems].particle_index = i;
		q3p_drawitems[q3p_numdrawitems].blend_group = blend_group;
		q3p_drawitems[q3p_numdrawitems].material_id = Q3P_MaterialId (p->material);
		q3p_drawitems[q3p_numdrawitems].depth = DotProduct (r_origin, vpn) - DotProduct (p->org, vpn);
		++q3p_numdrawitems;
	}

	if (!q3p_numdrawitems)
		return;

	qsort (q3p_drawitems, q3p_numdrawitems, sizeof(q3p_drawitems[0]), Q3P_DrawSortCmp);

	GL_BeginGroup ("Q3P Particles");

	dither = (softemu == SOFTEMU_COARSE && !showtris);
	oit = (alpha && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT);
	GL_UseProgram (glprogs.particles[oit][dither]);

	scalex = scaley = 0.375f;
	scalex *=  r_matproj[1*4 + 0];
	scaley *= -r_matproj[2*4 + 1];
	GL_Uniform3fFunc (0, scalex, scaley, 0.25f);

	saved_state = glstate;
	if (alpha)
		GL_SetState (GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));
	else
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));

	q3p_numpartverts = 0;
	for (i = 0; i < q3p_numdrawitems; ++i)
	{
		q3p_particle_t *p = &q3p_particles[q3p_drawitems[i].particle_index];
		q3p_particlevert_t *v;
		const GLubyte *c = showtris ? white : (const GLubyte *)&d_8to24table[p->color & 0xff];

		if (q3p_numpartverts == q3p_maxparticles)
			Q3P_FlushParticleBatch ();

		v = &q3p_partverts[q3p_numpartverts++];
		VectorCopy (p->org, v->pos);
		v->color[0] = c[0];
		v->color[1] = c[1];
		v->color[2] = c[2];
		v->color[3] = (GLubyte)(CLAMP (0.f, p->alpha, 1.f) * 255.f);
	}

	Q3P_FlushParticleBatch ();
	GL_SetState (saved_state);
	GL_EndGroup ();
}

int Q3P_ActiveCount (void)
{
	return q3p_numactive;
}
