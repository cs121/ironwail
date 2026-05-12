#include "quakedef.h"

typedef struct jobnode_s {
	jobs_func_t func;
	void *userdata;
	JobHandle *handle;
	qboolean detached;
	jobs_priority_t priority;
} jobnode_t;

typedef struct jobs_ring_s {
	jobnode_t *items;
	size_t head;
	size_t tail;
	size_t count;
	size_t capacity;
} jobs_ring_t;

typedef enum jobs_runtime_state_e {
	JOBS_STATE_STOPPED = 0,
	JOBS_STATE_RUNNING,
	JOBS_STATE_SHUTTING_DOWN
} jobs_runtime_state_t;

static SDL_Thread **jobs_threads;
static int jobs_num_threads;
static SDL_mutex *jobs_mutex;
static SDL_cond *jobs_not_empty;
static SDL_cond *jobs_not_full;
static jobs_ring_t jobs_queues[JOBS_PRIORITY_COUNT];
static jobs_runtime_state_t jobs_state = JOBS_STATE_STOPPED;
/* Queue stats are protected by jobs_mutex. Read/write them only while holding that lock. */
static unsigned int jobs_pending;
static unsigned int jobs_peak_pending;
static unsigned int jobs_dropped;
static unsigned int jobs_queue_full_hits;
static unsigned int jobs_submit_failed;
static unsigned int jobs_sync_fallbacks;
static unsigned int jobs_wake_signals;
static unsigned int jobs_wake_broadcasts;
static unsigned int jobs_overflow_sync;
static unsigned int jobs_overflow_drop;
static unsigned int jobs_overflow_fail;
static unsigned int jobs_overflow_block_waits;
static unsigned int jobs_overflow_block_timeouts;

typedef enum jobs_overflow_policy_e {
	JOBS_OVERFLOW_SYNC,
	JOBS_OVERFLOW_DROP,
	JOBS_OVERFLOW_BLOCK,
	JOBS_OVERFLOW_FAIL
} jobs_overflow_policy_t;

static qboolean Jobs_SubmitNode (const jobnode_t *node);
static jobs_overflow_policy_t Jobs_GetOverflowPolicy (void);
static void Jobs_LogQueueStats (const char *reason);
static qboolean Jobs_EnsureRingCapacityLocked (jobs_ring_t *ring, size_t required);
static qboolean Jobs_EnqueueLocked (const jobnode_t *node);
static qboolean Jobs_DequeueLocked (jobnode_t *out);
static JobHandle *Jobs_CreateCompletedHandle (void);
static qboolean Jobs_IsAcceptingSubmissionsLocked (void);

static cvar_t host_async = {"host_async", "0", CVAR_ARCHIVE};
static cvar_t host_async_fs = {"host_async_fs", "0", CVAR_ARCHIVE};
static cvar_t host_async_assets = {"host_async_assets", "0", CVAR_ARCHIVE};
static cvar_t host_async_workers = {"host_async_workers", "1", CVAR_ARCHIVE};
static cvar_t host_async_max_pending = {"host_async_max_pending", "128", CVAR_ARCHIVE};
static cvar_t host_async_overflow_policy = {"host_async_overflow_policy", "sync", CVAR_ARCHIVE};
static cvar_t host_async_overflow_block_ms = {"host_async_overflow_block_ms", "2", CVAR_ARCHIVE};

qboolean Host_AsyncEnabled (void)
{
	return host_async.value != 0;
}

qboolean Host_AsyncFSEnabled (void)
{
	return host_async.value != 0 && host_async_fs.value != 0;
}

qboolean Host_AsyncAssetsEnabled (void)
{
	return Host_AsyncFSEnabled () && host_async_assets.value != 0;
}

static int Jobs_Worker (void *unused)
{
	while (1)
	{
		jobnode_t node;

		SDL_LockMutex (jobs_mutex);
		while (jobs_state == JOBS_STATE_RUNNING && jobs_pending == 0)
			SDL_CondWait (jobs_not_empty, jobs_mutex);
		if (jobs_state != JOBS_STATE_RUNNING && jobs_pending == 0)
		{
			SDL_UnlockMutex (jobs_mutex);
			break;
		}
		if (!Jobs_DequeueLocked (&node))
		{
			SDL_UnlockMutex (jobs_mutex);
			continue;
		}
		SDL_CondSignal (jobs_not_full);
		jobs_wake_signals++;
		SDL_UnlockMutex (jobs_mutex);

		node.func (node.userdata);
		if (node.detached)
			continue;
		SDL_LockMutex (node.handle->mutex);
		node.handle->done = true;
		SDL_CondSignal (node.handle->cond);
		SDL_UnlockMutex (node.handle->mutex);
	}
	return 0;
}

static jobs_overflow_policy_t Jobs_GetOverflowPolicy (void)
{
	const char *policy = host_async_overflow_policy.string;

	if (!q_strcasecmp (policy, "drop"))
		return JOBS_OVERFLOW_DROP;
	if (!q_strcasecmp (policy, "block"))
		return JOBS_OVERFLOW_BLOCK;
	if (!q_strcasecmp (policy, "fail"))
		return JOBS_OVERFLOW_FAIL;
	return JOBS_OVERFLOW_SYNC;
}

static JobHandle *Jobs_CreateCompletedHandle (void)
{
	JobHandle *handle;

	handle = (JobHandle *) q_calloc(1, sizeof (*handle));
	if (!handle)
		Sys_Error ("Jobs_CreateCompletedHandle: out of memory");
	handle->mutex = SDL_CreateMutex ();
	handle->cond = SDL_CreateCond ();
	if (!handle->mutex || !handle->cond)
		Sys_Error ("Jobs_CreateCompletedHandle: failed to create handle sync primitives");
	SDL_LockMutex (handle->mutex);
	handle->done = true;
	SDL_CondSignal (handle->cond);
	SDL_UnlockMutex (handle->mutex);
	return handle;
}

