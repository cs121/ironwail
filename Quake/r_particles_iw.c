#include "quakedef.h"
#include "r_particles_iw.h"

#define IW_DEFAULT_PARTICLE_CAP 8192
#define IW_ABSOLUTE_MIN_PARTICLE_CAP 256
#define IW_ABSOLUTE_MAX_PARTICLE_CAP 262144

typedef struct iw_q3p_particle_s
{
	vec3_t pos;
	vec3_t vel;
	byte color[4];
	float size;
	float life;
	float max_life;
	float sort_key;
	int material;
	int blend_state;
	qboolean depth_write;
	qboolean active;
} iw_q3p_particle_t;

typedef struct iw_q3p_particle_vert_s
{
	vec3_t pos;
	byte color[4];
} iw_q3p_particle_vert_t;

cvar_t r_particles_mode = {"r_particles_mode", "classic", CVAR_ARCHIVE};
cvar_t r_particles_max = {"r_particles_max", "8192", CVAR_ARCHIVE};
cvar_t r_particles_sort = {"r_particles_sort", "0", CVAR_ARCHIVE};

static particle_mode_t r_particles_mode_state = PARTICLE_MODE_CLASSIC;

static particle_instance_t *iw_particles;
static int iw_capacity;
static int iw_count;

static iw_q3p_particle_t *iw_q3p_particles;
static int iw_q3p_capacity;
static int iw_q3p_count;
static double iw_q3p_last_sim_time;
static qboolean iw_q3p_sim_inited;

static iw_q3p_particle_vert_t iw_q3p_verts[4096];
static int iw_q3p_numverts;

static particle_mode_t R_Particles_ParseModeString (const char *value)
{
	if (!value || !value[0])
		return PARTICLE_MODE_CLASSIC;

	if (!q_strcasecmp(value, "q3p") || !strcmp(value, "2"))
		return PARTICLE_MODE_Q3P;
	if (!q_strcasecmp(value, "hybrid") || !strcmp(value, "3"))
		return PARTICLE_MODE_HYBRID;
	if (!q_strcasecmp(value, "legacy") || !q_strcasecmp(value, "classic") || !strcmp(value, "0") || !strcmp(value, "1"))
		return PARTICLE_MODE_CLASSIC;

	return PARTICLE_MODE_CLASSIC;
}

static const char *R_Particles_ModeName (particle_mode_t mode)
{
	switch (mode)
	{
	case PARTICLE_MODE_Q3P:
		return "q3p";
	case PARTICLE_MODE_HYBRID:
		return "hybrid";
	default:
		return "classic";
	}
}

static void R_IWParticles_ApplyMode_f (cvar_t *var)
{
	particle_mode_t mode = R_Particles_ParseModeString(var->string);
	const char *canonical = R_Particles_ModeName(mode);

	r_particles_mode_state = mode;
	if (q_strcasecmp(var->string, canonical))
		Cvar_SetQuick(var, canonical);
}

static int R_IWParticles_ClampMax (void)
{
	int limit = (int)Q_rint(r_particles_max.value);

	if (limit <= 0)
		limit = IW_DEFAULT_PARTICLE_CAP;
	limit = CLAMP(IW_ABSOLUTE_MIN_PARTICLE_CAP, limit, IW_ABSOLUTE_MAX_PARTICLE_CAP);
	if (limit != (int)Q_rint(r_particles_max.value))
		Cvar_SetValueQuick(&r_particles_max, limit);
	return limit;
}

static int IW_Q3P_SortMode (void)
{
	int mode = (int)Q_rint(r_particles_sort.value);
	mode = CLAMP(0, mode, 1);
	if (mode != (int)Q_rint(r_particles_sort.value))
		Cvar_SetValueQuick(&r_particles_sort, mode);
	return mode;
}

static void IW_Q3P_ApplySortMode_f (cvar_t *var)
{
	(void)var;
	(void)IW_Q3P_SortMode();
}

static void IW_Q3P_Realloc (void)
{
	int target = R_IWParticles_ClampMax();

	if (target == iw_q3p_capacity)
		return;

	if (iw_q3p_particles)
		free(iw_q3p_particles);

	iw_q3p_particles = (iw_q3p_particle_t *)calloc((size_t)target, sizeof(*iw_q3p_particles));
	if (!iw_q3p_particles)
		Sys_Error("IW_Q3P_Realloc: unable to allocate %d particles", target);

	iw_q3p_capacity = target;
	iw_q3p_count = 0;
}

