#include "quakedef.h"
#include "r_dlight_pool.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>

extern cvar_t r_dlight_entities;

typedef struct dlight_pool_s
{
	dlight_t *items;
	int capacity;
	int next_id;
	int framecount;
	double time;
	vec3_t last_vieworg;
	qboolean has_vieworg;
	dlight_t **scratch;
	int scratch_capacity;
	dlight_pool_stats_t stats;
} dlight_pool_t;

static dlight_pool_t dlight_pool;
static dlight_t dlight_fallback;
static const int k_dlight_budget = 64;
static const int k_dlight_pool_max = 512;

int DLightPool_GetBudget (void)
{
	return k_dlight_budget;
}

static int DLightPool_GetPoolMax (void)
{
	return k_dlight_pool_max;
}

static void DLightPool_ResetStats (void)
{
	memset (&dlight_pool.stats, 0, sizeof (dlight_pool.stats));
}

void DLightPool_Init (void)
{
	memset (&dlight_pool, 0, sizeof (dlight_pool));
	memset (&dlight_fallback, 0, sizeof (dlight_fallback));
	dlight_pool.next_id = 1;
}

void DLightPool_Shutdown (void)
{
	q_free(dlight_pool.items);
	q_free(dlight_pool.scratch);
	DLightPool_Init ();
}

void DLightPool_Clear (void)
{
	if (dlight_pool.items && dlight_pool.capacity > 0)
		memset (dlight_pool.items, 0, sizeof (dlight_pool.items[0]) * dlight_pool.capacity);
	dlight_pool.next_id = 1;
	DLightPool_ResetStats ();
}

void DLightPool_ClearPersistent (void)
{
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (dl->active && dl->kind == DL_PERSISTENT)
			dl->active = false;
	}
}

static qboolean DLightPool_EnsureScratch (int needed)
{
	if (needed <= dlight_pool.scratch_capacity)
		return true;

	dlight_t **new_scratch = (dlight_t **)q_realloc(dlight_pool.scratch, sizeof (dlight_pool.scratch[0]) * needed);
	if (!new_scratch)
	{
		Con_DPrintf ("DLightPool_EnsureScratch: allocation failed for %d entries (capacity stays %d)\n",
				needed, dlight_pool.scratch_capacity);
		return false;
	}

	dlight_pool.scratch = new_scratch;
	dlight_pool.scratch_capacity = needed;
	return true;
}

static qboolean DLightPool_EnsureCapacity (int desired)
{
	const int pool_max = DLightPool_GetPoolMax ();
	if (desired > pool_max)
		desired = pool_max;
	if (desired <= dlight_pool.capacity)
		return true;

	int new_capacity = dlight_pool.capacity ? dlight_pool.capacity * 2 : 64;
	if (new_capacity < desired)
		new_capacity = desired;
	if (new_capacity > pool_max)
		new_capacity = pool_max;
	if (new_capacity <= dlight_pool.capacity)
		return true;

	dlight_t *new_items = (dlight_t *)q_realloc(dlight_pool.items, sizeof (dlight_pool.items[0]) * new_capacity);
	if (!new_items)
	{
		Con_Warning ("DLightPool_EnsureCapacity: allocation failed for %d lights (capacity stays %d)\n",
				new_capacity, dlight_pool.capacity);
		return false;
	}

	dlight_pool.items = new_items;
	memset (dlight_pool.items + dlight_pool.capacity, 0, sizeof (dlight_pool.items[0]) * (new_capacity - dlight_pool.capacity));
	dlight_pool.capacity = new_capacity;
	return true;
}

static void DLightPool_ResetLight (dlight_t *dl, dlight_kind_t kind, double time)
{
	memset (dl, 0, sizeof (*dl));
	dl->id = dlight_pool.next_id++;
	dl->kind = kind;
	dl->active = true;
	dl->spawn_time = (float)time;
	dl->spawn = (float)time;
	dl->die = (kind == DL_PERSISTENT) ? FLT_MAX : (float)time;
	dl->last_frame_touched = dlight_pool.framecount;
}

static float DLightPool_ScoreLight (dlight_t *dl, double time, const vec3_t vieworg)
{
	vec3_t delta;
	float dist;
	float influence;
	float lum;
	float bias = 1.f;
	float radius = dl->radius > 0.f ? dl->radius : dl->baseradius;

	VectorSubtract (vieworg, dl->origin, delta);
	dist = VectorLength (delta);
	influence = radius / q_max (dist, 1.f);
	lum = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));

	/* Under budget pressure, prioritize transient gameplay lights over persistent
	 * map/entity dlights so muzzle/projectile effects do not disappear. */
	if (dl->kind == DL_TRANSIENT)
	{
		float age = (float)(time - dl->spawn_time);
		float freshness = 1.f - age * 0.35f;
		freshness = CLAMP (0.65f, freshness, 1.0f);
		bias *= 1.45f * freshness;
	}
	else
	{
		bias *= 0.75f;
	}

	switch (dl->type)
	{
	case DLIGHT_DEFAULT:
		bias *= 1.30f;
		break;
	case DLIGHT_ROCKET:
		bias *= 1.9f;
		break;
	case DLIGHT_EXPLOSION:
		bias *= 1.6f;
		break;
	case DLIGHT_PLASMA:
		bias *= 1.5f;
		break;
	case DLIGHT_LIGHTNING:
		bias *= 1.35f;
		break;
	case DLIGHT_LAVA:
		bias *= 1.45f;
		break;
	case DLIGHT_TORCH:
		bias *= 0.85f;
		break;
	default:
		break;
	}

	return influence * lum * bias;
}

