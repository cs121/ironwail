#include "quakedef.h"
#include "r_particles_iw.h"

#define IW_DEFAULT_PARTICLE_CAP 8192
#define IW_ABSOLUTE_MIN_PARTICLE_CAP 256
#define IW_ABSOLUTE_MAX_PARTICLE_CAP 262144

cvar_t r_particles_mode = {"r_particles_mode", "classic", CVAR_ARCHIVE};
cvar_t r_particles_max = {"r_particles_max", "8192", CVAR_ARCHIVE};

static particle_mode_t r_particles_mode_state = PARTICLE_MODE_CLASSIC;

static particle_instance_t *iw_particles;
static int iw_capacity;
static int iw_count;

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
}

void R_IWParticles_Init (void)
{
	Cvar_RegisterVariable(&r_particles_mode);
	Cvar_SetCallback(&r_particles_mode, R_IWParticles_ApplyMode_f);
	R_IWParticles_ApplyMode_f(&r_particles_mode);

	Cvar_RegisterVariable(&r_particles_max);
	Cvar_SetCallback(&r_particles_max, R_IWParticles_SetMax_f);
	R_IWParticles_Realloc();
}

void R_IWParticles_Shutdown (void)
{
	if (iw_particles)
		free(iw_particles);
	iw_particles = NULL;
	iw_capacity = 0;
	iw_count = 0;
}

void R_IWParticles_NewMap (void)
{
	iw_count = 0;
}

void R_IWParticles_Clear (void)
{
	iw_count = 0;
}

particle_mode_t R_ParticlesMode (void)
{
	return r_particles_mode_state;
}

qboolean R_ParticlesUseClassic (void)
{
	return (r_particles_mode_state == PARTICLE_MODE_CLASSIC || r_particles_mode_state == PARTICLE_MODE_HYBRID);
}