static qboolean Jobs_IsAcceptingSubmissionsLocked (void)
{
	return jobs_state == JOBS_STATE_RUNNING;
}

static qboolean Jobs_EnsureRingCapacityLocked (jobs_ring_t *ring, size_t required)
{
	jobnode_t *new_items;
	size_t new_capacity;

	if (ring->capacity >= required)
		return true;

	new_capacity = ring->capacity ? ring->capacity : 64;
	while (new_capacity < required)
		new_capacity <<= 1;

	new_items = (jobnode_t *) q_calloc(new_capacity, sizeof (*new_items));
	if (!new_items)
		return false;

	for (size_t i = 0; i < ring->count; ++i)
		new_items[i] = ring->items[(ring->head + i) & (ring->capacity - 1)];

	q_free(ring->items);
	ring->items = new_items;
	ring->capacity = new_capacity;
	ring->head = 0;
	ring->tail = ring->count;
	return true;
}

static qboolean Jobs_EnqueueLocked (const jobnode_t *node)
{
	jobs_ring_t *ring = &jobs_queues[node->priority];
	if (!Jobs_EnsureRingCapacityLocked (ring, ring->count + 1))
		return false;
	ring->items[ring->tail] = *node;
	ring->tail = (ring->tail + 1) & (ring->capacity - 1);
	ring->count++;
	jobs_pending++;
	if (jobs_pending > jobs_peak_pending)
		jobs_peak_pending = jobs_pending;
	return true;
}

static qboolean Jobs_DequeueLocked (jobnode_t *out)
{
	for (int prio = JOBS_PRIORITY_HIGH; prio < JOBS_PRIORITY_COUNT; ++prio)
	{
		jobs_ring_t *ring = &jobs_queues[prio];
		if (ring->count == 0)
			continue;
		*out = ring->items[ring->head];
		ring->head = (ring->head + 1) & (ring->capacity - 1);
		ring->count--;
		if (jobs_pending > 0)
			jobs_pending--;
		return true;
	}
	return false;
}

JobHandle *Jobs_SubmitPriority (jobs_priority_t priority, jobs_func_t func, void *userdata)
{
	JobHandle *handle;
	jobnode_t node;

	if (!func)
		Sys_Error ("Jobs_SubmitPriority: NULL function");

	if (priority < JOBS_PRIORITY_HIGH || priority >= JOBS_PRIORITY_COUNT)
		priority = JOBS_PRIORITY_NORMAL;

	if (!jobs_mutex)
	{
		handle = (JobHandle *) q_calloc(1, sizeof (*handle));
		if (!handle)
			Sys_Error ("Jobs_SubmitPriority: out of memory");
		handle->mutex = SDL_CreateMutex ();
		handle->cond = SDL_CreateCond ();
		if (!handle->mutex || !handle->cond)
			Sys_Error ("Jobs_SubmitPriority: failed to create handle sync primitives");
		func (userdata);
		SDL_LockMutex (handle->mutex);
		handle->done = true;
		SDL_CondSignal (handle->cond);
		SDL_UnlockMutex (handle->mutex);
		return handle;
	}

	SDL_LockMutex (jobs_mutex);
	if (!Jobs_IsAcceptingSubmissionsLocked ())
	{
		jobs_submit_failed++;
		SDL_UnlockMutex (jobs_mutex);
		return Jobs_CreateCompletedHandle ();
	}
	SDL_UnlockMutex (jobs_mutex);

	handle = (JobHandle *) q_calloc(1, sizeof (*handle));
	if (!handle)
		Sys_Error ("Jobs_SubmitPriority: out of memory");
	handle->mutex = SDL_CreateMutex ();
	handle->cond = SDL_CreateCond ();
	if (!handle->mutex || !handle->cond)
		Sys_Error ("Jobs_SubmitPriority: failed to create handle sync primitives");

	if (!Host_AsyncEnabled () || jobs_num_threads <= 0)
	{
		func (userdata);
		SDL_LockMutex (handle->mutex);
		handle->done = true;
		SDL_CondSignal (handle->cond);
		SDL_UnlockMutex (handle->mutex);
		return handle;
	}

	memset (&node, 0, sizeof (node));
	node.func = func;
	node.userdata = userdata;
	node.handle = handle;
	node.priority = priority;

	if (!Jobs_SubmitNode (&node))
	{
		qboolean do_sync_fallback = false;

		SDL_LockMutex (jobs_mutex);
		if (Jobs_IsAcceptingSubmissionsLocked ())
		{
			jobs_sync_fallbacks++;
			do_sync_fallback = true;
		}
		SDL_UnlockMutex (jobs_mutex);
		/* During active runtime, preserve existing sync fallback behavior. */
		if (do_sync_fallback)
		{
			func (userdata);
			SDL_LockMutex (handle->mutex);
			handle->done = true;
			SDL_CondSignal (handle->cond);
			SDL_UnlockMutex (handle->mutex);
		}
		else
		{
			SDL_LockMutex (handle->mutex);
			handle->done = true;
			SDL_CondSignal (handle->cond);
			SDL_UnlockMutex (handle->mutex);
		}
	}

	return handle;
}

JobHandle *Jobs_Submit (jobs_func_t func, void *userdata)
{
	return Jobs_SubmitPriority (JOBS_PRIORITY_NORMAL, func, userdata);
}

