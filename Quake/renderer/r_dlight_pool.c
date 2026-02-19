#include "quakedef.h"
#include "renderer/r_dlight_pool.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

#define DLIGHT_POOL_BUDGET DLIGHT_GPU_MAX
#define DLIGHT_POOL_MAX 512
#define DLIGHT_POOL_MIN_RADIUS 8.0f
#define DLIGHT_POOL_MIN_BRIGHTNESS 0.02f
#define DLIGHT_POOL_HYSTERESIS_FRAMES 6
#define DLIGHT_POOL_SMOOTH 0.25f

#define DLIGHT_DEBUG_SAMPLE_MAX 4

static dlight_filter_debug_t dlight_filter_debug;
static dlight_debug_entry_t dlight_debug_samples[DLIGHT_DEBUG_SAMPLE_MAX];

extern cvar_t r_clustered_light_debug;

static void DLightPool_DebugReset (void)
{
	memset (&dlight_filter_debug, 0, sizeof (dlight_filter_debug));
	dlight_filter_debug.min_radius = FLT_MAX;
	dlight_filter_debug.max_radius = 0.f;
	memset (dlight_debug_samples, 0, sizeof (dlight_debug_samples));
}

static dlight_debug_entry_t *DLightPool_DebugCapture (const dlight_t *dl)
{
	for (int i = 0; i < DLIGHT_DEBUG_SAMPLE_MAX; i++)
	{
		if (dlight_debug_samples[i].captured)
			continue;
		dlight_debug_entry_t *entry = &dlight_debug_samples[i];
		memset (entry, 0, sizeof (*entry));
		entry->captured = true;
		entry->id = dl ? dl->id : 0;
		if (dl)
		{
			VectorCopy (dl->origin, entry->origin);
			VectorCopy (dl->color, entry->color);
			entry->radius = dl->radius;
			entry->baseradius = dl->baseradius;
			entry->die = dl->die;
			entry->flags = dl->flags;
		}
		entry->reason = DLIGHT_REJECT_NONE;
		return entry;
	}
	return NULL;
}

static void DLightPool_DebugSetReason (dlight_debug_entry_t *entry, dlight_reject_reason_t reason)
{
	if (!entry || entry->reason != DLIGHT_REJECT_NONE)
		return;
	entry->reason = reason;
}

const char *DLightPool_RejectReasonName (dlight_reject_reason_t reason)
{
	switch (reason)
	{
	default:
	case DLIGHT_REJECT_NONE: return "accepted";
	case DLIGHT_REJECT_INACTIVE: return "inactive";
	case DLIGHT_REJECT_LIFETIME: return "lifetime/radius";
	case DLIGHT_REJECT_RADIUS: return "radius";
	case DLIGHT_REJECT_WORLD_FLAG: return "world_flag";
	case DLIGHT_REJECT_PVS: return "pvs";
	case DLIGHT_REJECT_FRUSTUM: return "frustum";
	case DLIGHT_REJECT_BUDGET: return "budget";
	case DLIGHT_REJECT_OTHER: return "other";
	}
}

void DLightPool_GetFilterDebug (dlight_filter_debug_t *out_stats, dlight_debug_entry_t out_entries[4])
{
	if (out_stats)
		*out_stats = dlight_filter_debug;
	if (out_entries)
		memcpy (out_entries, dlight_debug_samples, sizeof (dlight_debug_samples));
}


int DLightPool_GetBudget (void)
{
	return DLIGHT_POOL_BUDGET;
}

