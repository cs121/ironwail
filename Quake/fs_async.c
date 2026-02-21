#include "quakedef.h"
#include "fs_async.h"
#include "sys_jobs.h"
#include "miniz.h"

#define FS_ASYNC_MAX_REQUESTS 1024
#define FS_ASYNC_COMPLETED_CAPACITY 1024
#define FS_ASYNC_DEFAULT_WORKERS 1

#define FS_ASYNC_HANDLE_INDEX_BITS 16
#define FS_ASYNC_HANDLE_INDEX_MASK ((1u << FS_ASYNC_HANDLE_INDEX_BITS) - 1u)
#define FS_ASYNC_MAKE_HANDLE(idx, gen) ((((uint32_t)(gen)) << FS_ASYNC_HANDLE_INDEX_BITS) | (uint32_t)(idx))
#define FS_ASYNC_HANDLE_INDEX(h) ((unsigned int)((h) & FS_ASYNC_HANDLE_INDEX_MASK))
#define FS_ASYNC_HANDLE_GEN(h) ((unsigned int)((h) >> FS_ASYNC_HANDLE_INDEX_BITS))

typedef struct
{
	qboolean in_use;
	unsigned int generation;
	char path[MAX_QPATH];
	int flags;
	fs_async_result_t result;
	qboolean completed;
} fs_async_request_t;

static qboolean fs_async_initialized;
static SDL_mutex *fs_async_mutex;
static SDL_cond *fs_async_done_cond;
static sys_job_queue_t *fs_async_queue;

static fs_async_handle_t fs_async_completed[FS_ASYNC_COMPLETED_CAPACITY];
static size_t fs_async_completed_head;
static size_t fs_async_completed_tail;

static fs_async_request_t fs_async_requests[FS_ASYNC_MAX_REQUESTS];

static cvar_t fs_async_enabled = {"fs_async_enabled", "0", CVAR_ARCHIVE};
static cvar_t fs_async_debug = {"fs_async_debug", "0", CVAR_NONE};

static uint64_t fs_async_enqueued;
static uint64_t fs_async_completed_count;
static uint64_t fs_async_completed_bytes;

extern qfileofs_t COM_filelength (FILE *f);

typedef struct
{
	qboolean is_pack;
	qboolean is_pk3;
	char filename[MAX_OSPATH];
	int numfiles;
	packfile_t *files;
} fs_async_searchpath_entry_t;

typedef struct
{
	fs_async_searchpath_entry_t *entries;
	size_t count;
} fs_async_searchpath_view_t;

static void FS_Async_ReleaseSearchPathView (fs_async_searchpath_view_t *view)
{
	size_t i;

	if (!view || !view->entries)
		return;

	for (i = 0; i < view->count; ++i)
	{
		if (view->entries[i].files)
			free (view->entries[i].files);
	}

	free (view->entries);
	view->entries = NULL;
	view->count = 0;
}

static qboolean FS_Async_CaptureSearchPathView (fs_async_searchpath_view_t *view)
{
	searchpath_t *search;
	size_t count = 0;
	size_t i = 0;

	memset (view, 0, sizeof (*view));

	COM_LockSearchPaths ();
	for (search = com_searchpaths; search; search = search->next)
		count++;

	if (!count)
	{
		COM_UnlockSearchPaths ();
		return true;
	}

	view->entries = (fs_async_searchpath_entry_t *) calloc (count, sizeof (*view->entries));
	if (!view->entries)
	{
		COM_UnlockSearchPaths ();
		return false;
	}
	view->count = count;

	for (search = com_searchpaths; search; search = search->next, ++i)
	{
		fs_async_searchpath_entry_t *entry = &view->entries[i];

		if (search->pack)
		{
			entry->is_pack = true;
			entry->is_pk3 = search->pack->is_pk3;
			q_strlcpy (entry->filename, search->pack->filename, sizeof (entry->filename));
			entry->numfiles = search->pack->numfiles;
			if (entry->numfiles > 0)
			{
				size_t n = (size_t) entry->numfiles;

				entry->files = (packfile_t *) malloc (n * sizeof (*entry->files));
				if (!entry->files)
				{
					COM_UnlockSearchPaths ();
					FS_Async_ReleaseSearchPathView (view);
					return false;
				}
				memcpy (entry->files, search->pack->files, n * sizeof (*entry->files));
			}
		}
		else
		{
			q_strlcpy (entry->filename, search->filename, sizeof (entry->filename));
		}
	}
	COM_UnlockSearchPaths ();

	return true;
}

/*
 * Thread-safety: this function runs on worker threads and only reads
 * request-local arguments plus the immutable snapshot in search_view.
 * It never dereferences com_searchpaths directly and therefore does not
 * require holding the global filesystem search-path lock while doing IO.
 */