static qboolean Jobs_SubmitNode (const jobnode_t *node)
{
	int max_pending;
	jobs_overflow_policy_t policy;

	policy = Jobs_GetOverflowPolicy ();
	SDL_LockMutex (jobs_mutex);
	if (!Jobs_IsAcceptingSubmissionsLocked ())
	{
		jobs_submit_failed++;
		SDL_UnlockMutex (jobs_mutex);
		Jobs_LogQueueStats ("submit-shutdown");
		return false;
	}
	max_pending = (int) host_async_max_pending.value;
	while (max_pending > 0 && jobs_pending >= (unsigned int) max_pending)
	{
		int wait_ms;

		jobs_queue_full_hits++;
		switch (policy)
		{
		case JOBS_OVERFLOW_DROP:
			jobs_dropped++;
			jobs_overflow_drop++;
			jobs_submit_failed++;
			SDL_UnlockMutex (jobs_mutex);
			Jobs_LogQueueStats ("overflow-drop");
			return false;
		case JOBS_OVERFLOW_FAIL:
			jobs_overflow_fail++;
			jobs_submit_failed++;
			SDL_UnlockMutex (jobs_mutex);
			Jobs_LogQueueStats ("overflow-fail");
			return false;
		case JOBS_OVERFLOW_BLOCK:
			wait_ms = q_max (0, (int) host_async_overflow_block_ms.value);
			if (wait_ms <= 0 || SDL_CondWaitTimeout (jobs_not_full, jobs_mutex, wait_ms) == SDL_MUTEX_TIMEDOUT)
			{
				jobs_overflow_block_timeouts++;
				jobs_submit_failed++;
				SDL_UnlockMutex (jobs_mutex);
				Jobs_LogQueueStats ("overflow-block-timeout");
				return false;
			}
			if (!Jobs_IsAcceptingSubmissionsLocked ())
			{
				jobs_submit_failed++;
				SDL_UnlockMutex (jobs_mutex);
				Jobs_LogQueueStats ("submit-shutdown");
				return false;
			}
			jobs_overflow_block_waits++;
			max_pending = (int) host_async_max_pending.value;
			continue;
		case JOBS_OVERFLOW_SYNC:
		default:
			jobs_overflow_sync++;
			SDL_UnlockMutex (jobs_mutex);
			Jobs_LogQueueStats ("overflow-sync");
			return false;
		}
	}

	if (!Jobs_EnqueueLocked (node))
	{
		jobs_submit_failed++;
		SDL_UnlockMutex (jobs_mutex);
		Jobs_LogQueueStats ("enqueue-oom");
		return false;
	}
	SDL_CondSignal (jobs_not_empty);
	jobs_wake_signals++;
	SDL_UnlockMutex (jobs_mutex);
	Jobs_LogQueueStats ("enqueue");
	return true;
}

static void Jobs_LogQueueStats (const char *reason)
{
	unsigned int pending;
	unsigned int peak_pending;
	unsigned int dropped;
	unsigned int queue_full_hits;
	unsigned int submit_failed;
	unsigned int sync_fallbacks;
	unsigned int wake_signals;
	unsigned int wake_broadcasts;
	unsigned int overflow_sync;
	unsigned int overflow_drop;
	unsigned int overflow_fail;
	unsigned int overflow_block_waits;
	unsigned int overflow_block_timeouts;

	if (!developer.value)
		return;

	SDL_LockMutex (jobs_mutex);
	pending = jobs_pending;
	peak_pending = jobs_peak_pending;
	dropped = jobs_dropped;
	queue_full_hits = jobs_queue_full_hits;
	submit_failed = jobs_submit_failed;
	sync_fallbacks = jobs_sync_fallbacks;
	wake_signals = jobs_wake_signals;
	wake_broadcasts = jobs_wake_broadcasts;
	overflow_sync = jobs_overflow_sync;
	overflow_drop = jobs_overflow_drop;
	overflow_fail = jobs_overflow_fail;
	overflow_block_waits = jobs_overflow_block_waits;
	overflow_block_timeouts = jobs_overflow_block_timeouts;
	SDL_UnlockMutex (jobs_mutex);

	Con_DPrintf ("Async jobs %s: pending=%u peak=%u dropped=%u queue_full_hits=%u submit_failed=%u sync_fallbacks=%u wake_signals=%u wake_broadcasts=%u ovf_sync=%u ovf_drop=%u ovf_fail=%u ovf_block_waits=%u ovf_block_timeouts=%u max_pending=%d policy=%s block_ms=%d\n",
		reason,
		pending,
		peak_pending,
		dropped,
		queue_full_hits,
		submit_failed,
		sync_fallbacks,
		wake_signals,
		wake_broadcasts,
		overflow_sync,
		overflow_drop,
		overflow_fail,
		overflow_block_waits,
		overflow_block_timeouts,
		(int) host_async_max_pending.value,
		host_async_overflow_policy.string,
		(int) host_async_overflow_block_ms.value);
}

qboolean Jobs_TrySubmitDetachedPriority (jobs_priority_t priority, jobs_func_t func, void *userdata)
{
	jobnode_t node;

	if (!func)
		Sys_Error ("Jobs_TrySubmitDetachedPriority: NULL function");

	if (priority < JOBS_PRIORITY_HIGH || priority >= JOBS_PRIORITY_COUNT)
		priority = JOBS_PRIORITY_NORMAL;

	if (!jobs_mutex)
	{
		func (userdata);
		return true;
	}

	SDL_LockMutex (jobs_mutex);
	if (!Jobs_IsAcceptingSubmissionsLocked ())
	{
		jobs_submit_failed++;
		SDL_UnlockMutex (jobs_mutex);
		return false;
	}
	SDL_UnlockMutex (jobs_mutex);

	if (!Host_AsyncEnabled () || jobs_num_threads <= 0)
	{
		func (userdata);
		return true;
	}

	memset (&node, 0, sizeof (node));
	node.func = func;
	node.userdata = userdata;
	node.detached = true;
	node.priority = priority;

	if (!Jobs_SubmitNode (&node))
		return false;

	return true;
}