static int DLightPool_GetPoolMax (void)
{
	return DLIGHT_POOL_MAX;
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
	DLightPool_DebugReset ();
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
	dlight_t **scratch;

	if (needed <= dlight_pool.scratch_capacity)
		return;

	scratch = (dlight_t **)realloc (dlight_pool.scratch, sizeof (dlight_pool.scratch[0]) * needed);
	if (!scratch)
	{
		Con_Printf ("WARNING: failed to grow dlight scratch buffer to %d entries\n", needed);
		return;
	}

	dlight_pool.scratch = scratch;
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

	dlight_t *items = (dlight_t *)realloc (dlight_pool.items, sizeof (dlight_pool.items[0]) * new_capacity);
	if (!items)
	{
		Con_Printf ("WARNING: failed to grow dlight pool to %d entries\n", new_capacity);
		return;
	}

	dlight_pool.items = items;
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
	dl->lod_scale = 1.f;
	dl->selected_until_frame = 0;
	dl->last_frame_touched = dlight_pool.framecount;
	dl->flags = DLIGHTF_DEFAULT;
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
	influence = (radius * q_max (dl->lod_scale, 0.01f)) / q_max (dist, 1.f);
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

	if (dl->selected_until_frame >= dlight_pool.framecount)
		bias *= 1.1f;

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
		if (dl->kind == DL_TRANSIENT && (dl->die < time || dl->spawn > time))
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
	DLightPool_DebugReset ();

	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		if (!dl->active)
			continue;
		if (dl->kind == DL_TRANSIENT && dl->die < time)
		{
			dl->active = false;
			dlight_pool.stats.expired++;
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

	dlight_filter_debug.created_count = dlight_pool.next_id - 1;

	DLightPool_EnsureScratch (dlight_pool.capacity);
	if (!dlight_pool.scratch || dlight_pool.scratch_capacity < dlight_pool.capacity)
		return NULL;

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
	const float min_radius = DLIGHT_POOL_MIN_RADIUS;
	const float min_brightness = DLIGHT_POOL_MIN_BRIGHTNESS;
	const float cull_distance = 0.f;
	const float smooth = DLIGHT_POOL_SMOOTH;
	qboolean have_world = (cl.worldmodel != NULL && cl.worldmodel->leafs != NULL);
	byte *view_pvs = NULL;

	dlight_pool.time = time;
	VectorCopy (vieworg, dlight_pool.last_vieworg);
	dlight_pool.has_vieworg = true;
	DLightPool_DebugReset ();

	if (viewleaf && have_world)
		view_pvs = Mod_LeafPVS ((mleaf_t *)viewleaf, cl.worldmodel);

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

	dlight_filter_debug.created_count = dlight_pool.next_id - 1;

	DLightPool_EnsureScratch (dlight_pool.capacity);
	if (!dlight_pool.scratch || dlight_pool.scratch_capacity < dlight_pool.capacity)
	{
		dlight_pool.stats.submitted = 0;
		return 0;
	}

	int found = 0;
	for (int i = 0; i < dlight_pool.capacity; i++)
	{
		dlight_t *dl = &dlight_pool.items[i];
		float lum;
		float dist;
		float target_scale;
		vec3_t delta;
		vec3_t mins, maxs;
		qboolean in_pvs = true;
		qboolean in_frustum = true;
		dlight_debug_entry_t *dbg;

		if (!dl->active)
			continue;

		dlight_filter_debug.after_merge_count++;
		dlight_filter_debug.total_active++;
		dbg = DLightPool_DebugCapture (dl);

		if (dl->kind == DL_TRANSIENT && dl->die < time)
		{
			dl->active = false;
			dlight_pool.stats.expired++;
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_LIFETIME);
			dlight_filter_debug.rejected_lifetime_radius++;
			continue;
		}

		if (dl->spawn > time || !dl->baseradius || (dl->radius < min_radius && dl->baseradius < min_radius))
		{
			if (dl->spawn > time)
				dl->die = 0.f;
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_LIFETIME);
			dlight_filter_debug.rejected_lifetime_radius++;
			continue;
		}
		dlight_filter_debug.pass_lifetime_radius++;
		dlight_filter_debug.min_radius = q_min (dlight_filter_debug.min_radius, dl->radius);
		dlight_filter_debug.max_radius = q_max (dlight_filter_debug.max_radius, dl->radius);

		if ((dl->flags & DLIGHTF_AFFECTS_WORLD) == 0)
		{
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_WORLD_FLAG);
			dlight_filter_debug.rejected_world_flag++;
			continue;
		}
		dlight_filter_debug.pass_world_flag++;

		lum = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));
		if (lum <= 0.f || lum < min_brightness)
		{
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_LIFETIME);
			dlight_filter_debug.rejected_lifetime_radius++;
			continue;
		}

		VectorSubtract (vieworg, dl->origin, delta);
		dist = VectorLength (delta);
		if (cull_distance > 0.f && dist > cull_distance + q_max (dl->radius, dl->baseradius))
		{
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_LIFETIME);
			dlight_filter_debug.rejected_lifetime_radius++;
			continue;
		}

		target_scale = 1.f;
		if (dl->radius > 0.f && dl->radius < min_radius)
			target_scale = q_max (0.1f, dl->radius / min_radius);
		dl->lod_scale += (target_scale - dl->lod_scale) * smooth;
		dl->lod_scale = CLAMP (0.1f, dl->lod_scale, 1.f);

		VectorSet (mins, dl->origin[0] - dl->radius, dl->origin[1] - dl->radius, dl->origin[2] - dl->radius);
		VectorSet (maxs, dl->origin[0] + dl->radius, dl->origin[1] + dl->radius, dl->origin[2] + dl->radius);
		if (dbg)
		{
			VectorCopy (mins, dbg->mins);
			VectorCopy (maxs, dbg->maxs);
		}

		if (have_world && viewleaf && view_pvs)
		{
			mleaf_t *lightleaf = Mod_PointInLeaf (dl->origin, cl.worldmodel);
			if (dbg)
			{
				dbg->has_leaf = (lightleaf != NULL);
				dbg->leaf_index = lightleaf ? (int)(lightleaf - cl.worldmodel->leafs) : -1;
			}
			if (!lightleaf)
				in_pvs = false;
			else
			{
				int leafnum = (int)(lightleaf - cl.worldmodel->leafs) - 1;
				if (leafnum >= 0)
					in_pvs = ((view_pvs[leafnum >> 3] & (1 << (leafnum & 7))) != 0);
			}
		}
		if (dbg)
			dbg->in_pvs = in_pvs;
		if (!in_pvs)
		{
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_PVS);
			dlight_filter_debug.rejected_pvs++;
			continue;
		}
		dlight_filter_debug.pass_pvs++;

		for (int j = 0; j < 4; j++)
		{
			const mplane_t *p = &frustum[j];
			if (DotProduct (p->normal, dl->origin) - p->dist + dl->radius < 0.f)
			{
				in_frustum = false;
				break;
			}
		}
		if (dbg)
			dbg->in_frustum = in_frustum;
		if (!in_frustum)
		{
			DLightPool_DebugSetReason (dbg, DLIGHT_REJECT_FRUSTUM);
			dlight_filter_debug.rejected_frustum++;
			continue;
		}
		dlight_filter_debug.pass_frustum++;

		dl->last_score = DLightPool_ScoreLight (dl, time, vieworg);
		dlight_pool.scratch[found++] = dl;
	}

	if (found > 1)
		qsort (dlight_pool.scratch, found, sizeof (dlight_pool.scratch[0]), DLightPool_CompareScore);

	const int submit_count = q_min (found, out_max);
	for (int i = 0; i < submit_count; i++)
	{
		out[i] = dlight_pool.scratch[i];
		out[i]->selected_until_frame = dlight_pool.framecount + DLIGHT_POOL_HYSTERESIS_FRAMES;
	}

	dlight_filter_debug.pass_budget = submit_count;
	if (found > submit_count)
		dlight_filter_debug.rejected_budget = found - submit_count;
	for (int i = submit_count; i < found; i++)
	{
		dlight_t *dl = dlight_pool.scratch[i];
		for (int j = 0; j < DLIGHT_DEBUG_SAMPLE_MAX; j++)
		{
			dlight_debug_entry_t *entry = &dlight_debug_samples[j];
			if (!entry->captured || entry->id != dl->id)
				continue;
			DLightPool_DebugSetReason (entry, DLIGHT_REJECT_BUDGET);
			break;
		}
	}

	dlight_pool.stats.submitted = submit_count;
	dlight_filter_debug.final_active_count = submit_count;
	if (dlight_filter_debug.pass_lifetime_radius <= 0)
		dlight_filter_debug.min_radius = 0.f;
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

