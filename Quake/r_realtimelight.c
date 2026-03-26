#include "quakedef.h"

#include "r_realtimelight.h"
#include "r_dlight_pool.h"

#define RL_FRAME_LIGHTS_MAX DLIGHT_GPU_MAX

extern cvar_t r_dlight_entities;

cvar_t r_ppdlights = { "r_ppdlights", "0", CVAR_ARCHIVE };
/* Forward world consumer toggle (shared frame-light list -> world dlight pass). */
cvar_t r_ppdlights_world = { "r_ppdlights_world", "1", CVAR_ARCHIVE };
cvar_t r_ppdlights_world_scale = { "r_ppdlights_world_scale", "1", CVAR_ARCHIVE };
/* World-light shaping controls, applied in shader before additive blend. */
cvar_t r_ppdlights_world_luma_clamp = { "r_ppdlights_world_luma_clamp", "1.0", CVAR_ARCHIVE };
cvar_t r_ppdlights_world_soft_knee = { "r_ppdlights_world_soft_knee", "1.0", CVAR_ARCHIVE };
/* Experimental fixed-function blend op override for world dlight pass. */
cvar_t r_experimental_ppdlights_world_blendop = { "r_experimental_ppdlights_world_blendop", "0", CVAR_ARCHIVE };
/* Forward alias/model consumer toggle (shared frame-light list -> alias lighting). */
cvar_t r_ppdlights_models = { "r_ppdlights_models", "1", CVAR_ARCHIVE };
/* Froxel fog consumer toggle (shared frame-light list -> volumetric injection). */
cvar_t r_ppdlights_fog = { "r_ppdlights_fog", "0", CVAR_ARCHIVE };
cvar_t r_ppdlights_fog_debug = { "r_ppdlights_fog_debug", "0", CVAR_NONE };
/* Per-frame fog consumer budget from the shared frame-light list. */
cvar_t r_ppdlights_fog_budget = { "r_ppdlights_fog_budget", "32", CVAR_ARCHIVE };
/* Optional GI helper that derives broad bounce from shared frame lights. */
cvar_t r_ppdlights_gi = { "r_ppdlights_gi", "0", CVAR_ARCHIVE };
cvar_t r_ppdlights_gi_debug = { "r_ppdlights_gi_debug", "0", CVAR_NONE };
cvar_t r_ppdlights_gi_budget = { "r_ppdlights_gi_budget", "8", CVAR_ARCHIVE };
cvar_t r_ppdlights_debug = { "r_ppdlights_debug", "0", CVAR_NONE };
/* World dlight debug views: 0=off, 1=affected count, 2=attenuation, 3=raw light, 4=new/legacy split,
 * 5=pre-compression contribution, 6=post-compression contribution. */
cvar_t r_ppdlights_debug_mode = { "r_ppdlights_dbgmode", "0", CVAR_ARCHIVE };
cvar_t r_ppdlights_emissive = { "r_ppdlights_emissive", "0", CVAR_ARCHIVE };
cvar_t r_ppdlights_emissive_debug = { "r_ppdlights_emissive_debug", "0", CVAR_NONE };

static rl_light_t rl_frame_lights[RL_FRAME_LIGHTS_MAX];
static dlight_t *rl_frame_light_sources[RL_FRAME_LIGHTS_MAX];
static int rl_frame_light_count = 0;
static rl_light_collect_stats_t rl_frame_stats;
static rl_consumer_stats_t rl_consumer_stats[RL_CONSUMER_COUNT];

typedef struct rl_source_summary_s
{
	unsigned int source_id;
	int offered_count;
	int accepted[RL_CONSUMER_COUNT];
	float energy[RL_CONSUMER_COUNT];
	int rejected[RL_CONSUMER_COUNT][RL_REJECT_COUNT];
} rl_source_summary_t;

static rl_source_summary_t rl_source_summaries[RL_FRAME_LIGHTS_MAX];
static int rl_source_summary_count = 0;

#define RL_EMISSIVE_BUDGET_DEFAULT 24