qboolean Jobs_TrySubmitDetached (jobs_func_t func, void *userdata)
{
	return Jobs_TrySubmitDetachedPriority (JOBS_PRIORITY_NORMAL, func, userdata);
}

void Jobs_SubmitDetachedPriority (jobs_priority_t priority, jobs_func_t func, void *userdata)
{
	if (!Jobs_TrySubmitDetachedPriority (priority, func, userdata))
	{
		qboolean do_sync_fallback = false;

		SDL_LockMutex (jobs_mutex);
		if (Jobs_IsAcceptingSubmissionsLocked ())
		{
			jobs_sync_fallbacks++;
			do_sync_fallback = true;
		}
		SDL_UnlockMutex (jobs_mutex);
		if (do_sync_fallback)
			func (userdata);
	}
}

void Jobs_SubmitDetached (jobs_func_t func, void *userdata)
{
	Jobs_SubmitDetachedPriority (JOBS_PRIORITY_NORMAL, func, userdata);
}

void Jobs_Wait (JobHandle *handle)
{
	if (!handle)
		return;
	SDL_LockMutex (handle->mutex);
	while (!handle->done)
		SDL_CondWait (handle->cond, handle->mutex);
	SDL_UnlockMutex (handle->mutex);
	SDL_DestroyCond (handle->cond);
	SDL_DestroyMutex (handle->mutex);
	q_free(handle);
}

typedef struct fs_completion_s {
	fs_async_cb cb;
	void *user;
	uint8_t *data;
	size_t len;
	int status;
	unsigned int id;
	unsigned int generation;
	struct fs_completion_s *next;
} fs_completion_t;

typedef struct fs_job_s {
	char path[MAX_QPATH];
	unsigned int id;
	unsigned int generation;
	fs_async_cb cb;
	void *user;
} fs_job_t;

static SDL_mutex *fs_mutex;
static fs_completion_t *fs_comp_head;
static fs_completion_t *fs_comp_tail;
static unsigned int fs_next_id = 1;
static unsigned int fs_generation;

typedef struct fs_idset_s {
	unsigned int *keys;
	size_t capacity;
	size_t count;
} fs_idset_t;

typedef struct fs_cancelset_s {
	unsigned int *keys;
	unsigned int *generations;
	size_t capacity;
	size_t count;
} fs_cancelset_t;

static fs_idset_t fs_outstanding_set;
static fs_idset_t fs_completion_set;
static fs_cancelset_t fs_cancel_set;

#define FS_RECENT_COMPLETED_CAPACITY 64
#define FS_CANCELED_PRUNE_GENERATIONS 4
#define FS_IDSET_TOMBSTONE 0xffffffffu

static unsigned int fs_recent_completed_ids[FS_RECENT_COMPLETED_CAPACITY];
static unsigned int fs_recent_completed_count;
static unsigned int fs_recent_completed_write;

static size_t FS_IdHash (unsigned int id, size_t capacity)
{
	return ((size_t)id * 2654435761u) & (capacity - 1);
}

static qboolean FS_IdSetEnsureCapacityLocked (fs_idset_t *set, size_t required)
{
	unsigned int *new_keys;
	size_t new_capacity;

	if (required == 0)
		required = 1;
	if (set->capacity > 0 && required * 10 <= set->capacity * 7)
		return true;

	new_capacity = set->capacity ? set->capacity : 64;
	while (required * 10 > new_capacity * 7)
		new_capacity <<= 1;

	new_keys = (unsigned int *) q_calloc(new_capacity, sizeof (*new_keys));
	if (!new_keys)
		return false;

	if (set->keys)
	{
		for (size_t i = 0; i < set->capacity; ++i)
		{
			unsigned int key = set->keys[i];
			size_t idx;
			if (key == 0 || key == FS_IDSET_TOMBSTONE)
				continue;
			idx = FS_IdHash (key, new_capacity);
			while (new_keys[idx] != 0)
				idx = (idx + 1) & (new_capacity - 1);
			new_keys[idx] = key;
		}
		q_free(set->keys);
	}
	set->keys = new_keys;
	set->capacity = new_capacity;
	return true;
}

static qboolean FS_IdSetContainsLocked (const fs_idset_t *set, unsigned int id)
{
	size_t idx;
	size_t probes;

	if (!set->keys || set->capacity == 0 || id == 0 || id == FS_IDSET_TOMBSTONE)
		return false;
	idx = FS_IdHash (id, set->capacity);
	probes = 0;
	while (probes < set->capacity)
	{
		unsigned int key = set->keys[idx];
		if (key == 0)
			return false;
		if (key == id)
			return true;
		idx = (idx + 1) & (set->capacity - 1);
		probes++;
	}
	return false;
}

static qboolean FS_IdSetInsertLocked (fs_idset_t *set, unsigned int id)
{
	size_t idx;
	size_t probes;
	size_t tombstone = (size_t)-1;

	if (id == 0 || id == FS_IDSET_TOMBSTONE)
		return false;
	if (!FS_IdSetEnsureCapacityLocked (set, set->count + 1))
		return false;

	idx = FS_IdHash (id, set->capacity);
	probes = 0;
	while (probes < set->capacity)
	{
		unsigned int key = set->keys[idx];
		if (key == id)
			return true;
		if (key == FS_IDSET_TOMBSTONE && tombstone == (size_t)-1)
			tombstone = idx;
		if (key == 0)
		{
			size_t target = (tombstone != (size_t)-1) ? tombstone : idx;
			set->keys[target] = id;
			set->count++;
			return true;
		}
		idx = (idx + 1) & (set->capacity - 1);
		probes++;
	}

	if (tombstone != (size_t)-1)
	{
		set->keys[tombstone] = id;
		set->count++;
		return true;
	}

	return false;
}