static qboolean DLightPool_IsBetterScore (const dlight_t *candidate, const dlight_t *current)
{
	if (candidate->last_score > current->last_score)
		return true;
	if (candidate->last_score < current->last_score)
		return false;
	if (candidate->spawn_time > current->spawn_time)
		return true;
	if (candidate->spawn_time < current->spawn_time)
		return false;
	if (candidate->id < current->id)
		return true;
	return false;
}

static void DLightPool_InsertScored (dlight_t **dst, int *count, int max_count, dlight_t *dl)
{
	int insert_at;

	if (!dst || !count || max_count <= 0 || !dl)
		return;

	if (*count < max_count)
	{
		insert_at = *count;
		while (insert_at > 0 && DLightPool_IsBetterScore (dl, dst[insert_at - 1]))
		{
			dst[insert_at] = dst[insert_at - 1];
			insert_at--;
		}
		dst[insert_at] = dl;
		(*count)++;
		return;
	}

	/* Caller decides replacement policy when full. */
}

static dlight_t *DLightPool_EvictWorstTransient (double time)
{
	int best_index = -1;
	float best_score = FLT_MAX;

	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!dl->active || dl->kind != DL_TRANSIENT)
			continue;

		if (dl->die < time)
			continue;

		float score = 0.f;
		if (dlight_pool.has_vieworg)
			score = DLightPool_ScoreLight (dl, dlight_pool.time, dlight_pool.last_vieworg);

		if (score < best_score)
		{
			best_score = score;
			best_index = i;
		}
		else if (score == best_score && best_index >= 0)
		{
			const dlight_t *best = &dlight_pool.items[best_index];
			if (dl->spawn_time < best->spawn_time || (dl->spawn_time == best->spawn_time && dl->id > best->id))
				best_index = i;
		}
	}

	if (best_index < 0)
		return NULL;

	dlight_pool.stats.evicted++;
	return &dlight_pool.items[best_index];
}

static dlight_t *DLightPool_AcquireSlot (double time)
{
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!dl->active)
			return dl;
		if (dl->kind == DL_TRANSIENT && !CL_DlightTransientIsLiveAtTime (dl, time, NULL))
			return dl;
	}

	DLightPool_EnsureCapacity (dlight_pool.capacity + 1);
	if (dlight_pool.capacity > 0)
	{
		for (int i = 0; i < dlight_pool.capacity; i++)
		{
			dlight_t *dl = &dlight_pool.items[i];
			if (!dl->active)
				return dl;
		}
	}

	dlight_t *evicted = DLightPool_EvictWorstTransient (time);
	if (evicted)
		return evicted;

	dlight_pool.stats.evicted++;
	return &dlight_fallback;
}

dlight_t *DLightPool_AllocTransient (double time)
{
	dlight_t *dl = DLightPool_AcquireSlot (time);
	DLightPool_ResetLight (dl, DL_TRANSIENT, time);
	return dl;
}

dlight_t *DLightPool_AllocTransientByKey (int key, double time)
{
	if (key)
	{
		for (int i = 0; i < dlight_pool.capacity; i++)
		{
			dlight_t *dl = &dlight_pool.items[i];
			if (dl->active && dl->kind == DL_TRANSIENT && dl->key == key)
			{
				DLightPool_ResetLight (dl, DL_TRANSIENT, time);
				dl->key = key;
				return dl;
			}
		}
	}

	dlight_t *dl = DLightPool_AcquireSlot (time);
	DLightPool_ResetLight (dl, DL_TRANSIENT, time);
	dl->key = key;
	return dl;
}

dlight_t *DLightPool_GetOrCreatePersistent (int key, double time)
{
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (dl->active && dl->kind == DL_PERSISTENT && dl->key == key)
		{
			dl->last_frame_touched = dlight_pool.framecount;
			return dl;
		}
	}

	dlight_t *dl = DLightPool_AcquireSlot (time);
	DLightPool_ResetLight (dl, DL_PERSISTENT, time);
	dl->key = key;
	return dl;
}

void DLightPool_KillByKey (int key)
{
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (dl->active && dl->key == key)
		{
			dl->active = false;
			break;
		}
	}
}

void DLightPool_NewFrame (double time, int framecount)
{
	dlight_pool.time = time;
	dlight_pool.framecount = framecount;
	dlight_pool.has_vieworg = false;
	DLightPool_ResetStats ();

	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!dl->active)
			continue;
		qboolean expired = false;
		if (!CL_DlightTransientIsLiveAtTime (dl, time, &expired))
		{
			if (expired)
				dlight_pool.stats.expired++;
			dl->active = false;
		}
	}
}