static void R_PPdlights_DebugModeCompat_f (void)
{
	if (Cmd_Argc () <= 1)
	{
		Con_Printf ("\"r_ppdlights_debug_mode\" is deprecated; use \"r_ppdlights_dbgmode\" (current %.0f)\n",
			r_ppdlights_debug_mode.value);
		return;
	}

	Cvar_SetValueQuick (&r_ppdlights_debug_mode, Q_atof (Cmd_Argv (1)));
}

static void R_PPdlights_WorldBlendOpCompat_f (void)
{
	if (Cmd_Argc () <= 1)
	{
		Con_Printf ("\"r_ppdlights_world_blendop\" is deprecated; use \"r_experimental_ppdlights_world_blendop\" (current %.0f)\n",
			r_experimental_ppdlights_world_blendop.value);
		return;
	}

	Cvar_SetValueQuick (&r_experimental_ppdlights_world_blendop, Q_atof (Cmd_Argv (1)));
}

/* Backward compatibility for temporary milestone cvar names. */
static void R_PPdlights_EmissiveShortAlias_f (void)
{
	if (Cmd_Argc () <= 1)
	{
		Con_Printf ("\"r_ppd_emissive\" is deprecated; use \"r_ppdlights_emissive\" (current %.0f)\n",
			r_ppdlights_emissive.value);
		return;
	}

	Cvar_SetValueQuick (&r_ppdlights_emissive, Q_atof (Cmd_Argv (1)));
}

/* Backward compatibility for temporary milestone cvar names. */
static void R_PPdlights_EmissiveDebugShortAlias_f (void)
{
	if (Cmd_Argc () <= 1)
	{
		Con_Printf ("\"r_ppd_emisdbg\" is deprecated; use \"r_ppdlights_emissive_debug\" (current %.0f)\n",
			r_ppdlights_emissive_debug.value);
		return;
	}

	Cvar_SetValueQuick (&r_ppdlights_emissive_debug, Q_atof (Cmd_Argv (1)));
}

static qboolean R_PPdlights_IsFrustumCulled (const vec3_t origin, float radius)
{
	int i;

	for (i = 0; i < 4; ++i)
	{
		const mplane_t *p = &frustum[i];
		if (DotProduct (p->normal, origin) - p->dist + radius < 0.f)
			return true;
	}

	return false;
}

static void R_PPdlights_Stats_f (void)
{
	Con_Printf ("r_ppdlights: src(dlight=%d emissive=%d) accepted(total=%d dlight=%d emissive=%d) culled(inactive=%d not_live=%d persistent_off=%d zero_radius=%d frustum=%d budget=%d emissive_budget=%d)\n",
		rl_frame_stats.source_dlights,
		rl_frame_stats.source_emissive,
		rl_frame_stats.accepted,
		rl_frame_stats.accepted_dlights,
		rl_frame_stats.accepted_emissive,
		rl_frame_stats.rejected_inactive,
		rl_frame_stats.rejected_not_live,
		rl_frame_stats.rejected_persistent_disabled,
		rl_frame_stats.rejected_zero_radius,
		rl_frame_stats.rejected_frustum,
		rl_frame_stats.rejected_budget,
		rl_frame_stats.rejected_emissive_budget);
}

static const char *R_PPdlights_ConsumerName (rl_light_consumer_t consumer)
{
	switch (consumer)
	{
	case RL_CONSUMER_WORLD: return "world";
	case RL_CONSUMER_MODEL: return "model";
	case RL_CONSUMER_FOG: return "fog";
	default: return "unknown";
	}
}

static const char *R_PPdlights_RejectReasonName (rl_consumer_reject_reason_t reason)
{
	switch (reason)
	{
	case RL_REJECT_NON_CONTRIB: return "non_contrib";
	case RL_REJECT_DISTANCE: return "distance";
	case RL_REJECT_LOCAL_BUDGET: return "local_budget";
	case RL_REJECT_HW_BUDGET: return "hw_budget";
	default: return "unknown";
	}
}