static qboolean FS_IdSetRemoveLocked (fs_idset_t *set, unsigned int id)
{
	size_t idx;
	size_t probes;

	if (!set->keys || set->capacity == 0 || id == 0 || id == FS_IDSET_TOMBSTONE)
		return false;
	idx = FS_IdHash (id, set->capacity);
	probes = 0;
	while (probes < set->capacity)
	{
		unsigned int key = set->keys[idx];
		if (key == 0)
			return false;
		if (key == id)
		{
			set->keys[idx] = FS_IDSET_TOMBSTONE;
			if (set->count > 0)
				set->count--;
			return true;
		}
		idx = (idx + 1) & (set->capacity - 1);
		probes++;
	}
	return false;
}

static void FS_IdSet_ClearLocked (fs_idset_t *set)
{
	q_free(set->keys);
	set->keys = NULL;
	set->capacity = 0;
	set->count = 0;
}

static qboolean FS_CancelSetEnsureCapacityLocked (size_t required)
{
	unsigned int *new_keys;
	unsigned int *new_generations;
	size_t new_capacity;

	if (required == 0)
		required = 1;
	if (fs_cancel_set.capacity > 0 && required * 10 <= fs_cancel_set.capacity * 7)
		return true;

	new_capacity = fs_cancel_set.capacity ? fs_cancel_set.capacity : 64;
	while (required * 10 > new_capacity * 7)
		new_capacity <<= 1;

	new_keys = (unsigned int *) q_calloc(new_capacity, sizeof (*new_keys));
	new_generations = (unsigned int *) q_calloc(new_capacity, sizeof (*new_generations));
	if (!new_keys || !new_generations)
	{
		q_free(new_keys);
		q_free(new_generations);
		return false;
	}

	if (fs_cancel_set.keys)
	{
		for (size_t i = 0; i < fs_cancel_set.capacity; ++i)
		{
			unsigned int key = fs_cancel_set.keys[i];
			unsigned int generation = fs_cancel_set.generations[i];
			size_t idx;
			if (key == 0 || key == FS_IDSET_TOMBSTONE)
				continue;
			idx = FS_IdHash (key, new_capacity);
			while (new_keys[idx] != 0)
				idx = (idx + 1) & (new_capacity - 1);
			new_keys[idx] = key;
			new_generations[idx] = generation;
		}
		q_free(fs_cancel_set.keys);
		q_free(fs_cancel_set.generations);
	}
	fs_cancel_set.keys = new_keys;
	fs_cancel_set.generations = new_generations;
	fs_cancel_set.capacity = new_capacity;
	return true;
}

static qboolean FS_CancelSetContainsLocked (unsigned int id)
{
	size_t idx;
	size_t probes;

	if (!fs_cancel_set.keys || fs_cancel_set.capacity == 0 || id == 0 || id == FS_IDSET_TOMBSTONE)
		return false;
	idx = FS_IdHash (id, fs_cancel_set.capacity);
	probes = 0;
	while (probes < fs_cancel_set.capacity)
	{
		unsigned int key = fs_cancel_set.keys[idx];
		if (key == 0)
			return false;
		if (key == id)
			return true;
		idx = (idx + 1) & (fs_cancel_set.capacity - 1);
		probes++;
	}
	return false;
}

static qboolean FS_CancelSetInsertLocked (unsigned int id, unsigned int generation)
{
	size_t idx;
	size_t probes;
	size_t tombstone = (size_t)-1;

	if (id == 0 || id == FS_IDSET_TOMBSTONE)
		return false;
	if (!FS_CancelSetEnsureCapacityLocked (fs_cancel_set.count + 1))
		return false;
	idx = FS_IdHash (id, fs_cancel_set.capacity);
	probes = 0;
	while (probes < fs_cancel_set.capacity)
	{
		unsigned int key = fs_cancel_set.keys[idx];
		if (key == id)
			return true;
		if (key == FS_IDSET_TOMBSTONE && tombstone == (size_t)-1)
			tombstone = idx;
		if (key == 0)
		{
			size_t target = (tombstone != (size_t)-1) ? tombstone : idx;
			fs_cancel_set.keys[target] = id;
			fs_cancel_set.generations[target] = generation;
			fs_cancel_set.count++;
			return true;
		}
		idx = (idx + 1) & (fs_cancel_set.capacity - 1);
		probes++;
	}

	if (tombstone != (size_t)-1)
	{
		fs_cancel_set.keys[tombstone] = id;
		fs_cancel_set.generations[tombstone] = generation;
		fs_cancel_set.count++;
		return true;
	}
	return false;
}

static qboolean FS_CancelSetRemoveLocked (unsigned int id)
{
	size_t idx;
	size_t probes;

	if (!fs_cancel_set.keys || fs_cancel_set.capacity == 0 || id == 0 || id == FS_IDSET_TOMBSTONE)
		return false;
	idx = FS_IdHash (id, fs_cancel_set.capacity);
	probes = 0;
	while (probes < fs_cancel_set.capacity)
	{
		unsigned int key = fs_cancel_set.keys[idx];
		if (key == 0)
			return false;
		if (key == id)
		{
			fs_cancel_set.keys[idx] = FS_IDSET_TOMBSTONE;
			fs_cancel_set.generations[idx] = 0;
			if (fs_cancel_set.count > 0)
				fs_cancel_set.count--;
			return true;
		}
		idx = (idx + 1) & (fs_cancel_set.capacity - 1);
		probes++;
	}
	return false;
}

