#include "quakedef.h"
#include "r_dlight_pool.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern cvar_t r_dlight_debug;
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

cvar_t r_dlight_budget = { "r_dlight_budget", "64", CVAR_ARCHIVE };
cvar_t r_dlight_pool_max = { "r_dlight_pool_max", "512", CVAR_ARCHIVE };

int DLightPool_GetBudget (void)
{
	return CLAMP (0, (int)r_dlight_budget.value, 1024);
}

static int DLightPool_GetPoolMax (void)
{
	return CLAMP (0, (int)r_dlight_pool_max.value, 8192);
}

static void DLightPool_ClampCvar (cvar_t *var, int minval, int maxval)
{
	const int value = (int)var->value;
	const int clamped = CLAMP (minval, value, maxval);
	if (clamped != value)
		Cvar_SetValueQuick (var, (float)clamped);
}

static void DLightPool_Budget_Changed (cvar_t *var)
{
	DLightPool_ClampCvar (var, 0, 1024);
}

static void DLightPool_PoolMax_Changed (cvar_t *var)
{
	DLightPool_ClampCvar (var, 0, 8192);
}

void DLightPool_RegisterCvars (void)
{
	Cvar_RegisterVariable (&r_dlight_budget);
	Cvar_RegisterVariable (&r_dlight_pool_max);
	Cvar_SetCallback (&r_dlight_budget, DLightPool_Budget_Changed);
	Cvar_SetCallback (&r_dlight_pool_max, DLightPool_PoolMax_Changed);
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
	free (dlight_pool.items);
	free (dlight_pool.scratch);
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

static void DLightPool_EnsureScratch (int needed)
{
	if (needed <= dlight_pool.scratch_capacity)
		return;

	dlight_pool.scratch = (dlight_t **)realloc (dlight_pool.scratch, sizeof (dlight_pool.scratch[0]) * needed);
	dlight_pool.scratch_capacity = needed;
}

static void DLightPool_EnsureCapacity (int desired)
{
	const int pool_max = DLightPool_GetPoolMax ();
	if (desired > pool_max)
		desired = pool_max;
	if (desired <= dlight_pool.capacity)
		return;

	int new_capacity = dlight_pool.capacity ? dlight_pool.capacity * 2 : 64;
	if (new_capacity < desired)
		new_capacity = desired;
	if (new_capacity > pool_max)
		new_capacity = pool_max;
	if (new_capacity <= dlight_pool.capacity)
		return;

	dlight_pool.items = (dlight_t *)realloc (dlight_pool.items, sizeof (dlight_pool.items[0]) * new_capacity);
	memset (dlight_pool.items + dlight_pool.capacity, 0, sizeof (dlight_pool.items[0]) * (new_capacity - dlight_pool.capacity));
	dlight_pool.capacity = new_capacity;
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

	if (dl->kind == DL_TRANSIENT)
	{
		float age = (float)(time - dl->spawn_time);
		float boost = 1.f - age * 0.5f;
		boost = CLAMP (0.5f, boost, 1.f);
		bias *= boost;
	}
	else
	{
		bias *= 1.05f;
	}

	return influence * lum * bias;
}

static int DLightPool_CompareScore (const void *a, const void *b)
{
	const dlight_t *dl_a = *(const dlight_t *const *)a;
	const dlight_t *dl_b = *(const dlight_t *const *)b;

	if (dl_a->last_score > dl_b->last_score)
		return -1;
	if (dl_a->last_score < dl_b->last_score)
		return 1;
	if (dl_a->spawn_time > dl_b->spawn_time)
		return -1;
	if (dl_a->spawn_time < dl_b->spawn_time)
		return 1;
	if (dl_a->id < dl_b->id)
		return -1;
	if (dl_a->id > dl_b->id)
		return 1;
	return 0;
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

		if (CL_DlightShouldFlicker (dl))
			dl->radius = dl->baseradius * (1.0f + 0.1f * (float) sin (time * 9.0 + dl->flicker_seed));
		else
			dl->radius = dl->baseradius;
		if (dl->radius < 0.f)
			dl->radius = 0.f;
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

	int found = 0;
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!CL_DlightIsActive (dl))
			continue;
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

	int selected = 0;
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!dl->active)
			continue;

		if (dl->kind == DL_PERSISTENT && r_dlight_entities.value <= 0.f)
			continue;

		qboolean expired = false;
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
			int insert_at = selected;
			while (insert_at > 0 && DLightPool_IsBetterScore (dl, dlight_pool.scratch[insert_at - 1]))
			{
				dlight_pool.scratch[insert_at] = dlight_pool.scratch[insert_at - 1];
				insert_at--;
			}
			dlight_pool.scratch[insert_at] = dl;
			selected++;
		}
		else if (selected > 0 && DLightPool_IsBetterScore (dl, dlight_pool.scratch[selected - 1]))
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

void DLightPool_DebugPrintIfEnabled (void)
{
	if (r_dlight_debug.value <= 0.f)
		return;

	DLightPool_UpdateStats ();

	Con_DPrintf ("pool active=%d persistent=%d transient=%d submitted=%d expired=%d evicted=%d\n",
			dlight_pool.stats.active,
			dlight_pool.stats.persistent,
			dlight_pool.stats.transient,
			dlight_pool.stats.submitted,
			dlight_pool.stats.expired,
			dlight_pool.stats.evicted);
}
