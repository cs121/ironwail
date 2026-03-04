#include "quakedef.h"

typedef struct jobnode_s {
	jobs_func_t func;
	void *userdata;
	JobHandle *handle;
	qboolean detached;
	struct jobnode_s *next;
} jobnode_t;

static SDL_Thread **jobs_threads;
static int jobs_num_threads;
static SDL_mutex *jobs_mutex;
static SDL_cond *jobs_cond;
static jobnode_t *jobs_head;
static jobnode_t *jobs_tail;
static qboolean jobs_shutdown;
/* Queue stats are protected by jobs_mutex. Read/write them only while holding that lock. */
static unsigned int jobs_pending;
static unsigned int jobs_peak_pending;
static unsigned int jobs_dropped;
static unsigned int jobs_sync_fallbacks;
static unsigned int jobs_wake_signals;
static unsigned int jobs_wake_broadcasts;

static qboolean Jobs_SubmitNode (jobnode_t *node);
static void Jobs_LogQueueStats (const char *reason);

static cvar_t host_async = {"host_async", "0", CVAR_ARCHIVE};
static cvar_t host_async_fs = {"host_async_fs", "0", CVAR_ARCHIVE};
static cvar_t host_async_assets = {"host_async_assets", "0", CVAR_ARCHIVE};
static cvar_t host_async_workers = {"host_async_workers", "1", CVAR_ARCHIVE};
static cvar_t host_async_max_pending = {"host_async_max_pending", "128", CVAR_ARCHIVE};

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
		jobnode_t *node;

		SDL_LockMutex (jobs_mutex);
		while (!jobs_shutdown && !jobs_head)
			SDL_CondWait (jobs_cond, jobs_mutex);
		if (jobs_shutdown && !jobs_head)
		{
			SDL_UnlockMutex (jobs_mutex);
			break;
		}
		node = jobs_head;
		jobs_head = node->next;
		if (!jobs_head)
			jobs_tail = NULL;
		if (jobs_pending > 0)
			jobs_pending--;
		SDL_UnlockMutex (jobs_mutex);

		node->func (node->userdata);
		if (node->detached)
		{
			free (node);
			continue;
		}
		SDL_LockMutex (node->handle->mutex);
		node->handle->done = true;
		SDL_CondSignal (node->handle->cond);
		SDL_UnlockMutex (node->handle->mutex);
		free (node);
	}
	return 0;
}

JobHandle *Jobs_Submit (jobs_func_t func, void *userdata)
{
	JobHandle *handle;
	jobnode_t *node;

	handle = (JobHandle *) calloc (1, sizeof (*handle));
	if (!handle)
		Sys_Error ("Jobs_Submit: out of memory");
	handle->mutex = SDL_CreateMutex ();
	handle->cond = SDL_CreateCond ();
	if (!handle->mutex || !handle->cond)
		Sys_Error ("Jobs_Submit: failed to create handle sync primitives");

	if (!Host_AsyncEnabled () || jobs_num_threads <= 0)
	{
		func (userdata);
		SDL_LockMutex (handle->mutex);
		handle->done = true;
		SDL_CondSignal (handle->cond);
		SDL_UnlockMutex (handle->mutex);
		return handle;
	}

	node = (jobnode_t *) calloc (1, sizeof (*node));
	if (!node)
		Sys_Error ("Jobs_Submit: out of memory");
	node->func = func;
	node->userdata = userdata;
	node->handle = handle;

	if (!Jobs_SubmitNode (node))
	{
		SDL_LockMutex (jobs_mutex);
		jobs_sync_fallbacks++;
		SDL_UnlockMutex (jobs_mutex);
		func (userdata);
		SDL_LockMutex (handle->mutex);
		handle->done = true;
		SDL_CondSignal (handle->cond);
		SDL_UnlockMutex (handle->mutex);
		free (node);
	}

	return handle;
}

static qboolean Jobs_SubmitNode (jobnode_t *node)
{
	int max_pending;

	SDL_LockMutex (jobs_mutex);
	max_pending = (int) host_async_max_pending.value;
	if (max_pending > 0 && jobs_pending >= (unsigned int) max_pending)
	{
		jobs_dropped++;
		SDL_UnlockMutex (jobs_mutex);
		Jobs_LogQueueStats ("queue full");
		return false;
	}

	if (jobs_tail)
		jobs_tail->next = node;
	else
		jobs_head = node;
	jobs_tail = node;
	jobs_pending++;
	if (jobs_pending > jobs_peak_pending)
		jobs_peak_pending = jobs_pending;
	SDL_CondSignal (jobs_cond);
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
	unsigned int sync_fallbacks;
	unsigned int wake_signals;
	unsigned int wake_broadcasts;

	if (!developer.value)
		return;

	SDL_LockMutex (jobs_mutex);
	pending = jobs_pending;
	peak_pending = jobs_peak_pending;
	dropped = jobs_dropped;
	sync_fallbacks = jobs_sync_fallbacks;
	wake_signals = jobs_wake_signals;
	wake_broadcasts = jobs_wake_broadcasts;
	SDL_UnlockMutex (jobs_mutex);

	Con_DPrintf ("Async jobs %s: pending=%u peak=%u dropped=%u sync_fallbacks=%u wake_signals=%u wake_broadcasts=%u max_pending=%d\n",
		reason,
		pending,
		peak_pending,
		dropped,
		sync_fallbacks,
		wake_signals,
		wake_broadcasts,
		(int) host_async_max_pending.value);
}