static void R_IWParticles_Realloc (void)
{
	int target = R_IWParticles_ClampMax();

	if (target == iw_capacity)
		return;

	if (iw_particles)
		free(iw_particles);

	iw_particles = (particle_instance_t *)calloc((size_t)target, sizeof(*iw_particles));
	if (!iw_particles)
		Sys_Error("R_IWParticles_Realloc: unable to allocate %d particles", target);

	iw_capacity = target;
	iw_count = 0;
}

static void R_IWParticles_SetMax_f (cvar_t *var)
{
	(void)var;
	R_IWParticles_Realloc();
	IW_Q3P_Realloc();
}

static void IW_Q3P_SpawnDummyParticle (const vec3_t origin, float yaw, float life)
{
	iw_q3p_particle_t *p;

	if (iw_q3p_count >= iw_q3p_capacity)
		return;

	p = &iw_q3p_particles[iw_q3p_count++];
	VectorCopy(origin, p->pos);
	p->pos[0] += cosf(yaw) * 24.f;
	p->pos[1] += sinf(yaw) * 24.f;
	p->pos[2] += 8.f + sinf(yaw * 3.f) * 6.f;
	p->vel[0] = cosf(yaw + 3.14159265358979323846 * 0.5f) * 6.f;
	p->vel[1] = sinf(yaw + 3.14159265358979323846 * 0.5f) * 6.f;
	p->vel[2] = 8.f;
	p->color[0] = 255;
	p->color[1] = (byte)(160 + (int)(95.f * (0.5f + 0.5f * sinf(yaw))));
	p->color[2] = 64;
	p->color[3] = 192;
	p->size = 1.f;
	p->life = life;
	p->max_life = life;
	p->material = 0;
	p->blend_state = 0;
	p->depth_write = false;
	p->active = true;
}

static void IW_Q3P_Simulate (double now)
{
	int i;
	int active;
	float dt;
	vec3_t spawn_origin;
	int spawn_count;

	if (!iw_q3p_sim_inited)
	{
		iw_q3p_last_sim_time = now;
		iw_q3p_sim_inited = true;
	}

	dt = (float)(now - iw_q3p_last_sim_time);
	if (dt < 0.f)
		dt = 0.f;
	if (dt > 0.05f)
		dt = 0.05f;
	iw_q3p_last_sim_time = now;

	VectorCopy(r_refdef.vieworg, spawn_origin);
	spawn_origin[2] += 12.f;
	spawn_count = (int)CLAMP(0, iw_q3p_capacity / 8, dt * 60.f + 1.f);
	for (i = 0; i < spawn_count; ++i)
	{
		float yaw = (float)now * 2.5f + (float)i * (float)(3.14159265358979323846 * 2.0 / 8.0);
		IW_Q3P_SpawnDummyParticle(spawn_origin, yaw, 0.9f);
	}

	for (i = active = 0; i < iw_q3p_count; ++i)
	{
		iw_q3p_particle_t *p = &iw_q3p_particles[i];
		if (!p->active)
			continue;

		p->life -= dt;
		if (p->life <= 0.f)
			continue;

		p->vel[2] -= dt * 14.f;
		VectorMA(p->pos, dt, p->vel, p->pos);
		p->color[3] = (byte)(255.f * (p->life / p->max_life));

		if (i != active)
			iw_q3p_particles[active] = *p;
		active++;
	}

	iw_q3p_count = active;
}

static int IW_Q3P_ParticleCompare (const void *a, const void *b)
{
	const iw_q3p_particle_t *pa = (const iw_q3p_particle_t *)a;
	const iw_q3p_particle_t *pb = (const iw_q3p_particle_t *)b;

	if (pa->material != pb->material)
		return pa->material - pb->material;
	if (pa->blend_state != pb->blend_state)
		return pa->blend_state - pb->blend_state;
	if (pa->depth_write != pb->depth_write)
		return (int)pa->depth_write - (int)pb->depth_write;

	if (IW_Q3P_SortMode() == 1)
	{
		if (pa->sort_key > pb->sort_key)
			return -1;
		if (pa->sort_key < pb->sort_key)
			return 1;
	}

	return 0;
}

static void IW_Q3P_PrepareDrawList (void)
{
	int i;

	for (i = 0; i < iw_q3p_count; ++i)
	{
		vec3_t toeye;
		VectorSubtract(iw_q3p_particles[i].pos, r_origin, toeye);
		iw_q3p_particles[i].sort_key = DotProduct(toeye, toeye);
	}

	qsort(iw_q3p_particles, (size_t)iw_q3p_count, sizeof(iw_q3p_particles[0]), IW_Q3P_ParticleCompare);
}