static rl_source_summary_t *R_PPdlights_GetSourceSummary (unsigned int source_id, qboolean create_if_missing)
{
	int i;

	for (i = 0; i < rl_source_summary_count; ++i)
	{
		if (rl_source_summaries[i].source_id == source_id)
			return &rl_source_summaries[i];
	}

	if (!create_if_missing || rl_source_summary_count >= RL_FRAME_LIGHTS_MAX)
		return NULL;

	memset (&rl_source_summaries[rl_source_summary_count], 0, sizeof (rl_source_summaries[rl_source_summary_count]));
	rl_source_summaries[rl_source_summary_count].source_id = source_id;
	rl_source_summary_count++;
	return &rl_source_summaries[rl_source_summary_count - 1];
}

static void R_PPdlights_Participation_f (void)
{
	int i, c;

	Con_Printf ("r_ppdlights participation summary:\n");
	for (c = 0; c < RL_CONSUMER_COUNT; ++c)
	{
		const rl_consumer_stats_t *s = &rl_consumer_stats[c];
		Con_Printf ("  %s: considered=%d accepted=%d energy=%.3f reject(%s=%d %s=%d %s=%d %s=%d)\n",
			R_PPdlights_ConsumerName ((rl_light_consumer_t)c),
			s->considered,
			s->accepted,
			s->accepted_energy,
			R_PPdlights_RejectReasonName (RL_REJECT_NON_CONTRIB), s->rejected[RL_REJECT_NON_CONTRIB],
			R_PPdlights_RejectReasonName (RL_REJECT_DISTANCE), s->rejected[RL_REJECT_DISTANCE],
			R_PPdlights_RejectReasonName (RL_REJECT_LOCAL_BUDGET), s->rejected[RL_REJECT_LOCAL_BUDGET],
			R_PPdlights_RejectReasonName (RL_REJECT_HW_BUDGET), s->rejected[RL_REJECT_HW_BUDGET]);
	}

	Con_Printf ("  source participation (source_id offered world model fog | reject reasons):\n");
	for (i = 0; i < rl_source_summary_count; ++i)
	{
		const rl_source_summary_t *e = &rl_source_summaries[i];
		Con_Printf ("    0x%08x offer=%d world=%d(%.3f) model=%d(%.3f) fog=%d(%.3f) reject[w:%d/%d/%d/%d m:%d/%d/%d/%d f:%d/%d/%d/%d]\n",
			e->source_id,
			e->offered_count,
			e->accepted[RL_CONSUMER_WORLD], e->energy[RL_CONSUMER_WORLD],
			e->accepted[RL_CONSUMER_MODEL], e->energy[RL_CONSUMER_MODEL],
			e->accepted[RL_CONSUMER_FOG], e->energy[RL_CONSUMER_FOG],
			e->rejected[RL_CONSUMER_WORLD][RL_REJECT_NON_CONTRIB],
			e->rejected[RL_CONSUMER_WORLD][RL_REJECT_DISTANCE],
			e->rejected[RL_CONSUMER_WORLD][RL_REJECT_LOCAL_BUDGET],
			e->rejected[RL_CONSUMER_WORLD][RL_REJECT_HW_BUDGET],
			e->rejected[RL_CONSUMER_MODEL][RL_REJECT_NON_CONTRIB],
			e->rejected[RL_CONSUMER_MODEL][RL_REJECT_DISTANCE],
			e->rejected[RL_CONSUMER_MODEL][RL_REJECT_LOCAL_BUDGET],
			e->rejected[RL_CONSUMER_MODEL][RL_REJECT_HW_BUDGET],
			e->rejected[RL_CONSUMER_FOG][RL_REJECT_NON_CONTRIB],
			e->rejected[RL_CONSUMER_FOG][RL_REJECT_DISTANCE],
			e->rejected[RL_CONSUMER_FOG][RL_REJECT_LOCAL_BUDGET],
			e->rejected[RL_CONSUMER_FOG][RL_REJECT_HW_BUDGET]);
	}
}

static unsigned int R_PPdlights_EmissiveSourceId (const entity_t *ent, unsigned int tag)
{
	unsigned int ent_id = 0u;

	if (ent >= cl_entities && ent < cl_entities + cl.num_entities)
		ent_id = (unsigned int)(ent - cl_entities);
	else
		ent_id = (unsigned int)(((uintptr_t)ent >> 4) & 0xffffu);

	return (0xEEu << 24) | ((tag & 0xffu) << 16) | (ent_id & 0xffffu);
}