void Jobs_SubmitDetached (jobs_func_t func, void *userdata)
{
	jobnode_t *node;

	if (!Host_AsyncEnabled () || jobs_num_threads <= 0)
	{
		func (userdata);
		return;
	}

	node = (jobnode_t *) calloc (1, sizeof (*node));
	if (!node)
		Sys_Error ("Jobs_SubmitDetached: out of memory");
	node->func = func;
	node->userdata = userdata;
	node->detached = true;

	if (!Jobs_SubmitNode (node))
	{
		SDL_LockMutex (jobs_mutex);
		jobs_sync_fallbacks++;
		SDL_UnlockMutex (jobs_mutex);
		func (userdata);
		free (node);
	}
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
	free (handle);
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

typedef struct fs_canceled_id_s {
	unsigned int id;
	unsigned int added_generation;
	struct fs_canceled_id_s *next;
} fs_canceled_id_t;

static fs_canceled_id_t *fs_canceled_ids;

#define FS_RECENT_COMPLETED_CAPACITY 64
#define FS_CANCELED_PRUNE_GENERATIONS 4

static unsigned int fs_recent_completed_ids[FS_RECENT_COMPLETED_CAPACITY];
static unsigned int fs_recent_completed_count;
static unsigned int fs_recent_completed_write;

static qboolean FS_HasCanceledIdLocked (unsigned int id)
{
	fs_canceled_id_t *entry = fs_canceled_ids;
	while (entry)
	{
		if (entry->id == id)
			return true;
		entry = entry->next;
	}

	return false;
}

static qboolean FS_HasCompletionIdLocked (unsigned int id)
{
	fs_completion_t *comp = fs_comp_head;
	while (comp)
	{
		if (comp->id == id)
			return true;
		comp = comp->next;
	}

	return false;
}

static unsigned int FS_AllocAsyncReadIdLocked (void)
{
	unsigned int candidate = fs_next_id;

	for (;;)
	{
		/* id 0 is reserved and never issued as a valid async handle. */
		if (candidate == 0)
			candidate = 1;

		if (!FS_HasCompletionIdLocked (candidate) && !FS_HasCanceledIdLocked (candidate))
			break;

		candidate++;
	}

	fs_next_id = candidate + 1;
	if (fs_next_id == 0)
		fs_next_id = 1;

	return candidate;
}

static qboolean FS_TakeCanceledIdLocked (unsigned int id)
{
	fs_canceled_id_t **cursor = &fs_canceled_ids;
	while (*cursor)
	{
		fs_canceled_id_t *entry = *cursor;
		if (entry->id == id)
		{
			*cursor = entry->next;
			free (entry);
			return true;
		}
		cursor = &entry->next;
	}

	return false;
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

static qboolean FS_IsCancelTooOldLocked (const fs_canceled_id_t *entry)
{
	unsigned int age = fs_generation - entry->added_generation;
	return age > FS_CANCELED_PRUNE_GENERATIONS;
}

static void FS_PruneCanceledIdsLocked (void)
{
	fs_canceled_id_t **cursor = &fs_canceled_ids;
	while (*cursor)
	{
		fs_canceled_id_t *entry = *cursor;
		if (FS_IsCancelTooOldLocked (entry))
		{
			*cursor = entry->next;
			free (entry);
			continue;
		}

		cursor = &entry->next;
	}
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
	buf = (uint8_t *) malloc ((size_t) len + 1);
	if (!buf)
		Sys_Error ("FS_LoadMallocThread: out of memory");
	nread = fread (buf, 1, (size_t) len, f);
	fclose (f);
	if (nread != (size_t) len)
	{
		free (buf);
		return NULL;
	}
	buf[len] = 0;
	*len_out = (size_t) len;
	return buf;
}

static void FS_AsyncReadJob (void *userdata)
{
	fs_job_t *job = (fs_job_t *) userdata;
	fs_completion_t *comp = (fs_completion_t *) calloc (1, sizeof (*comp));
	if (!comp)
		Sys_Error ("FS_AsyncReadJob: out of memory");

	comp->cb = job->cb;
	comp->user = job->user;
	comp->id = job->id;
	comp->generation = job->generation;
	comp->data = FS_LoadMallocThread (job->path, &comp->len);
	comp->status = comp->data ? 0 : -1;

	SDL_LockMutex (fs_mutex);
	if (fs_comp_tail)
		fs_comp_tail->next = comp;
	else
		fs_comp_head = comp;
	fs_comp_tail = comp;
	SDL_UnlockMutex (fs_mutex);

	free (job);
}

fs_asyncread_handle_t FS_AsyncRead (const char *path, fs_async_cb cb, void *user)
{
	fs_asyncread_handle_t handle;
	fs_job_t *job;
	handle.id = 0;

	if (!Host_AsyncFSEnabled ())
	{
		size_t len;
		uint8_t *data = FS_LoadMallocThread (path, &len);
		cb (user, data, len, data ? 0 : -1);
		return handle;
	}

	job = (fs_job_t *) calloc (1, sizeof (*job));
	if (!job)
		Sys_Error ("FS_AsyncRead: out of memory");
	q_strlcpy (job->path, path, sizeof (job->path));
	job->cb = cb;
	job->user = user;

	SDL_LockMutex (fs_mutex);
	job->id = FS_AllocAsyncReadIdLocked ();
	job->generation = fs_generation;
	SDL_UnlockMutex (fs_mutex);

	handle.id = job->id;
	Jobs_SubmitDetached (FS_AsyncReadJob, job);
	return handle;
}

void FS_AsyncReadCancel (fs_asyncread_handle_t handle)
{
	fs_canceled_id_t *entry;

	if (!handle.id)
		return;

	entry = (fs_canceled_id_t *) calloc (1, sizeof (*entry));
	if (!entry)
		Sys_Error ("FS_AsyncReadCancel: out of memory");
	entry->id = handle.id;

	SDL_LockMutex (fs_mutex);
	if (FS_HasCanceledIdLocked (handle.id))
	{
		SDL_UnlockMutex (fs_mutex);
		free (entry);
		return;
	}
	if (FS_IsRecentCompletionLocked (handle.id))
	{
		SDL_UnlockMutex (fs_mutex);
		free (entry);
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
	entry->added_generation = fs_generation;
	entry->next = fs_canceled_ids;
	fs_canceled_ids = entry;
	SDL_UnlockMutex (fs_mutex);
}

void FS_AsyncAdvanceGeneration (void)
{
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
		canceled = FS_TakeCanceledIdLocked (list->id);
		stale = list->generation != fs_generation;
		SDL_UnlockMutex (fs_mutex);

		if (!canceled && !stale)
			list->cb (list->user, list->data, list->len, list->status);
		else
		{
			if (stale)
				list->cb (list->user, NULL, 0, -1);
			if (list->data)
				free (list->data);
		}
		free (list);
		list = next;
	}
}

void Jobs_Shutdown (void)
{
	jobnode_t *node;
	fs_completion_t *comp;
	fs_canceled_id_t *canceled;

	if (!jobs_mutex)
		return;

	SDL_LockMutex (jobs_mutex);
	jobs_shutdown = true;
	SDL_CondBroadcast (jobs_cond);
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
		free (jobs_threads);
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
			free (comp->data);
		free (comp);
		comp = next;
	}

	SDL_LockMutex (fs_mutex);
	canceled = fs_canceled_ids;
	fs_canceled_ids = NULL;
	SDL_UnlockMutex (fs_mutex);

	while (canceled)
	{
		fs_canceled_id_t *next = canceled->next;
		free (canceled);
		canceled = next;
	}

	while ((node = jobs_head) != NULL)
	{
		jobs_head = node->next;
		if (!node->detached)
		{
			SDL_LockMutex (node->handle->mutex);
			node->handle->done = true;
			SDL_CondSignal (node->handle->cond);
			SDL_UnlockMutex (node->handle->mutex);
		}
		free (node);
	}
	jobs_tail = NULL;

	SDL_DestroyCond (jobs_cond);
	SDL_DestroyMutex (jobs_mutex);
	jobs_cond = NULL;
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

	jobs_mutex = SDL_CreateMutex ();
	jobs_cond = SDL_CreateCond ();
	fs_mutex = SDL_CreateMutex ();
	if (!jobs_mutex || !jobs_cond || !fs_mutex)
		Sys_Error ("Jobs_Init: failed to initialize async primitives");

	{
		int i;
		int cpu_count;
		int worker_count;

		cpu_count = SDL_GetCPUCount ();
		if (cpu_count < 1)
			cpu_count = 1;
		worker_count = CLAMP (1, (int) host_async_workers.value, cpu_count);
		jobs_threads = (SDL_Thread **) calloc ((size_t) worker_count, sizeof (*jobs_threads));
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
