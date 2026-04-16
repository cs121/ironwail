#include "quakedef.h"
#include "glquake.h"

#include "r_realtimelight.h"
#include "r_dlight_pool.h"

#define RL_FRAME_LIGHTS_MAX DLIGHT_GPU_MAX
enum
{
	RL_EMISSIVE_BUDGET = 24
};
static const qboolean rl_emissive_enabled = true;
static const qboolean rl_debug_enabled = false;

static rl_light_t rl_frame_lights[RL_FRAME_LIGHTS_MAX];
static dlight_t *rl_frame_light_sources[RL_FRAME_LIGHTS_MAX];
static int rl_frame_light_count = 0;
static rl_light_collect_stats_t rl_frame_stats;
static rl_consumer_stats_t rl_consumer_stats[RL_CONSUMER_COUNT];

typedef struct rl_source_summary_s
{
	unsigned int source_id;
	int offered_count;
	int rejected_priority;
	int accepted[RL_CONSUMER_COUNT];
	float energy[RL_CONSUMER_COUNT];
	int rejected[RL_CONSUMER_COUNT][RL_REJECT_COUNT];
} rl_source_summary_t;

static rl_source_summary_t rl_source_summaries[RL_FRAME_LIGHTS_MAX];
static int rl_source_summary_count = 0;
static int R_PPdlights_BuildGpuLightsForConsumer (rl_light_consumer_t consumer, gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights);

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

static unsigned int R_PPdlights_EmissiveSourceId (const entity_t *ent, unsigned int tag)
{
	unsigned int ent_id = 0u;

	if (ent >= cl_entities && ent < cl_entities + cl.num_entities)
		ent_id = (unsigned int)(ent - cl_entities);
	else
		ent_id = (unsigned int)(((uintptr_t)ent >> 4) & 0xffffu);

	return (0xEEu << 24) | ((tag & 0xffu) << 16) | (ent_id & 0xffffu);
}

typedef struct rl_frame_light_candidate_s
{
	vec3_t origin;
	float radius;
	vec3_t color;
	float intensity;
	unsigned int type;
	unsigned int flags;
	unsigned int source_id;
	dlight_t *source;
	qboolean from_emissive;
	float luminance;
	float camera_distance;
	float score;
} rl_frame_light_candidate_t;

static float R_PPdlights_CandidateLuminance (const vec3_t color, float intensity)
{
	return intensity * (0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2]);
}

static qboolean R_PPdlights_IsCandidateBetter (const rl_frame_light_candidate_t *a, const rl_frame_light_candidate_t *b)
{
	if (a->score > b->score)
		return true;
	if (a->score < b->score)
		return false;
	return a->source_id < b->source_id;
}

static void R_PPdlights_InsertTopCandidate (rl_frame_light_candidate_t *best, int *best_count,
	const rl_frame_light_candidate_t *candidate)
{
	int insert_at;

	if (*best_count == RL_FRAME_LIGHTS_MAX
		&& !R_PPdlights_IsCandidateBetter (candidate, &best[RL_FRAME_LIGHTS_MAX - 1]))
	{
		rl_source_summary_t *sum = R_PPdlights_GetSourceSummary (candidate->source_id, true);
		rl_frame_stats.rejected_priority++;
		if (sum)
			sum->rejected_priority++;
		return;
	}

	insert_at = *best_count;
	while (insert_at > 0 && R_PPdlights_IsCandidateBetter (candidate, &best[insert_at - 1]))
		insert_at--;

	if (*best_count == RL_FRAME_LIGHTS_MAX)
	{
		rl_source_summary_t *dropped = R_PPdlights_GetSourceSummary (best[RL_FRAME_LIGHTS_MAX - 1].source_id, true);
		rl_frame_stats.rejected_priority++;
		if (dropped)
			dropped->rejected_priority++;
	}
	else
	{
		(*best_count)++;
	}

	if (*best_count - 1 > insert_at)
		memmove (&best[insert_at + 1], &best[insert_at], (size_t)(*best_count - insert_at - 1) * sizeof (best[0]));

	best[insert_at] = *candidate;
}