static qboolean R_PPdlights_AddFrameLight (const vec3_t origin, float radius, const vec3_t color, float intensity,
	unsigned int type, unsigned int flags, unsigned int source_id, dlight_t *source, qboolean from_emissive)
{
	rl_light_t *dst;

	if (radius <= 0.f || intensity <= 0.f)
	{
		rl_frame_stats.rejected_zero_radius++;
		return false;
	}

	/*
	 * Keep froxel fog parity with legacy dlight injection:
	 * when PP fog is enabled, do not frustum-cull volumetric contributors.
	 */
	if (R_PPdlights_IsFrustumCulled (origin, radius)
		&& !(r_ppdlights.value > 0.f
			&& r_ppdlights_fog.value > 0.f
			&& (flags & RL_LIGHT_VOLUMETRIC_CONTRIB) != 0u))
	{
		rl_frame_stats.rejected_frustum++;
		return false;
	}

	if (rl_frame_light_count >= RL_FRAME_LIGHTS_MAX)
	{
		rl_frame_stats.rejected_budget++;
		return false;
	}

	dst = &rl_frame_lights[rl_frame_light_count++];
	VectorCopy (origin, dst->origin);
	dst->radius = radius;
	VectorCopy (color, dst->color);
	dst->intensity = intensity;
	dst->type = type;
	dst->flags = flags;
	dst->source_id = source_id;
	/* Preserve original dlight subtype for fog/froxel parity (e.g. lava/torch weighting). */
	dst->reserved = (type == RL_LIGHT_POINT && source != NULL)
		? (unsigned int)source->type
		: 0u;
	rl_frame_light_sources[rl_frame_light_count - 1] = source;
	{
		rl_source_summary_t *sum = R_PPdlights_GetSourceSummary (source_id, true);
		if (sum)
			sum->offered_count++;
	}
	rl_frame_stats.accepted++;
	if (from_emissive)
		rl_frame_stats.accepted_emissive++;
	else
		rl_frame_stats.accepted_dlights++;
	return true;
}

static void R_PPdlights_CollectEmissiveFrame (void)
{
	int i;
	int emissive_count = 0;
	const int emissive_budget = CLAMP (0, RL_EMISSIVE_BUDGET_DEFAULT, RL_FRAME_LIGHTS_MAX);

	if (r_ppdlights_emissive.value <= 0.f || cl_numvisedicts <= 0)
		return;

	for (i = 0; i < cl_numvisedicts; ++i)
	{
		entity_t *ent = cl_visedicts[i];
		vec3_t color;
		float radius = 0.f;
		float intensity = 0.f;
		unsigned int source_tag = 0u;

		if (!ent || !ent->model || ent->alpha == ENTALPHA_ZERO)
			continue;

		if ((ent->effects & EF_MUZZLEFLASH) != 0)
		{
			VectorSet (color, 1.00f, 0.70f, 0.30f);
			radius = 128.f;
			intensity = 0.45f;
			source_tag = 1u;
		}
		else if ((ent->model->flags & EF_ROCKET) != 0)
		{
			VectorSet (color, 1.00f, 0.82f, 0.58f);
			radius = 144.f;
			intensity = 0.55f;
			source_tag = 2u;
		}
		else if ((ent->model->flags & EF_GRENADE) != 0)
		{
			VectorSet (color, 1.00f, 0.78f, 0.48f);
			radius = 112.f;
			intensity = 0.40f;
			source_tag = 3u;
		}
		else if ((ent->effects & EF_BRIGHTLIGHT) != 0)
		{
			VectorSet (color, 1.00f, 0.94f, 0.72f);
			radius = 136.f;
			intensity = 0.35f;
			source_tag = 4u;
		}
		else if ((ent->effects & EF_DIMLIGHT) != 0)
		{
			VectorSet (color, 0.70f, 0.82f, 1.00f);
			radius = 96.f;
			intensity = 0.30f;
			source_tag = 5u;
		}
		else if ((ent->effects & EF_QEX_QUADLIGHT) != 0)
		{
			VectorSet (color, 0.30f, 0.45f, 1.00f);
			radius = 128.f;
			intensity = 0.40f;
			source_tag = 6u;
		}
		else if ((ent->effects & EF_QEX_PENTALIGHT) != 0)
		{
			VectorSet (color, 1.00f, 0.44f, 0.30f);
			radius = 128.f;
			intensity = 0.40f;
			source_tag = 7u;
		}
		else if ((ent->effects & EF_QEX_CANDLELIGHT) != 0)
		{
			VectorSet (color, 1.00f, 0.68f, 0.36f);
			radius = 88.f;
			intensity = 0.25f;
			source_tag = 8u;
		}
		else if (ent->model->name && !Q_strncmp (ent->model->name, "progs/bolt", 10))
		{
			VectorSet (color, 0.58f, 0.70f, 1.00f);
			radius = 120.f;
			intensity = 0.35f;
			source_tag = 9u;
		}

		if (source_tag == 0u)
			continue;

		rl_frame_stats.source_emissive++;
		if (emissive_count >= emissive_budget)
		{
			rl_frame_stats.rejected_emissive_budget++;
			continue;
		}

		if (R_PPdlights_AddFrameLight (ent->origin, radius, color, intensity,
			RL_LIGHT_EMISSIVE_PROXY,
			RL_LIGHT_SURFACE_CONTRIB | RL_LIGHT_VOLUMETRIC_CONTRIB,
			R_PPdlights_EmissiveSourceId (ent, source_tag),
			NULL,
			true))
		{
			emissive_count++;
		}
	}

	if (r_ppdlights_emissive_debug.value >= 1.f && (r_framecount % 60) == 0)
	{
		Con_DPrintf ("r_ppdlights_emissive: src=%d accepted=%d budget=%d reject_budget=%d\n",
			rl_frame_stats.source_emissive,
			rl_frame_stats.accepted_emissive,
			emissive_budget,
			rl_frame_stats.rejected_emissive_budget);
	}
}