static void FS_CancelSet_PruneLocked (void)
{
	if (!fs_cancel_set.keys)
		return;
	for (size_t i = 0; i < fs_cancel_set.capacity; ++i)
	{
		unsigned int key = fs_cancel_set.keys[i];
		unsigned int generation;
		unsigned int age;
		if (key == 0 || key == FS_IDSET_TOMBSTONE)
			continue;
		generation = fs_cancel_set.generations[i];
		age = fs_generation - generation;
		if (age > FS_CANCELED_PRUNE_GENERATIONS)
		{
			fs_cancel_set.keys[i] = FS_IDSET_TOMBSTONE;
			fs_cancel_set.generations[i] = 0;
			if (fs_cancel_set.count > 0)
				fs_cancel_set.count--;
		}
	}
}

static void FS_CancelSet_ClearLocked (void)
{
	q_free(fs_cancel_set.keys);
	q_free(fs_cancel_set.generations);
	fs_cancel_set.keys = NULL;
	fs_cancel_set.generations = NULL;
	fs_cancel_set.capacity = 0;
	fs_cancel_set.count = 0;
}

static qboolean FS_HasCanceledIdLocked (unsigned int id)
{
	return FS_CancelSetContainsLocked (id);
}

static qboolean FS_HasCompletionIdLocked (unsigned int id)
{
	return FS_IdSetContainsLocked (&fs_completion_set, id);
}

static qboolean FS_HasOutstandingIdLocked (unsigned int id)
{
	return FS_IdSetContainsLocked (&fs_outstanding_set, id);
}

static void FS_AddOutstandingIdLocked (unsigned int id)
{
	if (!FS_IdSetInsertLocked (&fs_outstanding_set, id))
		Sys_Error ("FS_AddOutstandingIdLocked: out of memory");
}

static qboolean FS_RemoveOutstandingIdLocked (unsigned int id)
{
	return FS_IdSetRemoveLocked (&fs_outstanding_set, id);
}

static void FS_AddCompletionIdLocked (unsigned int id)
{
	if (!FS_IdSetInsertLocked (&fs_completion_set, id))
		Sys_Error ("FS_AddCompletionIdLocked: out of memory");
}

static void FS_RemoveCompletionIdLocked (unsigned int id)
{
	FS_IdSetRemoveLocked (&fs_completion_set, id);
}

static qboolean FS_TakeCanceledIdLocked (unsigned int id)
{
	return FS_CancelSetRemoveLocked (id);
}

static void FS_PruneCanceledIdsLocked (void)
{
	FS_CancelSet_PruneLocked ();
}

static unsigned int FS_AllocAsyncReadIdLocked (void)
{
	unsigned int candidate = fs_next_id;

	for (;;)
	{
		/* id 0 is reserved and never issued as a valid async handle. */
		if (candidate == 0 || candidate == FS_IDSET_TOMBSTONE)
			candidate = 1;

		if (!FS_HasOutstandingIdLocked (candidate)
			&& !FS_HasCanceledIdLocked (candidate)
			&& !FS_HasCompletionIdLocked (candidate))
			break;

		candidate++;
	}

	fs_next_id = candidate + 1;
	if (fs_next_id == 0 || fs_next_id == FS_IDSET_TOMBSTONE)
		fs_next_id = 1;

	return candidate;
}

static qboolean FS_IsRecentCompletionLocked (unsigned int id)
{
	unsigned int i;

	for (i = 0; i < fs_recent_completed_count; ++i)
	{
		if (fs_recent_completed_ids[i] == id)
			return true;
	}

	return false;
}

static void FS_RecordCompletionIdLocked (unsigned int id)
{
	fs_recent_completed_ids[fs_recent_completed_write] = id;
	fs_recent_completed_write = (fs_recent_completed_write + 1) % FS_RECENT_COMPLETED_CAPACITY;
	if (fs_recent_completed_count < FS_RECENT_COMPLETED_CAPACITY)
		fs_recent_completed_count++;
}


static uint8_t *FS_LoadMallocThread (const char *path, size_t *len_out)
{
	FILE *f;
	long len;
	long pos;
	uint8_t *buf;
	size_t nread;

	*len_out = 0;
	if (COM_FOpenFile (path, &f, NULL) < 0 || !f)
		return NULL;
	pos = ftell (f);
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, pos, SEEK_SET);
	if (len < 0)
	{
		fclose (f);
		return NULL;
	}
	buf = (uint8_t *) q_malloc((size_t) len + 1);
	if (!buf)
		Sys_Error ("FS_LoadMallocThread: out of memory");
	nread = fread (buf, 1, (size_t) len, f);
	fclose (f);
	if (nread != (size_t) len)
	{
		q_free(buf);
		return NULL;
	}
	buf[len] = 0;
	*len_out = (size_t) len;
	return buf;
}

static void FS_AsyncReadJob (void *userdata)
{
	fs_job_t *job = (fs_job_t *) userdata;
	fs_completion_t *comp = (fs_completion_t *) q_calloc(1, sizeof (*comp));
	if (!comp)
		Sys_Error ("FS_AsyncReadJob: out of memory");

	comp->cb = job->cb;
	comp->user = job->user;
	comp->id = job->id;
	comp->generation = job->generation;
	comp->data = FS_LoadMallocThread (job->path, &comp->len);
	comp->status = comp->data ? 0 : -1;

	SDL_LockMutex (fs_mutex);
	FS_AddCompletionIdLocked (comp->id);
	if (fs_comp_tail)
		fs_comp_tail->next = comp;
	else
		fs_comp_head = comp;
	fs_comp_tail = comp;
	SDL_UnlockMutex (fs_mutex);

	q_free(job);
}

