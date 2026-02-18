#include "quakedef.h"
#include "sys_jobs.h"

typedef struct
{
	sys_job_execute_fn execute;
	void *job_data;
} sys_job_entry_t;

struct sys_job_queue_s
{
	char name[32];
	size_t head;
	size_t tail;
	size_t capacity;
	qboolean teardown;
	SDL_mutex *mutex;
	SDL_cond *notfull;
	SDL_cond *notempty;
	SDL_Thread *thread;
	sys_job_entry_t *entries;
};

static int Sys_Jobs_WorkerMain (void *param)
{
	sys_job_queue_t *queue = (sys_job_queue_t *) param;

	for (;;)
	{
		sys_job_entry_t entry;

		SDL_LockMutex (queue->mutex);
		while (!queue->teardown && queue->head == queue->tail)
			SDL_CondWait (queue->notempty, queue->mutex);

		if (queue->teardown && queue->head == queue->tail)
		{
			SDL_UnlockMutex (queue->mutex);
			break;
		}

		entry = queue->entries[(queue->head++) & (queue->capacity - 1)];
		SDL_CondSignal (queue->notfull);
		SDL_UnlockMutex (queue->mutex);

		if (entry.execute)
			entry.execute (entry.job_data);
	}

	return 0;
}

sys_job_queue_t *Sys_Jobs_CreateQueue (const char *name, size_t capacity)
{
	sys_job_queue_t *queue;

	if (!capacity)
		capacity = 256;
	capacity = Q_nextPow2 (capacity);

	queue = (sys_job_queue_t *) calloc (1, sizeof (*queue));
	if (!queue)
		Sys_Error ("Sys_Jobs_CreateQueue: calloc failed");

	q_strlcpy (queue->name, name ? name : "JobQueue", sizeof (queue->name));
	queue->capacity = capacity;
	queue->entries = (sys_job_entry_t *) calloc (capacity, sizeof (queue->entries[0]));
	if (!queue->entries)
		Sys_Error ("Sys_Jobs_CreateQueue: calloc failed for queue entries");

	queue->mutex = SDL_CreateMutex ();
	queue->notfull = SDL_CreateCond ();
	queue->notempty = SDL_CreateCond ();
	if (!queue->mutex || !queue->notfull || !queue->notempty)
		Sys_Error ("Sys_Jobs_CreateQueue: failed to create synchronization primitives");

	queue->thread = SDL_CreateThread (Sys_Jobs_WorkerMain, queue->name, queue);
	if (!queue->thread)
		Sys_Error ("Sys_Jobs_CreateQueue: failed to create worker thread");

	return queue;
}

void Sys_Jobs_DestroyQueue (sys_job_queue_t *queue)
{
	if (!queue)
		return;

	SDL_LockMutex (queue->mutex);
	queue->teardown = true;
	SDL_UnlockMutex (queue->mutex);
	SDL_CondBroadcast (queue->notempty);
	SDL_CondBroadcast (queue->notfull);

	if (queue->thread)
		SDL_WaitThread (queue->thread, NULL);

	if (queue->notempty)
		SDL_DestroyCond (queue->notempty);
	if (queue->notfull)
		SDL_DestroyCond (queue->notfull);
	if (queue->mutex)
		SDL_DestroyMutex (queue->mutex);
	free (queue->entries);
	free (queue);
}

qboolean Sys_Jobs_Submit (sys_job_queue_t *queue, sys_job_execute_fn execute, void *job_data)
{
	if (!queue || !execute)
		return false;

	SDL_LockMutex (queue->mutex);
	while (!queue->teardown && queue->tail - queue->head >= queue->capacity)
		SDL_CondWait (queue->notfull, queue->mutex);

	if (queue->teardown)
	{
		SDL_UnlockMutex (queue->mutex);
		return false;
	}

	queue->entries[(queue->tail++) & (queue->capacity - 1)] = (sys_job_entry_t) { execute, job_data };
	SDL_CondSignal (queue->notempty);
	SDL_UnlockMutex (queue->mutex);
	return true;
}