static void IW_Q3P_FlushBatch (void)
{
	GLuint buf;
	GLbyte *ofs;

	if (!iw_q3p_numverts)
		return;

	GL_Upload(GL_ARRAY_BUFFER, iw_q3p_verts, sizeof(iw_q3p_verts[0]) * iw_q3p_numverts, &buf, &ofs);
	GL_BindBuffer(GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc(0, 3, GL_FLOAT, GL_FALSE, sizeof(iw_q3p_verts[0]), ofs + offsetof(iw_q3p_particle_vert_t, pos));
	GL_VertexAttribPointerFunc(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(iw_q3p_verts[0]), ofs + offsetof(iw_q3p_particle_vert_t, color));
	GL_DrawArraysInstancedFunc(GL_TRIANGLE_STRIP, 0, 4, iw_q3p_numverts);
	iw_q3p_numverts = 0;
}

void R_IWParticles_Draw (void)
{
	int i;
	int current_material;
	int current_blend;
	qboolean current_depth_write;
	float scalex;
	float scaley;

	if (R_ParticlesMode() != PARTICLE_MODE_Q3P)
		return;

	if (!iw_q3p_particles || iw_q3p_capacity <= 0)
		return;

	IW_Q3P_Simulate(cl.time);
	if (!iw_q3p_count)
		return;

	IW_Q3P_PrepareDrawList();

	GL_BeginGroup("IW Particles");
	GL_UseProgram(glprogs.particles[0][softemu == SOFTEMU_COARSE]);

	scalex = scaley = 0.375f;
	scalex *= r_matproj[1 * 4 + 0];
	scaley *= -r_matproj[2 * 4 + 1];
	GL_Uniform3fFunc(0, scalex, scaley, 1.f);

	GL_SetState(GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2) | GLS_INSTANCED_ATTRIBS(2));

	current_material = -1;
	current_blend = -1;
	current_depth_write = false;

	for (i = 0, iw_q3p_numverts = 0; i < iw_q3p_count; ++i)
	{
		iw_q3p_particle_vert_t *v;
		if (iw_q3p_particles[i].material != current_material
			|| iw_q3p_particles[i].blend_state != current_blend
			|| iw_q3p_particles[i].depth_write != current_depth_write)
		{
			IW_Q3P_FlushBatch();
			current_material = iw_q3p_particles[i].material;
			current_blend = iw_q3p_particles[i].blend_state;
			current_depth_write = iw_q3p_particles[i].depth_write;
		}

		if (iw_q3p_numverts == (int)countof(iw_q3p_verts))
			IW_Q3P_FlushBatch();

		v = &iw_q3p_verts[iw_q3p_numverts++];
		VectorCopy(iw_q3p_particles[i].pos, v->pos);
		v->color[0] = iw_q3p_particles[i].color[0];
		v->color[1] = iw_q3p_particles[i].color[1];
		v->color[2] = iw_q3p_particles[i].color[2];
		v->color[3] = iw_q3p_particles[i].color[3];
	}

	IW_Q3P_FlushBatch();
	GL_EndGroup();
}

void R_IWParticles_Init (void)
{
	Cvar_RegisterVariable(&r_particles_mode);
	Cvar_SetCallback(&r_particles_mode, R_IWParticles_ApplyMode_f);
	R_IWParticles_ApplyMode_f(&r_particles_mode);

	Cvar_RegisterVariable(&r_particles_max);
	Cvar_SetCallback(&r_particles_max, R_IWParticles_SetMax_f);
	R_IWParticles_Realloc();

	Cvar_RegisterVariable(&r_particles_sort);
	Cvar_SetCallback(&r_particles_sort, IW_Q3P_ApplySortMode_f);
	IW_Q3P_ApplySortMode_f(&r_particles_sort);
	IW_Q3P_Realloc();
}

void R_IWParticles_Shutdown (void)
{
	if (iw_particles)
		free(iw_particles);
	iw_particles = NULL;
	iw_capacity = 0;
	iw_count = 0;

	if (iw_q3p_particles)
		free(iw_q3p_particles);
	iw_q3p_particles = NULL;
	iw_q3p_capacity = 0;
	iw_q3p_count = 0;
	iw_q3p_sim_inited = false;
}

void R_IWParticles_NewMap (void)
{
	iw_count = 0;
	iw_q3p_count = 0;
	iw_q3p_sim_inited = false;
}

void R_IWParticles_Clear (void)
{
	iw_count = 0;
	iw_q3p_count = 0;
}

particle_mode_t R_ParticlesMode (void)
{
	return r_particles_mode_state;
}

qboolean R_ParticlesUseClassic (void)
{
	return (r_particles_mode_state == PARTICLE_MODE_CLASSIC || r_particles_mode_state == PARTICLE_MODE_HYBRID);
}