void DLightPool_Decay (float frametime, double time)
{
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!dl->active)
			continue;
		if (dl->die < time || dl->spawn > time || !dl->baseradius)
			continue;

		dl->baseradius -= frametime * dl->decay;
		if (dl->baseradius < 0.f)
			dl->baseradius = 0.f;

		/*
		Keep pool state authoritative for raw light values only.
		Type-specific flicker is applied in R_PushDlightArray so there is a single
		submit-time source of truth for radius/color modulation.
		*/
		dl->radius = dl->baseradius;
	}
}

static void DLightPool_UpdateStats (void)
{
	dlight_pool.stats.active = 0;
	dlight_pool.stats.persistent = 0;
	dlight_pool.stats.transient = 0;

	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		const dlight_t *dl = &dlight_pool.items[i];
		if (!CL_DlightIsActive (dl))
			continue;
		dlight_pool.stats.active++;
		if (dl->kind == DL_PERSISTENT)
			dlight_pool.stats.persistent++;
		else
			dlight_pool.stats.transient++;
	}
}

const dlight_t *const *DLightPool_GetActiveList (int *count)
{
	if (count)
		*count = 0;

	if (!dlight_pool.items || dlight_pool.capacity <= 0)
		return NULL;

	DLightPool_EnsureScratch (dlight_pool.capacity);

	const int max_scratch = q_min (dlight_pool.capacity, dlight_pool.scratch_capacity);
	if (!dlight_pool.scratch || max_scratch <= 0)
		return NULL;

	int found = 0;
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!CL_DlightIsActive (dl))
			continue;
		if (found >= max_scratch)
			break;
		dlight_pool.scratch[found++] = dl;
	}

	if (count)
		*count = found;
	return (const dlight_t *const *)dlight_pool.scratch;
}

int DLightPool_CollectForRender (double time, const vec3_t vieworg, const mleaf_t *viewleaf,
		dlight_t **out, int out_max)
{
	(void)viewleaf;

	dlight_pool.time = time;
	VectorCopy (vieworg, dlight_pool.last_vieworg);
	dlight_pool.has_vieworg = true;

	if (!out || out_max <= 0)
	{
		dlight_pool.stats.submitted = 0;
		return 0;
	}

	if (!dlight_pool.items || dlight_pool.capacity <= 0)
	{
		dlight_pool.stats.submitted = 0;
		return 0;
	}

	const int selection_max = q_min (out_max, dlight_pool.capacity);
	DLightPool_EnsureScratch (selection_max);

	const int max_scratch = q_min (dlight_pool.capacity, dlight_pool.scratch_capacity);
	if (!dlight_pool.scratch || max_scratch <= 0)
	{
		dlight_pool.stats.submitted = 0;
		DLightPool_UpdateStats ();
		return 0;
	}

	int selected = 0;
	for (int pass = 0; pass < 2; pass++)
	{
		for (int i = 0; i < dlight_pool.capacity; i++)
		{
			dlight_t *dl;
			qboolean expired = false;

			if (selected >= max_scratch && pass != 0)
				break;

			dl = &dlight_pool.items[i];
			if (!dl->active)
				continue;

			/* Pass 0: reserve budget for transient/gameplay lights first.
			 * Pass 1: fill leftover budget with persistent lights. */
			if (pass == 0 && dl->kind != DL_TRANSIENT)
				continue;
			if (pass == 1 && dl->kind == DL_TRANSIENT)
				continue;

			if (dl->kind == DL_PERSISTENT && r_dlight_entities.value <= 0.f)
				continue;

			if (!CL_DlightTransientIsLiveAtTime (dl, time, &expired))
			{
				if (expired)
				{
					dlight_pool.stats.expired++;
					dl->active = false;
				}
				continue;
			}

			if (!dl->baseradius)
				continue;

			if (dl->color[0] <= 0.f && dl->color[1] <= 0.f && dl->color[2] <= 0.f)
				continue;

			dl->last_score = DLightPool_ScoreLight (dl, time, vieworg);

			if (selected < selection_max)
			{
				DLightPool_InsertScored (dlight_pool.scratch, &selected, selection_max, dl);
			}
			else if (pass == 0 && selected > 0 && DLightPool_IsBetterScore (dl, dlight_pool.scratch[selected - 1]))
			{
				int insert_at = selected - 1;
				while (insert_at > 0 && DLightPool_IsBetterScore (dl, dlight_pool.scratch[insert_at - 1]))
				{
					dlight_pool.scratch[insert_at] = dlight_pool.scratch[insert_at - 1];
					insert_at--;
				}
				dlight_pool.scratch[insert_at] = dl;
			}
		}
	}

	const int submit_count = selected;
	for (int i = 0; i < submit_count; i++)
		out[i] = dlight_pool.scratch[i];

	dlight_pool.stats.submitted = submit_count;
	DLightPool_UpdateStats ();

	return submit_count;
}

void DLightPool_GetStats (dlight_pool_stats_t *out)
{
	if (!out)
		return;
	DLightPool_UpdateStats ();
	*out = dlight_pool.stats;
}