static qboolean FS_Async_ReadFileSync (const char *path, const fs_async_searchpath_view_t *search_view, fs_async_result_t *result)
{
	size_t search_idx;

	for (search_idx = 0; search_idx < search_view->count; ++search_idx)
	{
		const fs_async_searchpath_entry_t *search = &search_view->entries[search_idx];

		if (search->is_pack)
		{
			int i;

			if (search->is_pk3)
			{
				mz_zip_archive zip;
				qboolean zip_open = false;

				memset (&zip, 0, sizeof (zip));
				if (mz_zip_reader_init_file (&zip, search->filename, 0))
					zip_open = true;

				if (!zip_open)
					continue;

				for (i = 0; i < search->numfiles; i++)
				{
					size_t extracted_size;
					void *extracted;

					if (q_strcasecmp (search->files[i].name, path) != 0)
						continue;

					extracted = mz_zip_reader_extract_to_heap (&zip, search->files[i].filepos, &extracted_size, 0);
					if (!extracted)
					{
						result->status = FS_ASYNC_STATUS_IO_ERROR;
						result->error_code = EIO;
						mz_zip_reader_end (&zip);
						return false;
					}

					result->data = extracted;
					result->size = extracted_size;
					result->status = FS_ASYNC_STATUS_OK;
					mz_zip_reader_end (&zip);
					return true;
				}

				mz_zip_reader_end (&zip);
			}
			else
			{
				for (i = 0; i < search->numfiles; i++)
				{
					FILE *f;
					long nread;
					int len;
					byte *buf;

					if (strcmp (search->files[i].name, path) != 0)
						continue;

					len = search->files[i].filelen;
					buf = (byte *) malloc ((size_t) len + 1);
					if (!buf)
					{
						result->status = FS_ASYNC_STATUS_IO_ERROR;
						result->error_code = ENOMEM;
						return false;
					}

					f = Sys_fopen (search->filename, "rb");
					if (!f)
					{
						free (buf);
						result->status = FS_ASYNC_STATUS_IO_ERROR;
						result->error_code = errno;
						return false;
					}

					if (fseek (f, search->files[i].filepos, SEEK_SET) != 0)
					{
						fclose (f);
						free (buf);
						result->status = FS_ASYNC_STATUS_IO_ERROR;
						result->error_code = errno;
						return false;
					}

					nread = (long) fread (buf, 1, (size_t) len, f);
					fclose (f);
					if (nread != len)
					{
						free (buf);
						result->status = FS_ASYNC_STATUS_IO_ERROR;
						result->error_code = EIO;
						return false;
					}

					buf[len] = 0;
					result->data = buf;
					result->size = (size_t) len;
					result->status = FS_ASYNC_STATUS_OK;
					return true;
				}
			}
		}
		else
		{
			char netpath[MAX_OSPATH];
			FILE *f;
			long len;
			long nread;
			byte *buf;

			if (!registered.value)
			{
				if (strchr (path, '/') || strchr (path, '\\'))
					continue;
			}

			q_snprintf (netpath, sizeof (netpath), "%s/%s", search->filename, path);
			if (!(Sys_FileType (netpath) & FS_ENT_FILE))
				continue;

			f = Sys_fopen (netpath, "rb");
			if (!f)
				continue;

			len = COM_filelength (f);
			if (len < 0)
			{
				fclose (f);
				result->status = FS_ASYNC_STATUS_IO_ERROR;
				result->error_code = EIO;
				return false;
			}

			buf = (byte *) malloc ((size_t) len + 1);
			if (!buf)
			{
				fclose (f);
				result->status = FS_ASYNC_STATUS_IO_ERROR;
				result->error_code = ENOMEM;
				return false;
			}

			nread = (long) fread (buf, 1, (size_t) len, f);
			fclose (f);
			if (nread != len)
			{
				free (buf);
				result->status = FS_ASYNC_STATUS_IO_ERROR;
				result->error_code = EIO;
				return false;
			}

			buf[len] = 0;
			result->data = buf;
			result->size = (size_t) len;
			result->status = FS_ASYNC_STATUS_OK;
			return true;
		}
	}

	result->status = FS_ASYNC_STATUS_NOT_FOUND;
	return false;
}

static void FS_Async_Complete (fs_async_handle_t h)
{
	size_t count;

	if (h == 0)
		return;

	count = fs_async_completed_tail - fs_async_completed_head;
	if (count >= FS_ASYNC_COMPLETED_CAPACITY)
		return;

	fs_async_completed[fs_async_completed_tail & (FS_ASYNC_COMPLETED_CAPACITY - 1)] = h;
	fs_async_completed_tail++;
}