fs_asyncread_handle_t FS_AsyncRead (const char *path, fs_async_cb cb, void *user)
{
	fs_asyncread_handle_t handle;
	fs_job_t *job;
	size_t copied;
	handle.id = 0;

	if (!path || !path[0] || !cb)
	{
		if (cb)
			cb (user, NULL, 0, -1);
		return handle;
	}

	if (!Host_AsyncFSEnabled ())
	{
		size_t len;
		uint8_t *data = FS_LoadMallocThread (path, &len);
		cb (user, data, len, data ? 0 : -1);
		return handle;
	}

	job = (fs_job_t *) q_calloc(1, sizeof (*job));
	if (!job)
		Sys_Error ("FS_AsyncRead: out of memory");
	copied = q_strlcpy (job->path, path, sizeof (job->path));
	if (copied >= sizeof (job->path))
	{
		/*
		 * Prevent truncated filesystem paths from being queued to workers.
		 * Truncation can produce non-deterministic misses and hard-to-trace
		 * failures while map assets are loading.
		 */
		q_free(job);
		cb (user, NULL, 0, -1);
		return handle;
	}
	job->cb = cb;
	job->user = user;

	SDL_LockMutex (fs_mutex);
	job->id = FS_AllocAsyncReadIdLocked ();
	FS_AddOutstandingIdLocked (job->id);
	job->generation = fs_generation;
	SDL_UnlockMutex (fs_mutex);

	handle.id = job->id;
	if (!Jobs_TrySubmitDetachedPriority (JOBS_PRIORITY_HIGH, FS_AsyncReadJob, job))
	{
		SDL_LockMutex (fs_mutex);
		FS_RemoveOutstandingIdLocked (job->id);
		SDL_UnlockMutex (fs_mutex);
		handle.id = 0;
		cb (user, NULL, 0, -1);
		q_free(job);
	}
	return handle;
}

void FS_AsyncReadCancel (fs_asyncread_handle_t handle)
{
	if (!handle.id)
		return;
	if (!fs_mutex)
		return;

	SDL_LockMutex (fs_mutex);
	if (FS_HasCanceledIdLocked (handle.id))
	{
		SDL_UnlockMutex (fs_mutex);
		return;
	}
	if (FS_IsRecentCompletionLocked (handle.id))
	{
		SDL_UnlockMutex (fs_mutex);
		return;
	}

	/*
	 * Track cancellations by exact async handle id. This avoids the old
	 * "cancel-before" behavior where canceling one request could
	 * unintentionally suppress unrelated lower-id completions.
	 *
	 * Late cancels are best-effort: if the completion was already consumed,
	 * the id may still be in a small recent-completion cache and can be
	 * ignored immediately. Otherwise we enqueue the cancel id for matching in
	 * a later completion pump, and unmatched ids are pruned after a safe
	 * generation horizon.
	 */
	if (!FS_CancelSetInsertLocked (handle.id, fs_generation))
		Sys_Error ("FS_AsyncReadCancel: out of memory");
	SDL_UnlockMutex (fs_mutex);
}

void FS_AsyncAdvanceGeneration (void)
{
	if (!fs_mutex)
	{
		fs_generation++;
		return;
	}
	SDL_LockMutex (fs_mutex);
	fs_generation++;
	SDL_UnlockMutex (fs_mutex);
}

void FS_PumpAsyncCompletions (void)
{
	fs_completion_t *list;

	SDL_LockMutex (fs_mutex);
	FS_PruneCanceledIdsLocked ();
	list = fs_comp_head;
	fs_comp_head = fs_comp_tail = NULL;
	SDL_UnlockMutex (fs_mutex);

	while (list)
	{
		fs_completion_t *next = list->next;
		qboolean canceled;
		qboolean stale;

		SDL_LockMutex (fs_mutex);
		FS_RecordCompletionIdLocked (list->id);
		FS_RemoveCompletionIdLocked (list->id);
		canceled = FS_TakeCanceledIdLocked (list->id);
		stale = list->generation != fs_generation;
		FS_RemoveOutstandingIdLocked (list->id);
		SDL_UnlockMutex (fs_mutex);

		if (!canceled && !stale)
			list->cb (list->user, list->data, list->len, list->status);
		else if (list->data)
			q_free(list->data);
		q_free(list);
		list = next;
	}
}