static qboolean R_PPdlights_AddFrameLightCandidate (rl_frame_light_candidate_t *out_candidate,
	const vec3_t origin, float radius, const vec3_t color, float intensity,
	unsigned int type, unsigned int flags, unsigned int source_id, dlight_t *source, qboolean from_emissive)
{
	vec3_t delta;
	float distance;
	float dist_factor;
	rl_source_summary_t *sum;

	if (radius <= 0.f || intensity <= 0.f)
	{
		rl_frame_stats.rejected_zero_radius++;
		return false;
	}

	/* Volumetric contributors should remain available for volumetric effects. */
	if (R_PPdlights_IsFrustumCulled (origin, radius)
		&& (flags & RL_LIGHT_VOLUMETRIC_CONTRIB) == 0u)
	{
		rl_frame_stats.rejected_frustum++;
		return false;
	}

	VectorCopy (origin, out_candidate->origin);
	out_candidate->radius = radius;
	VectorCopy (color, out_candidate->color);
	out_candidate->intensity = intensity;
	out_candidate->type = type;
	out_candidate->flags = flags;
	out_candidate->source_id = source_id;
	out_candidate->source = source;
	out_candidate->from_emissive = from_emissive;
	out_candidate->luminance = R_PPdlights_CandidateLuminance (color, intensity);
	VectorSubtract (origin, r_refdef.vieworg, delta);
	distance = VectorLength (delta);
	out_candidate->camera_distance = distance;
	dist_factor = 1.f + distance;
	out_candidate->score = (radius * out_candidate->luminance) / dist_factor;

	sum = R_PPdlights_GetSourceSummary (source_id, true);
	if (sum)
		sum->offered_count++;
	return true;
}

static void R_PPdlights_CollectEmissiveFrame (rl_frame_light_candidate_t *best, int *best_count)
{
	const int emissive_budget = RL_EMISSIVE_BUDGET;
	int i;
	rl_frame_light_candidate_t candidate;

	if (!rl_emissive_enabled || cl_numvisedicts <= 0)
		return;

	for (i = 0; i < cl_numvisedicts; ++i)
	{
		entity_t *ent = cl_visedicts[i];
		vec3_t color;
		float radius = 0.f;
		float intensity = 0.f;
		unsigned int source_tag = 0u;
		int j;

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
		if (R_PPdlights_AddFrameLightCandidate (&candidate, ent->origin, radius, color, intensity,
			RL_LIGHT_EMISSIVE_PROXY,
			RL_LIGHT_SURFACE_CONTRIB | RL_LIGHT_VOLUMETRIC_CONTRIB,
			R_PPdlights_EmissiveSourceId (ent, source_tag),
			NULL,
			true))
		{
			int emissive_count = 0;
			int worst_emissive = -1;

			if (emissive_budget == 0)
			{
				rl_frame_stats.rejected_emissive_budget++;
				continue;
			}

			for (j = 0; j < *best_count; ++j)
			{
				if (best[j].type != RL_LIGHT_EMISSIVE_PROXY)
					continue;
				emissive_count++;
				worst_emissive = j;
			}

			if (emissive_count >= emissive_budget)
			{
				if (worst_emissive < 0
					|| !R_PPdlights_IsCandidateBetter (&candidate, &best[worst_emissive]))
				{
					rl_frame_stats.rejected_emissive_budget++;
					continue;
				}

				if (*best_count - 1 > worst_emissive)
					memmove (&best[worst_emissive], &best[worst_emissive + 1], (size_t)(*best_count - worst_emissive - 1) * sizeof (best[0]));
				(*best_count)--;
				rl_frame_stats.rejected_emissive_budget++;
			}

			R_PPdlights_InsertTopCandidate (best, best_count, &candidate);
		}
	}

	if (rl_debug_enabled && (r_framecount % 60) == 0)
	{
		int accepted_emissive = 0;
		for (i = 0; i < *best_count; ++i)
		{
			if (best[i].type == RL_LIGHT_EMISSIVE_PROXY)
				accepted_emissive++;
		}
		Con_DPrintf ("dlight_emissive: src=%d accepted=%d budget=%d/%d reject_budget=%d reject_priority=%d\n",
			rl_frame_stats.source_emissive,
			accepted_emissive,
			emissive_budget,
			RL_EMISSIVE_BUDGET,
			rl_frame_stats.rejected_emissive_budget,
			rl_frame_stats.rejected_priority);
	}
}