void R_PPdlights_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_ppdlights);
	Cvar_RegisterVariable (&r_ppdlights_world);
	Cvar_RegisterVariable (&r_ppdlights_world_scale);
	Cvar_RegisterVariable (&r_ppdlights_world_luma_clamp);
	Cvar_RegisterVariable (&r_ppdlights_world_soft_knee);
	Cvar_RegisterVariable (&r_experimental_ppdlights_world_blendop);
	Cvar_RegisterVariable (&r_ppdlights_models);
	Cvar_RegisterVariable (&r_ppdlights_fog);
	Cvar_RegisterVariable (&r_ppdlights_fog_debug);
	Cvar_RegisterVariable (&r_ppdlights_fog_budget);
	Cvar_RegisterVariable (&r_ppdlights_gi);
	Cvar_RegisterVariable (&r_ppdlights_gi_debug);
	Cvar_RegisterVariable (&r_ppdlights_gi_budget);
	Cvar_RegisterVariable (&r_ppdlights_debug);
	Cvar_RegisterVariable (&r_ppdlights_debug_mode);
	Cvar_RegisterVariable (&r_ppdlights_emissive);
	Cvar_RegisterVariable (&r_ppdlights_emissive_debug);
	Cmd_AddCommand ("r_ppdlights_stats", R_PPdlights_Stats_f);
	Cmd_AddCommand ("r_ppdlights_debug_mode", R_PPdlights_DebugModeCompat_f);
	Cmd_AddCommand ("r_ppdlights_world_blendop", R_PPdlights_WorldBlendOpCompat_f);
	Cmd_AddCommand ("r_ppd_emissive", R_PPdlights_EmissiveShortAlias_f);
	Cmd_AddCommand ("r_ppd_emisdbg", R_PPdlights_EmissiveDebugShortAlias_f);
	Cmd_AddCommand ("r_ppdlights_participation", R_PPdlights_Participation_f);
}