static void FS_Async_Worker (void *job_data)
{
	fs_async_handle_t h = (fs_async_handle_t) (uintptr_t) job_data;
	unsigned int idx = FS_ASYNC_HANDLE_INDEX (h);
	unsigned int gen = FS_ASYNC_HANDLE_GEN (h);
	fs_async_result_t result;
	fs_async_searchpath_view_t search_view;
	char path[MAX_QPATH];
	qboolean ok;

	memset (&result, 0, sizeof (result));
	memset (&search_view, 0, sizeof (search_view));
	path[0] = 0;

	SDL_LockMutex (fs_async_mutex);
	if (idx >= FS_ASYNC_MAX_REQUESTS || !fs_async_requests[idx].in_use || fs_async_requests[idx].generation != gen)
	{
		SDL_UnlockMutex (fs_async_mutex);
		return;
	}
	q_strlcpy (path, fs_async_requests[idx].path, sizeof (path));
	SDL_UnlockMutex (fs_async_mutex);

	if (!FS_Async_CaptureSearchPathView (&search_view))
	{
		result.status = FS_ASYNC_STATUS_IO_ERROR;
		result.error_code = ENOMEM;
		ok = false;
	}
	else
	{
		ok = FS_Async_ReadFileSync (path, &search_view, &result);
		if (!ok && result.status == FS_ASYNC_STATUS_PENDING)
			result.status = FS_ASYNC_STATUS_IO_ERROR;
	}
	FS_Async_ReleaseSearchPathView (&search_view);

	SDL_LockMutex (fs_async_mutex);
	if (idx < FS_ASYNC_MAX_REQUESTS && fs_async_requests[idx].in_use && fs_async_requests[idx].generation == gen)
	{
		fs_async_requests[idx].result = result;
		fs_async_requests[idx].completed = true;
		FS_Async_Complete (h);
		SDL_CondBroadcast (fs_async_done_cond);
	}
	else if (result.data)
	{
		free (result.data);
	}
	SDL_UnlockMutex (fs_async_mutex);
}

void FS_Async_Init (void)
{
	if (fs_async_initialized)
		return;

	memset (fs_async_requests, 0, sizeof (fs_async_requests));
	fs_async_completed_head = fs_async_completed_tail = 0;
	fs_async_enqueued = 0;
	fs_async_completed_count = 0;
	fs_async_completed_bytes = 0;

	fs_async_mutex = SDL_CreateMutex ();
	fs_async_done_cond = SDL_CreateCond ();
	if (!fs_async_mutex || !fs_async_done_cond)
		Sys_Error ("FS_Async_Init: failed to create synchronization primitives");

	fs_async_queue = Sys_Jobs_CreateQueue ("FS Async", 256, FS_ASYNC_DEFAULT_WORKERS);
	if (!fs_async_queue)
		Sys_Error ("FS_Async_Init: failed to create fs async queue");

	Cvar_RegisterVariable (&fs_async_enabled);
	Cvar_RegisterVariable (&fs_async_debug);
	fs_async_initialized = true;
}

void FS_Async_Shutdown (void)
{
	unsigned int i;

	if (!fs_async_initialized)
		return;

	Sys_Jobs_DestroyQueue (fs_async_queue);
	fs_async_queue = NULL;

	SDL_LockMutex (fs_async_mutex);
	for (i = 0; i < FS_ASYNC_MAX_REQUESTS; ++i)
	{
		if (fs_async_requests[i].in_use && fs_async_requests[i].result.data)
			free (fs_async_requests[i].result.data);
		memset (&fs_async_requests[i], 0, sizeof (fs_async_requests[i]));
	}
	SDL_UnlockMutex (fs_async_mutex);

	SDL_DestroyCond (fs_async_done_cond);
	SDL_DestroyMutex (fs_async_mutex);
	fs_async_done_cond = NULL;
	fs_async_mutex = NULL;
	fs_async_initialized = false;
}

fs_async_handle_t FS_ReadFileAsync (const char *path, int flags)
{
	unsigned int i;
	fs_async_handle_t h = 0;

	if (!path || !*path)
		return 0;
	if (!fs_async_initialized)
		FS_Async_Init ();

	if (!fs_async_enabled.value)
		return 0;

	SDL_LockMutex (fs_async_mutex);
	for (i = 0; i < FS_ASYNC_MAX_REQUESTS; ++i)
	{
		if (fs_async_requests[i].in_use)
			continue;

		fs_async_requests[i].in_use = true;
		fs_async_requests[i].generation++;
		if (fs_async_requests[i].generation == 0)
			fs_async_requests[i].generation = 1;
		q_strlcpy (fs_async_requests[i].path, path, sizeof (fs_async_requests[i].path));
		fs_async_requests[i].flags = flags;
		memset (&fs_async_requests[i].result, 0, sizeof (fs_async_requests[i].result));
		fs_async_requests[i].result.status = FS_ASYNC_STATUS_PENDING;
		fs_async_requests[i].completed = false;
		h = FS_ASYNC_MAKE_HANDLE (i, fs_async_requests[i].generation);
		break;
	}
	SDL_UnlockMutex (fs_async_mutex);

	if (!h)
		return 0;

	if (!Sys_Jobs_Submit (fs_async_queue, FS_Async_Worker, (void *) (uintptr_t) h))
	{
		FS_Release (h);
		return 0;
	}

	fs_async_enqueued++;
	return h;
}