void R_PPdlights_CollectFrame (void)
{
	const dlight_t *const *active;
	rl_frame_light_candidate_t *best;
	int best_count = 0;
	int active_count = 0;
	int i;
	int mark;
	qboolean collect_enabled;

	rl_frame_light_count = 0;
	memset (rl_frame_light_sources, 0, sizeof (rl_frame_light_sources));
	memset (&rl_frame_stats, 0, sizeof (rl_frame_stats));
	memset (rl_consumer_stats, 0, sizeof (rl_consumer_stats));
	memset (rl_source_summaries, 0, sizeof (rl_source_summaries));
	rl_source_summary_count = 0;

	collect_enabled = (r_dynamic.value > 0.f);
	if (!collect_enabled)
		return;

	/*
	 * Architecture: this is the sole frame-level gather point.
	 * We merge dynamic + emissive contributors once, then downstream passes
	 * (world forward lights, alias/model forward lights) each
	 * consume filtered views of this same array in their own pass budgets.
	 */
	active = DLightPool_GetActiveList (&active_count);
	mark = Hunk_LowMark ();
	best = (rl_frame_light_candidate_t *)Hunk_Alloc ((int)((size_t)RL_FRAME_LIGHTS_MAX * sizeof (*best)));
	if (active && active_count > 0)
	{
		for (i = 0; i < active_count; ++i)
		{
			const dlight_t *dl = active[i];
			float radius = 0.f;
			vec3_t color;
			rl_frame_light_candidate_t candidate;

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

			R_EvaluateDLightForRender (dl, &radius, color);

			if (R_PPdlights_AddFrameLightCandidate (&candidate, dl->origin, radius, color, 1.f,
				RL_LIGHT_POINT,
				RL_LIGHT_SURFACE_CONTRIB | RL_LIGHT_VOLUMETRIC_CONTRIB,
				(unsigned int)dl->key,
				(dlight_t *)dl,
				false))
			{
				R_PPdlights_InsertTopCandidate (best, &best_count, &candidate);
			}
		}
	}

	R_PPdlights_CollectEmissiveFrame (best, &best_count);

	rl_frame_light_count = best_count;
	rl_frame_stats.accepted = rl_frame_light_count;
	rl_frame_stats.accepted_dlights = 0;
	rl_frame_stats.accepted_emissive = 0;
	for (i = 0; i < rl_frame_light_count; ++i)
	{
		rl_light_t *dst = &rl_frame_lights[i];
		VectorCopy (best[i].origin, dst->origin);
		dst->radius = best[i].radius;
		VectorCopy (best[i].color, dst->color);
		dst->intensity = best[i].intensity;
		dst->type = best[i].type;
		dst->flags = best[i].flags;
		dst->source_id = best[i].source_id;
		dst->reserved = (dst->type == RL_LIGHT_POINT && best[i].source != NULL)
			? (unsigned int)best[i].source->type
			: 0u;
		rl_frame_light_sources[i] = best[i].source;
		if (dst->type == RL_LIGHT_EMISSIVE_PROXY)
			rl_frame_stats.accepted_emissive++;
		else
			rl_frame_stats.accepted_dlights++;
	}

	if (rl_debug_enabled && (r_framecount % 60) == 0)
	{
		Con_DPrintf ("dlight_collect: src(d=%d e=%d) accepted(d=%d e=%d) frustum=%d budget=%d\n",
			rl_frame_stats.source_dlights,
			rl_frame_stats.source_emissive,
			rl_frame_stats.accepted_dlights,
			rl_frame_stats.accepted_emissive,
			rl_frame_stats.rejected_frustum,
			rl_frame_stats.rejected_budget);
	}

	Hunk_FreeToLowMark (mark);
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

int R_PPdlights_BuildWorldGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	return R_PPdlights_BuildGpuLightsForConsumer (RL_CONSUMER_WORLD, out_buffer, out_sources, max_lights);
}

int R_PPdlights_BuildModelGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	return R_PPdlights_BuildGpuLightsForConsumer (RL_CONSUMER_MODEL, out_buffer, out_sources, max_lights);
}

/*
 * Keep world/model consumer behavior bit-for-bit aligned here; if they need to
 * diverge later, branch from this helper in one explicit consumer-specific spot.
 */
static int R_PPdlights_BuildGpuLightsForConsumer (rl_light_consumer_t consumer, gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	int i;
	int count = 0;
	const qboolean require_dynamic_points =
		(consumer == RL_CONSUMER_WORLD || consumer == RL_CONSUMER_MODEL);

	if (!out_buffer || !out_sources || max_lights <= 0)
		return 0;

	for (i = 0; i < rl_frame_light_count; ++i)
	{
		const rl_light_t *src = &rl_frame_lights[i];
		gpulight_t *dst;
		float energy;
		R_PPdlights_RecordConsumerConsidered (consumer, src->source_id);
		if (require_dynamic_points && src->type != RL_LIGHT_POINT)
		{
			/* World/model consumers only accept runtime point dlights. */
			R_PPdlights_RecordConsumerReject (consumer, src->source_id, RL_REJECT_NON_CONTRIB);
			continue;
		}
		if ((src->flags & RL_LIGHT_SURFACE_CONTRIB) == 0u)
		{
			R_PPdlights_RecordConsumerReject (consumer, src->source_id, RL_REJECT_NON_CONTRIB);
			continue;
		}
		if (count >= max_lights)
		{
			R_PPdlights_RecordConsumerReject (consumer, src->source_id, RL_REJECT_LOCAL_BUDGET);
			continue;
		}
		dst = &out_buffer->lights[count];
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
		R_PPdlights_RecordConsumerAccept (consumer, src->source_id, energy);
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