void R_PPdlights_CollectFrame (void)
{
	const dlight_t *const *active;
	int active_count = 0;
	int i;
	qboolean collect_enabled;

	rl_frame_light_count = 0;
	memset (rl_frame_light_sources, 0, sizeof (rl_frame_light_sources));
	memset (&rl_frame_stats, 0, sizeof (rl_frame_stats));
	memset (rl_consumer_stats, 0, sizeof (rl_consumer_stats));
	memset (rl_source_summaries, 0, sizeof (rl_source_summaries));
	rl_source_summary_count = 0;

	collect_enabled = (r_dynamic.value > 0.f)
		&& (r_ppdlights.value > 0.f
			|| r_ppdlights_debug.value > 0.f
			|| r_ppdlights_emissive.value > 0.f
			|| r_ppdlights_emissive_debug.value > 0.f);
	if (!collect_enabled)
		return;

	/*
	 * Architecture: this is the sole frame-level gather point.
	 * We merge dynamic + emissive contributors once, then downstream passes
	 * (world forward lights, alias/model forward lights, froxel fog/GI) each
	 * consume filtered views of this same array in their own pass budgets.
	 */
	active = DLightPool_GetActiveList (&active_count);
	if (active && active_count > 0)
	{
		for (i = 0; i < active_count; ++i)
		{
			const dlight_t *dl = active[i];
			float radius = 0.f;
			vec3_t color;

			rl_frame_stats.source_dlights++;

			if (!CL_DlightTransientIsLiveAtTime (dl, cl.time, NULL))
			{
				rl_frame_stats.rejected_not_live++;
				continue;
			}

			if (!CL_DlightIsActive (dl))
			{
				rl_frame_stats.rejected_inactive++;
				continue;
			}

			if (dl->kind == DL_PERSISTENT && r_dlight_entities.value <= 0.f)
			{
				rl_frame_stats.rejected_persistent_disabled++;
				continue;
			}

			R_EvaluateDLightForRender (dl, &radius, color);

			R_PPdlights_AddFrameLight (dl->origin, radius, color, 1.f,
				RL_LIGHT_POINT,
				RL_LIGHT_SURFACE_CONTRIB | RL_LIGHT_VOLUMETRIC_CONTRIB,
				(unsigned int)dl->key,
				(dlight_t *)dl,
				false);
		}
	}

	R_PPdlights_CollectEmissiveFrame ();

	if (r_ppdlights_debug.value >= 2.f && (r_framecount % 60) == 0)
	{
		Con_DPrintf ("r_ppdlights_collect: src(d=%d e=%d) accepted(d=%d e=%d) frustum=%d budget=%d\n",
			rl_frame_stats.source_dlights,
			rl_frame_stats.source_emissive,
			rl_frame_stats.accepted_dlights,
			rl_frame_stats.accepted_emissive,
			rl_frame_stats.rejected_frustum,
			rl_frame_stats.rejected_budget);
	}
}

const rl_light_t *R_PPdlights_GetFrameLights (int *out_count)
{
	if (out_count)
		*out_count = rl_frame_light_count;
	return rl_frame_lights;
}

void R_PPdlights_GetFrameStats (rl_light_collect_stats_t *out_stats)
{
	if (out_stats)
		*out_stats = rl_frame_stats;
}

qboolean R_PPdlights_WorldPathEnabled (void)
{
	return (r_dynamic.value > 0.f
		&& r_ppdlights.value > 0.f
		&& r_ppdlights_world.value > 0.f);
}

int R_PPdlights_BuildWorldGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	int i;
	int count = 0;

	if (!out_buffer || !out_sources || max_lights <= 0)
		return 0;

	for (i = 0; i < rl_frame_light_count; ++i)
	{
		const rl_light_t *src = &rl_frame_lights[i];
		gpulight_t *dst = &out_buffer->lights[count];
		float energy;
		R_PPdlights_RecordConsumerConsidered (RL_CONSUMER_WORLD, src->source_id);
		if ((src->flags & RL_LIGHT_SURFACE_CONTRIB) == 0u)
		{
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_WORLD, src->source_id, RL_REJECT_NON_CONTRIB);
			continue;
		}
		if (count >= max_lights)
		{
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_WORLD, src->source_id, RL_REJECT_LOCAL_BUDGET);
			continue;
		}
		dst->pos[0] = src->origin[0];
		dst->pos[1] = src->origin[1];
		dst->pos[2] = src->origin[2];
		dst->radius = src->radius;
		dst->color[0] = src->color[0] * src->intensity;
		dst->color[1] = src->color[1] * src->intensity;
		dst->color[2] = src->color[2] * src->intensity;
		dst->minlight = 0.f;
		out_sources[count] = rl_frame_light_sources[i];
		energy = dst->color[0] * 0.2126f + dst->color[1] * 0.7152f + dst->color[2] * 0.0722f;
		R_PPdlights_RecordConsumerAccept (RL_CONSUMER_WORLD, src->source_id, energy);
		count++;
	}

	return count;
}