void DLightPool_DebugPrint (void)
{
	if (r_clustered_light_debug.value < 2.f)
		return;

	Con_Printf ("DLIGHTDBG frame=%d created_count=%d after_merge_count=%d after_lifetime_radius_count=%d rejected_lifetime=%d after_world_count=%d rejected_world=%d after_pvs_count=%d rejected_pvs=%d after_frustum_count=%d rejected_frustum=%d after_budget_count=%d rejected_budget=%d final_active_count=%d radius_min=%.1f radius_max=%.1f\n",
		dlight_pool.framecount,
		dlight_filter_debug.created_count,
		dlight_filter_debug.after_merge_count,
		dlight_filter_debug.pass_lifetime_radius,
		dlight_filter_debug.rejected_lifetime_radius,
		dlight_filter_debug.pass_world_flag,
		dlight_filter_debug.rejected_world_flag,
		dlight_filter_debug.pass_pvs,
		dlight_filter_debug.rejected_pvs,
		dlight_filter_debug.pass_frustum,
		dlight_filter_debug.rejected_frustum,
		dlight_filter_debug.pass_budget,
		dlight_filter_debug.rejected_budget,
		dlight_filter_debug.final_active_count,
		dlight_filter_debug.min_radius,
		dlight_filter_debug.max_radius);

	for (int i = 0; i < DLIGHT_DEBUG_SAMPLE_MAX; i++)
	{
		const dlight_debug_entry_t *entry = &dlight_debug_samples[i];
		if (!entry->captured)
			continue;
		Con_Printf ("DLIGHTDBG sample[%d] id=%d pos=(%.1f %.1f %.1f) radius=%.1f color=(%.2f %.2f %.2f) die=%.3f reason=%s\n",
			i,
			entry->id,
			entry->origin[0], entry->origin[1], entry->origin[2],
			entry->radius,
			entry->color[0], entry->color[1], entry->color[2],
			entry->die,
			DLightPool_RejectReasonName (entry->reason));
	}
}