void Jobs_Shutdown (void)
{
	fs_completion_t *comp;

	if (!jobs_mutex)
		return;

	SDL_LockMutex (jobs_mutex);
	if (jobs_state == JOBS_STATE_STOPPED)
	{
		SDL_UnlockMutex (jobs_mutex);
		return;
	}
	jobs_state = JOBS_STATE_SHUTTING_DOWN;
	SDL_CondBroadcast (jobs_not_empty);
	SDL_CondBroadcast (jobs_not_full);
	jobs_wake_broadcasts++;
	SDL_UnlockMutex (jobs_mutex);

	if (jobs_threads)
	{
		int i;

		for (i = 0; i < jobs_num_threads; ++i)
		{
			if (jobs_threads[i])
				SDL_WaitThread (jobs_threads[i], NULL);
		}
		q_free(jobs_threads);
		jobs_threads = NULL;
	}
	jobs_num_threads = 0;

	/* Detach the completion queue under lock, then free nodes after unlock. */
	SDL_LockMutex (fs_mutex);
	comp = fs_comp_head;
	fs_comp_head = fs_comp_tail = NULL;
	SDL_UnlockMutex (fs_mutex);

	while (comp)
	{
		fs_completion_t *next = comp->next;
		if (comp->data)
			q_free(comp->data);
		q_free(comp);
		comp = next;
	}

	SDL_LockMutex (jobs_mutex);
	for (int prio = JOBS_PRIORITY_HIGH; prio < JOBS_PRIORITY_COUNT; ++prio)
	{
		jobs_ring_t *ring = &jobs_queues[prio];
		while (ring->count > 0)
		{
			jobnode_t node = ring->items[ring->head];
			ring->head = (ring->head + 1) & (ring->capacity - 1);
			ring->count--;
			if (!node.detached)
			{
				SDL_LockMutex (node.handle->mutex);
				node.handle->done = true;
				SDL_CondSignal (node.handle->cond);
				SDL_UnlockMutex (node.handle->mutex);
			}
		}
	}
	jobs_pending = 0;
	jobs_state = JOBS_STATE_STOPPED;
	SDL_UnlockMutex (jobs_mutex);

	SDL_LockMutex (fs_mutex);
	FS_CancelSet_ClearLocked ();
	FS_IdSet_ClearLocked (&fs_outstanding_set);
	FS_IdSet_ClearLocked (&fs_completion_set);
	fs_recent_completed_count = 0;
	fs_recent_completed_write = 0;
	memset (fs_recent_completed_ids, 0, sizeof (fs_recent_completed_ids));
	SDL_UnlockMutex (fs_mutex);

	SDL_LockMutex (jobs_mutex);
	for (int prio = JOBS_PRIORITY_HIGH; prio < JOBS_PRIORITY_COUNT; ++prio)
	{
		q_free (jobs_queues[prio].items);
		jobs_queues[prio].items = NULL;
		jobs_queues[prio].head = 0;
		jobs_queues[prio].tail = 0;
		jobs_queues[prio].count = 0;
		jobs_queues[prio].capacity = 0;
	}
	SDL_UnlockMutex (jobs_mutex);

	SDL_DestroyCond (jobs_not_full);
	SDL_DestroyCond (jobs_not_empty);
	SDL_DestroyMutex (jobs_mutex);
	jobs_not_full = NULL;
	jobs_not_empty = NULL;
	jobs_mutex = NULL;

	SDL_DestroyMutex (fs_mutex);
	fs_mutex = NULL;
}

void Jobs_Init (void)
{
	Cvar_RegisterVariable (&host_async);
	Cvar_RegisterVariable (&host_async_fs);
	Cvar_RegisterVariable (&host_async_assets);
	Cvar_RegisterVariable (&host_async_workers);
	Cvar_RegisterVariable (&host_async_max_pending);
	Cvar_RegisterVariable (&host_async_overflow_policy);
	Cvar_RegisterVariable (&host_async_overflow_block_ms);

	jobs_mutex = SDL_CreateMutex ();
	jobs_not_empty = SDL_CreateCond ();
	jobs_not_full = SDL_CreateCond ();
	fs_mutex = SDL_CreateMutex ();
	if (!jobs_mutex || !jobs_not_empty || !jobs_not_full || !fs_mutex)
		Sys_Error ("Jobs_Init: failed to initialize async primitives");
	SDL_LockMutex (jobs_mutex);
	jobs_state = JOBS_STATE_RUNNING;
	jobs_pending = 0;
	jobs_peak_pending = 0;
	jobs_dropped = 0;
	jobs_queue_full_hits = 0;
	jobs_submit_failed = 0;
	jobs_sync_fallbacks = 0;
	jobs_wake_signals = 0;
	jobs_wake_broadcasts = 0;
	jobs_overflow_sync = 0;
	jobs_overflow_drop = 0;
	jobs_overflow_fail = 0;
	jobs_overflow_block_waits = 0;
	jobs_overflow_block_timeouts = 0;
	for (int prio = JOBS_PRIORITY_HIGH; prio < JOBS_PRIORITY_COUNT; ++prio)
	{
		jobs_queues[prio].items = NULL;
		jobs_queues[prio].head = 0;
		jobs_queues[prio].tail = 0;
		jobs_queues[prio].count = 0;
		jobs_queues[prio].capacity = 0;
	}
	SDL_UnlockMutex (jobs_mutex);

	SDL_LockMutex (fs_mutex);
	fs_generation = 0;
	fs_next_id = 1;
	fs_recent_completed_count = 0;
	fs_recent_completed_write = 0;
	memset (fs_recent_completed_ids, 0, sizeof (fs_recent_completed_ids));
	FS_CancelSet_ClearLocked ();
	FS_IdSet_ClearLocked (&fs_outstanding_set);
	FS_IdSet_ClearLocked (&fs_completion_set);
	SDL_UnlockMutex (fs_mutex);

	{
		int i;
		int cpu_count;
		int worker_count;

		cpu_count = SDL_GetCPUCount ();
		if (cpu_count < 1)
			cpu_count = 1;
		worker_count = CLAMP (1, (int) host_async_workers.value, cpu_count);
		jobs_threads = (SDL_Thread **) q_calloc((size_t) worker_count, sizeof (*jobs_threads));
		if (!jobs_threads)
			Sys_Error ("Jobs_Init: out of memory");

		for (i = 0; i < worker_count; ++i)
		{
			jobs_threads[i] = SDL_CreateThread (Jobs_Worker, "jobs", NULL);
			if (!jobs_threads[i])
				Sys_Error ("Jobs_Init: failed to create worker thread");
		}
		jobs_num_threads = worker_count;
	}
}