qboolean R_PPdlights_ModelPathEnabled (void)
{
	return (r_dynamic.value > 0.f
		&& r_ppdlights.value > 0.f
		&& r_ppdlights_models.value > 0.f);
}

int R_PPdlights_BuildModelGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	int i;
	int count = 0;

	if (!out_buffer || !out_sources || max_lights <= 0)
		return 0;

	for (i = 0; i < rl_frame_light_count; ++i)
	{
		const rl_light_t *src = &rl_frame_lights[i];
		gpulight_t *dst = &out_buffer->lights[count];
		float energy;
		R_PPdlights_RecordConsumerConsidered (RL_CONSUMER_MODEL, src->source_id);
		if ((src->flags & RL_LIGHT_SURFACE_CONTRIB) == 0u)
		{
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_MODEL, src->source_id, RL_REJECT_NON_CONTRIB);
			continue;
		}
		if (count >= max_lights)
		{
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_MODEL, src->source_id, RL_REJECT_LOCAL_BUDGET);
			continue;
		}
		dst->pos[0] = src->origin[0];
		dst->pos[1] = src->origin[1];
		dst->pos[2] = src->origin[2];
		dst->radius = src->radius;
		dst->color[0] = src->color[0] * src->intensity;
		dst->color[1] = src->color[1] * src->intensity;
		dst->color[2] = src->color[2] * src->intensity;
		dst->minlight = 0.f;
		out_sources[count] = rl_frame_light_sources[i];
		energy = dst->color[0] * 0.2126f + dst->color[1] * 0.7152f + dst->color[2] * 0.0722f;
		R_PPdlights_RecordConsumerAccept (RL_CONSUMER_MODEL, src->source_id, energy);
		count++;
	}

	return count;
}

void R_PPdlights_RecordConsumerAccept (rl_light_consumer_t consumer, unsigned int source_id, float energy)
{
	rl_source_summary_t *sum;

	if ((int)consumer < 0 || consumer >= RL_CONSUMER_COUNT)
		return;
	rl_consumer_stats[consumer].accepted++;
	rl_consumer_stats[consumer].accepted_energy += q_max (0.f, energy);
	sum = R_PPdlights_GetSourceSummary (source_id, true);
	if (!sum)
		return;
	sum->accepted[consumer]++;
	sum->energy[consumer] += q_max (0.f, energy);
}

void R_PPdlights_RecordConsumerReject (rl_light_consumer_t consumer, unsigned int source_id, rl_consumer_reject_reason_t reason)
{
	rl_source_summary_t *sum;

	if ((int)consumer < 0 || consumer >= RL_CONSUMER_COUNT)
		return;
	if ((int)reason < 0 || reason >= RL_REJECT_COUNT)
		return;
	rl_consumer_stats[consumer].rejected[reason]++;
	sum = R_PPdlights_GetSourceSummary (source_id, true);
	if (!sum)
		return;
	sum->rejected[consumer][reason]++;
}

qboolean R_PPdlights_GetConsumerStats (rl_light_consumer_t consumer, rl_consumer_stats_t *out_stats)
{
	if ((int)consumer < 0 || consumer >= RL_CONSUMER_COUNT || !out_stats)
		return false;
	*out_stats = rl_consumer_stats[consumer];
	return true;
}

void R_PPdlights_RecordConsumerConsidered (rl_light_consumer_t consumer, unsigned int source_id)
{
	if ((int)consumer < 0 || consumer >= RL_CONSUMER_COUNT)
		return;
	rl_consumer_stats[consumer].considered++;
	(void)R_PPdlights_GetSourceSummary (source_id, true);
}