qboolean FS_Poll (fs_async_handle_t h, fs_async_result_t *out)
{
	unsigned int idx = FS_ASYNC_HANDLE_INDEX (h);
	unsigned int gen = FS_ASYNC_HANDLE_GEN (h);

	if (out)
		memset (out, 0, sizeof (*out));
	if (!h || idx >= FS_ASYNC_MAX_REQUESTS || !fs_async_initialized)
		return false;

	SDL_LockMutex (fs_async_mutex);
	if (!fs_async_requests[idx].in_use || fs_async_requests[idx].generation != gen)
	{
		SDL_UnlockMutex (fs_async_mutex);
		if (out)
			out->status = FS_ASYNC_STATUS_BAD_HANDLE;
		return false;
	}

	if (!fs_async_requests[idx].completed)
	{
		SDL_UnlockMutex (fs_async_mutex);
		return false;
	}

	if (out)
		*out = fs_async_requests[idx].result;
	SDL_UnlockMutex (fs_async_mutex);
	return true;
}

void FS_Wait (fs_async_handle_t h, fs_async_result_t *out)
{
	unsigned int idx = FS_ASYNC_HANDLE_INDEX (h);
	unsigned int gen = FS_ASYNC_HANDLE_GEN (h);

	if (out)
		memset (out, 0, sizeof (*out));
	if (!h || idx >= FS_ASYNC_MAX_REQUESTS || !fs_async_initialized)
		return;

	SDL_LockMutex (fs_async_mutex);
	while (fs_async_requests[idx].in_use && fs_async_requests[idx].generation == gen && !fs_async_requests[idx].completed)
		SDL_CondWait (fs_async_done_cond, fs_async_mutex);

	if (fs_async_requests[idx].in_use && fs_async_requests[idx].generation == gen && fs_async_requests[idx].completed)
	{
		if (out)
			*out = fs_async_requests[idx].result;
		}
	else if (out)
	{
		out->status = FS_ASYNC_STATUS_BAD_HANDLE;
	}
	SDL_UnlockMutex (fs_async_mutex);
}

void FS_Release (fs_async_handle_t h)
{
	unsigned int idx = FS_ASYNC_HANDLE_INDEX (h);
	unsigned int gen = FS_ASYNC_HANDLE_GEN (h);

	if (!h || idx >= FS_ASYNC_MAX_REQUESTS || !fs_async_initialized)
		return;

	SDL_LockMutex (fs_async_mutex);
	if (fs_async_requests[idx].in_use && fs_async_requests[idx].generation == gen)
	{
		if (!fs_async_requests[idx].completed)
		{
			SDL_UnlockMutex (fs_async_mutex);
			FS_Wait (h, NULL);
			SDL_LockMutex (fs_async_mutex);
		}

		if (fs_async_requests[idx].result.data)
			free (fs_async_requests[idx].result.data);
		memset (&fs_async_requests[idx], 0, sizeof (fs_async_requests[idx]));
	}
	SDL_UnlockMutex (fs_async_mutex);
}

void FS_Async_Pump (void)
{
	uint64_t pumped = 0;
	uint64_t bytes = 0;

	if (!fs_async_initialized)
		return;

	SDL_LockMutex (fs_async_mutex);
	while (fs_async_completed_head != fs_async_completed_tail)
	{
		fs_async_handle_t h = fs_async_completed[fs_async_completed_head & (FS_ASYNC_COMPLETED_CAPACITY - 1)];
		unsigned int idx = FS_ASYNC_HANDLE_INDEX (h);
		unsigned int gen = FS_ASYNC_HANDLE_GEN (h);

		fs_async_completed_head++;
		if (idx < FS_ASYNC_MAX_REQUESTS && fs_async_requests[idx].in_use && fs_async_requests[idx].generation == gen)
		{
			pumped++;
			bytes += fs_async_requests[idx].result.size;
		}
	}

	fs_async_completed_count += pumped;
	fs_async_completed_bytes += bytes;
	SDL_UnlockMutex (fs_async_mutex);

	if (fs_async_debug.value && pumped)
	{
		double avg = (double) fs_async_completed_bytes / (double) (fs_async_completed_count ? fs_async_completed_count : 1);
		Con_Printf ("fs_async: enqueued=%" SDL_PRIu64 " completed=%" SDL_PRIu64 " avg_bytes=%.1f\n",
			fs_async_enqueued, fs_async_completed_count, avg);
	}
}
