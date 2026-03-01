#include "quakedef.h"

typedef struct jobnode_s {
	jobs_func_t func;
	void *userdata;
	JobHandle *handle;
	struct jobnode_s *next;
} jobnode_t;

static SDL_Thread *jobs_thread;
static SDL_mutex *jobs_mutex;
static SDL_cond *jobs_cond;
static jobnode_t *jobs_head;
static jobnode_t *jobs_tail;
static qboolean jobs_shutdown;

static cvar_t host_async = {"host_async", "0", CVAR_ARCHIVE};
static cvar_t host_async_fs = {"host_async_fs", "0", CVAR_ARCHIVE};
static cvar_t host_async_assets = {"host_async_assets", "0", CVAR_ARCHIVE};

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
		SDL_UnlockMutex (jobs_mutex);

		node->func (node->userdata);
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

	if (!Host_AsyncEnabled () || !jobs_thread)
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

	SDL_LockMutex (jobs_mutex);
	if (jobs_tail)
		jobs_tail->next = node;
	else
		jobs_head = node;
	jobs_tail = node;
	SDL_CondSignal (jobs_cond);
	SDL_UnlockMutex (jobs_mutex);

	return handle;
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
static unsigned int fs_cancel_before;
static unsigned int fs_generation;

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
	job->id = fs_next_id++;
	job->generation = fs_generation;
	SDL_UnlockMutex (fs_mutex);

	handle.id = job->id;
	Jobs_Submit (FS_AsyncReadJob, job);
	return handle;
}

void FS_AsyncReadCancel (fs_asyncread_handle_t handle)
{
	if (!handle.id)
		return;
	SDL_LockMutex (fs_mutex);
	if (handle.id > fs_cancel_before)
		fs_cancel_before = handle.id;
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
	list = fs_comp_head;
	fs_comp_head = fs_comp_tail = NULL;
	SDL_UnlockMutex (fs_mutex);

	while (list)
	{
		fs_completion_t *next = list->next;
		qboolean canceled;

		SDL_LockMutex (fs_mutex);
		canceled = list->id <= fs_cancel_before || list->generation != fs_generation;
		SDL_UnlockMutex (fs_mutex);

		if (!canceled)
			list->cb (list->user, list->data, list->len, list->status);
		else if (list->data)
			free (list->data);
		free (list);
		list = next;
	}
}

void Jobs_Shutdown (void)
{
	jobnode_t *node;

	if (!jobs_mutex)
		return;

	SDL_LockMutex (jobs_mutex);
	jobs_shutdown = true;
	SDL_CondBroadcast (jobs_cond);
	SDL_UnlockMutex (jobs_mutex);

	if (jobs_thread)
		SDL_WaitThread (jobs_thread, NULL);
	jobs_thread = NULL;

	while ((node = jobs_head) != NULL)
	{
		jobs_head = node->next;
		SDL_LockMutex (node->handle->mutex);
		node->handle->done = true;
		SDL_CondSignal (node->handle->cond);
		SDL_UnlockMutex (node->handle->mutex);
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

	jobs_mutex = SDL_CreateMutex ();
	jobs_cond = SDL_CreateCond ();
	fs_mutex = SDL_CreateMutex ();
	if (!jobs_mutex || !jobs_cond || !fs_mutex)
		Sys_Error ("Jobs_Init: failed to initialize async primitives");

	jobs_thread = SDL_CreateThread (Jobs_Worker, "jobs", NULL);
	if (!jobs_thread)
		Sys_Error ("Jobs_Init: failed to create worker thread");
}
