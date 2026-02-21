#include "quakedef.h"
#include "asset_decode_async.h"
#include "sys_jobs.h"
#include "lodepng.h"
#include "opengl/gl_ktx2.h"

#define ASSET_MAX_REQUESTS 512
#define ASSET_HANDLE_INDEX_BITS 16
#define ASSET_HANDLE_INDEX_MASK ((1u << ASSET_HANDLE_INDEX_BITS) - 1u)
#define ASSET_MAKE_HANDLE(idx, gen) ((((uint32_t)(gen)) << ASSET_HANDLE_INDEX_BITS) | (uint32_t)(idx))
#define ASSET_HANDLE_INDEX(h) ((unsigned int)((h) & ASSET_HANDLE_INDEX_MASK))
#define ASSET_HANDLE_GEN(h) ((unsigned int)((h) >> ASSET_HANDLE_INDEX_BITS))

typedef enum { ASSET_STAGE_EMPTY = 0, ASSET_STAGE_IO_PENDING, ASSET_STAGE_DECODE_PENDING, ASSET_STAGE_FINALIZE_PENDING, ASSET_STAGE_FAILED } asset_stage_t;

typedef struct {
	qboolean in_use;
	unsigned int generation;
	asset_stage_t stage;
	int active_prev;
	int active_next;
	qboolean in_active_set;
	asset_kind_t kind;
	uint64_t t_io_start_us;
	char path[MAX_QPATH];
	asset_tex_params_t params;
	char key[MAX_QPATH + 64];
	fs_async_handle_t fs_handle;
	void *raw_data;
	size_t raw_size;
	qboolean decode_queued;
	asset_result_t result;
} asset_request_t;

static asset_request_t asset_requests[ASSET_MAX_REQUESTS];
static SDL_mutex *asset_mutex;
static SDL_cond *asset_done_cond;
static sys_job_queue_t *asset_decode_queue;
static qboolean asset_initialized;
static Uint32 asset_main_thread_id;
static int asset_active_head = -1;

#ifndef ASSET_ASYNC_DEBUG_FULLSCAN_COMPARE
#define ASSET_ASYNC_DEBUG_FULLSCAN_COMPARE 0
#endif

#define ASSET_WAIT_PUMP_INTERVAL_MS 16
#define ASSET_WAIT_WARN_THRESHOLD_MS 250
#define ASSET_WAIT_WARN_REPEAT_MS 1000

static cvar_t asset_async = {"asset_async", "0", CVAR_ARCHIVE};
static cvar_t asset_async_max_inflight_mb = {"asset_async_max_inflight_mb", "256", CVAR_ARCHIVE};
static cvar_t asset_async_debug = {"asset_async_debug", "0", CVAR_NONE};
static cvar_t asset_async_decode_workers = {"asset_async_decode_workers", "1", CVAR_ARCHIVE};

static uint64_t asset_queued_fs;
static uint64_t asset_completed_fs;
static uint64_t asset_queued_decode;
static uint64_t asset_completed_decode;
static uint64_t asset_failed_decode;
static uint64_t asset_decode_submit_backpressure;
static uint64_t asset_decode_us_png;
static uint64_t asset_decode_us_ktx2;
static size_t asset_inflight_raw_bytes;
static size_t asset_inflight_decoded_bytes;
static uint64_t asset_pump_calls;
static uint64_t asset_pump_visited_slots;
static uint32_t asset_pump_last_visited_slots;

typedef enum
{
	ASSET_DECODE_SUBMIT_POLICY_BLOCKING = 0,
	ASSET_DECODE_SUBMIT_POLICY_NONBLOCKING
} asset_decode_submit_policy_t;

/* Pump runs in the frame loop; keep decode queue submission explicitly non-blocking. */
static const asset_decode_submit_policy_t asset_decode_submit_policy = ASSET_DECODE_SUBMIT_POLICY_NONBLOCKING;

static const char *Asset_StageName(asset_stage_t stage)
{
	switch (stage)
	{
	case ASSET_STAGE_EMPTY: return "empty";
	case ASSET_STAGE_IO_PENDING: return "io_pending";
	case ASSET_STAGE_DECODE_PENDING: return "decode_pending";
	case ASSET_STAGE_FINALIZE_PENDING: return "finalize_pending";
	case ASSET_STAGE_FAILED: return "failed";
	default: return "unknown";
	}
}

static qboolean Asset_CanPumpCurrentThread(void)
{
	return asset_main_thread_id != 0 && SDL_ThreadID() == asset_main_thread_id;
}

static qboolean Asset_IsKTX2(const void *data, size_t size)
{
	static const uint8_t magic[12] = {0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
	return size >= sizeof(magic) && memcmp(data, magic, sizeof(magic)) == 0;
}

static qboolean Asset_StageNeedsActiveSet(asset_stage_t stage)
{
	return stage == ASSET_STAGE_IO_PENDING || stage == ASSET_STAGE_DECODE_PENDING;
}

/*
 * Active-set invariants (always under asset_mutex):
 * - Any request in IO_PENDING/DECODE_PENDING must be linked exactly once.
 * - Requests in any other stage must not be linked.
 * - active_prev/active_next are valid only while in_active_set == true.
 */
static void Asset_ActiveSetRemoveLocked(unsigned idx)
{
	asset_request_t *req = &asset_requests[idx];
	if (!req->in_active_set)
		return;
	if (req->active_prev >= 0)
		asset_requests[req->active_prev].active_next = req->active_next;
	else
		asset_active_head = req->active_next;
	if (req->active_next >= 0)
		asset_requests[req->active_next].active_prev = req->active_prev;
	req->active_prev = -1;
	req->active_next = -1;
	req->in_active_set = false;
}

static void Asset_ActiveSetInsertLocked(unsigned idx)
{
	asset_request_t *req = &asset_requests[idx];
	if (req->in_active_set)
		return;
	req->active_prev = -1;
	req->active_next = asset_active_head;
	if (asset_active_head >= 0)
		asset_requests[asset_active_head].active_prev = (int)idx;
	asset_active_head = (int)idx;
	req->in_active_set = true;
}

static void Asset_SetStageLocked(unsigned idx, asset_stage_t stage)
{
	asset_request_t *req = &asset_requests[idx];
	if (req->stage == stage)
		return;
	if (Asset_StageNeedsActiveSet(req->stage) && !Asset_StageNeedsActiveSet(stage))
		Asset_ActiveSetRemoveLocked(idx);
	else if (!Asset_StageNeedsActiveSet(req->stage) && Asset_StageNeedsActiveSet(stage))
		Asset_ActiveSetInsertLocked(idx);
	req->stage = stage;
}

static void Asset_ResetRequestLocked(unsigned idx)
{
	Asset_ActiveSetRemoveLocked(idx);
	memset(&asset_requests[idx], 0, sizeof(asset_requests[idx]));
}

static void Asset_FreeDecodeJob(asset_decode_job_t *job)
{
	int i;

	if (!job)
		return;

	if (job->kind == ASSET_KIND_SOUND)
	{
		free(job->payload.audio.data);
	}
	else
	{
		for (i = 0; i < job->payload.image.mip_count; ++i)
			free(job->payload.image.mips[i].data);
	}

	free(job);
}

static size_t Asset_DecodeJobBytes(const asset_decode_job_t *job)
{
	size_t total = 0;
	int i;

	if (!job)
		return 0;

	if (job->kind == ASSET_KIND_SOUND)
		return job->payload.audio.size;

	for (i = 0; i < job->payload.image.mip_count; ++i)
		total += job->payload.image.mips[i].size;

	return total;
}


static asset_decode_job_t *Asset_DecodePNG(const void *raw_data, size_t raw_size)
{
	asset_decode_job_t *job;
	unsigned char *rgba = NULL;
	unsigned w = 0, h = 0;
	unsigned err;

	err = lodepng_decode32(&rgba, &w, &h, (const unsigned char *)raw_data, raw_size);
	if (err || !rgba)
		return NULL;

	job = (asset_decode_job_t *)calloc(1, sizeof(*job));
	if (!job)
	{
		free(rgba);
		return NULL;
	}

	job->kind = ASSET_KIND_PNG;
	job->target_format = ASSET_BACKEND_FMT_RGBA8;
	job->payload.image.width = (int)w;
	job->payload.image.height = (int)h;
	job->payload.image.mip_count = 1;
	job->payload.image.format = ASSET_IMAGE_FMT_RGBA8;
	job->payload.image.has_alpha = true;
	job->payload.image.mips[0].data = rgba;
	job->payload.image.mips[0].size = (size_t)w * (size_t)h * 4u;
	job->payload.image.mips[0].pitch = (size_t)w * 4u;
	return job;
}

static asset_decode_job_t *Asset_DecodeKTX2(const void *raw_data, size_t raw_size)
{
	ktx2_header_t hdr;
	ktx2_decoded_image_t decoded;
	asset_decode_job_t *job;
	int i;

	memset(&decoded, 0, sizeof(decoded));
	if (!KTX2_ParseHeaderPublic(&hdr, (const uint8_t *)raw_data, raw_size))
		return NULL;
	if (!KTX2_TranscodeToRGBA((const uint8_t *)raw_data, raw_size, &hdr, &decoded))
		return NULL;

	job = (asset_decode_job_t *)calloc(1, sizeof(*job));
	if (!job)
	{
		KTX2_FreeDecodedImage(&decoded);
		return NULL;
	}

	job->kind = ASSET_KIND_KTX2;
	job->target_format = KTX2_SelectBackendTargetFormat();
	job->payload.image.width = decoded.width[0];
	job->payload.image.height = decoded.height[0];
	job->payload.image.mip_count = decoded.mip_count;
	job->payload.image.format = ASSET_IMAGE_FMT_RGBA8;
	job->payload.image.has_alpha = true;

	for (i = 0; i < decoded.mip_count; ++i)
	{
		job->payload.image.mips[i].data = decoded.mip_data[i];
		job->payload.image.mips[i].size = decoded.mip_size[i];
		job->payload.image.mips[i].pitch = (size_t)decoded.width[i] * 4u;
		decoded.mip_data[i] = NULL;
		decoded.mip_size[i] = 0;
	}

	KTX2_FreeDecodedImage(&decoded);
	return job;
}

static void Asset_DecodeWorker(void *job_data)
{
	asset_handle_t h = (asset_handle_t)(uintptr_t)job_data;
	unsigned idx = ASSET_HANDLE_INDEX(h);
	unsigned gen = ASSET_HANDLE_GEN(h);
	void *raw_data = NULL;
	size_t raw_size = 0;
	asset_kind_t kind = ASSET_KIND_UNKNOWN;
	uint64_t t0_us = 0;
	uint64_t decode_us = 0;
	asset_decode_job_t *job = NULL;

	SDL_LockMutex(asset_mutex);
	if (idx >= ASSET_MAX_REQUESTS || !asset_requests[idx].in_use || asset_requests[idx].generation != gen)
	{
		SDL_UnlockMutex(asset_mutex);
		return;
	}
	raw_data = asset_requests[idx].raw_data;
	raw_size = asset_requests[idx].raw_size;
	kind = asset_requests[idx].kind;
	t0_us = SDL_GetPerformanceCounter();
	asset_requests[idx].raw_data = NULL;
	asset_requests[idx].raw_size = 0;
	asset_inflight_raw_bytes -= raw_size;
	SDL_UnlockMutex(asset_mutex);

	if (kind == ASSET_KIND_KTX2 || Asset_IsKTX2(raw_data, raw_size))
		job = Asset_DecodeKTX2(raw_data, raw_size);
	else
		job = Asset_DecodePNG(raw_data, raw_size);

	decode_us = (uint64_t)((SDL_GetPerformanceCounter() - t0_us) * 1000000ull / SDL_GetPerformanceFrequency());
	free(raw_data);

	SDL_LockMutex(asset_mutex);
	if (idx < ASSET_MAX_REQUESTS && asset_requests[idx].in_use && asset_requests[idx].generation == gen)
	{
		asset_completed_decode++;
		asset_requests[idx].result.decode_us = decode_us;
		if (kind == ASSET_KIND_KTX2)
			asset_decode_us_ktx2 += decode_us;
		else
			asset_decode_us_png += decode_us;
		if (job)
		{
			asset_requests[idx].result.status = ASSET_RESULT_OK;
			asset_requests[idx].result.job = job;
			Asset_SetStageLocked(idx, ASSET_STAGE_FINALIZE_PENDING);
			asset_inflight_decoded_bytes += Asset_DecodeJobBytes(job);
		}
		else
		{
			asset_failed_decode++;
			asset_requests[idx].result.status = ASSET_RESULT_FAILED;
			q_snprintf(asset_requests[idx].result.error, sizeof(asset_requests[idx].result.error), "decode failed");
			Asset_SetStageLocked(idx, ASSET_STAGE_FAILED);
		}
		if (asset_async_debug.value)
		{
			Con_DPrintf("asset_async_done: %s kind=%d status=%d io_ms=%.3f decode_ms=%.3f\n",
				asset_requests[idx].path, (int)asset_requests[idx].result.kind, (int)asset_requests[idx].result.status,
				(double)asset_requests[idx].result.io_us / 1000.0, (double)asset_requests[idx].result.decode_us / 1000.0);
		}
		SDL_CondBroadcast(asset_done_cond);
	}
	else if (job)
	{
		Asset_FreeDecodeJob(job);
	}
	SDL_UnlockMutex(asset_mutex);
}

/*
 * Asset async API contract and thread-affinity rules:
 * - Asset_Async_Pump() must run on the thread that called Asset_Async_Init()
 *   (normally the main thread). It advances FS polling and decode scheduling.
 * - Asset_Poll() is non-blocking and opportunistically pumps; call it from the
 *   main thread/frame loop.
 * - Asset_Wait() may block on any thread. On the main thread it performs a
 *   timed wait loop and pumps work periodically; on other threads it only waits
 *   and therefore relies on main-thread pumping to make progress.
 * - Asset_Release() is synchronous: it ensures completion before freeing state,
 *   and triggers an immediate pump when called on the main thread so callers do
 *   not silently depend on external pumping right before release.
 *
 * Wait-time diagnostics:
 * - Asset_Wait() logs a warning after a threshold and repeats periodically with
 *   handle/path/stage context. It does not enforce a functional timeout abort.
 */

void Asset_Async_Init (void)
{
	if (asset_initialized)
		return;
	asset_mutex = SDL_CreateMutex();
	asset_done_cond = SDL_CreateCond();
	asset_decode_queue = Sys_Jobs_CreateQueue("Asset Decode", 256, (size_t)q_max(1.0f, asset_async_decode_workers.value));
	if (!asset_mutex || !asset_done_cond || !asset_decode_queue)
		Sys_Error("Asset_Async_Init failed");
	asset_main_thread_id = SDL_ThreadID();
	Cvar_RegisterVariable(&asset_async);
	Cvar_RegisterVariable(&asset_async_max_inflight_mb);
	Cvar_RegisterVariable(&asset_async_debug);
	Cvar_RegisterVariable(&asset_async_decode_workers);
	asset_initialized = true;
}

void Asset_Async_Shutdown (void)
{
	unsigned i;
	if (!asset_initialized)
		return;
	Sys_Jobs_DestroyQueue(asset_decode_queue);
	asset_decode_queue = NULL;
	SDL_LockMutex(asset_mutex);
	for (i = 0; i < ASSET_MAX_REQUESTS; ++i)
	{
		if (!asset_requests[i].in_use)
			continue;
		if (asset_requests[i].fs_handle)
			FS_Release(asset_requests[i].fs_handle);
		if (asset_requests[i].raw_data)
			free(asset_requests[i].raw_data);
		if (asset_requests[i].result.job)
		{
			Asset_FreeDecodeJob(asset_requests[i].result.job);
		}
		Asset_ResetRequestLocked(i);
	}
	SDL_UnlockMutex(asset_mutex);
	SDL_DestroyCond(asset_done_cond);
	SDL_DestroyMutex(asset_mutex);
	asset_done_cond = NULL;
	asset_mutex = NULL;
	asset_main_thread_id = 0;
	asset_initialized = false;
}

asset_handle_t Asset_LoadTextureAsync (const char *path, const asset_tex_params_t *params)
{
	asset_handle_t h = 0;
	fs_async_handle_t fs_handle = 0;
	unsigned i;
	size_t budget;

	if (!asset_initialized)
		Asset_Async_Init();
	if (!asset_async.value || !path || !*path)
		return 0;

	budget = (size_t)(q_max(16.f, asset_async_max_inflight_mb.value) * 1024.f * 1024.f);

	SDL_LockMutex(asset_mutex);
	for (i = 0; i < ASSET_MAX_REQUESTS; ++i)
	{
		if (!asset_requests[i].in_use)
			continue;
		if (params) { char key[MAX_QPATH + 64]; q_snprintf(key, sizeof(key), "%s|%u", path, params->flags); if (!strcmp(asset_requests[i].key, key))
			{
				h = ASSET_MAKE_HANDLE(i, asset_requests[i].generation);
				SDL_UnlockMutex(asset_mutex);
				return h;
			}
		}
		else if (!strcmp(asset_requests[i].key, path))
		{
			h = ASSET_MAKE_HANDLE(i, asset_requests[i].generation);
			SDL_UnlockMutex(asset_mutex);
			return h;
		}
	}

	if (asset_inflight_raw_bytes + asset_inflight_decoded_bytes >= budget)
	{
		SDL_UnlockMutex(asset_mutex);
		return 0;
	}

	for (i = 0; i < ASSET_MAX_REQUESTS; ++i)
	{
		if (asset_requests[i].in_use)
			continue;
		asset_requests[i].in_use = true;
		asset_requests[i].generation++;
		if (!asset_requests[i].generation)
			asset_requests[i].generation = 1;
		q_strlcpy(asset_requests[i].path, path, sizeof(asset_requests[i].path));
		q_snprintf(asset_requests[i].key, sizeof(asset_requests[i].key), "%s|%u", path, params ? params->flags : 0u);
		if (params)
			asset_requests[i].params = *params;
		asset_requests[i].stage = ASSET_STAGE_EMPTY;
		asset_requests[i].active_prev = -1;
		asset_requests[i].active_next = -1;
		asset_requests[i].in_active_set = false;
		Asset_SetStageLocked(i, ASSET_STAGE_IO_PENDING);
		asset_requests[i].kind = ASSET_KIND_UNKNOWN;
		asset_requests[i].t_io_start_us = SDL_GetPerformanceCounter();
		asset_requests[i].result.kind = ASSET_KIND_UNKNOWN;
		asset_requests[i].result.error_code = 0;
		asset_requests[i].result.error[0] = 0;
		asset_requests[i].result.io_us = 0;
		asset_requests[i].result.decode_us = 0;
		asset_requests[i].decode_queued = false;
		asset_requests[i].result.status = ASSET_RESULT_PENDING;
		h = ASSET_MAKE_HANDLE(i, asset_requests[i].generation);
		break;
	}
	SDL_UnlockMutex(asset_mutex);

	if (!h)
		return 0;

	fs_handle = FS_ReadFileAsync(path, FS_ASYNC_FLAG_ALLOW_DECOMPRESS);
	if (!fs_handle)
	{
		const unsigned idx = ASSET_HANDLE_INDEX(h);
		const unsigned gen = ASSET_HANDLE_GEN(h);

		SDL_LockMutex(asset_mutex);
		if (asset_requests[idx].in_use && asset_requests[idx].generation == gen)
			Asset_ResetRequestLocked(idx);
		SDL_UnlockMutex(asset_mutex);
		return 0;
	}

	SDL_LockMutex(asset_mutex);
	{
		const unsigned idx = ASSET_HANDLE_INDEX(h);
		const unsigned gen = ASSET_HANDLE_GEN(h);

		if (!asset_requests[idx].in_use || asset_requests[idx].generation != gen)
		{
			SDL_UnlockMutex(asset_mutex);
			FS_Release(fs_handle);
			return 0;
		}

		/* asset_requests[idx].fs_handle is protected by asset_mutex for all reads/writes. */
		asset_requests[idx].fs_handle = fs_handle;
		asset_queued_fs++;
	}
	SDL_UnlockMutex(asset_mutex);
	return h;
}

/* Main thread only: this pump must never block on worker queue backpressure. */
void Asset_Async_Pump (void)
{
	unsigned visited_slots = 0;
	int idx;
	if (!asset_initialized)
		return;

	asset_pump_calls++;

	SDL_LockMutex(asset_mutex);
	idx = asset_active_head;
	SDL_UnlockMutex(asset_mutex);

	while (idx >= 0)
	{
		asset_handle_t h = 0;
		unsigned i = (unsigned)idx;
		fs_async_handle_t fs_handle = 0;
		fs_async_result_t fs;
		qboolean has_fs;
		qboolean submit_decode = false;
		int next_idx = -1;

		visited_slots++;

		SDL_LockMutex(asset_mutex);
		next_idx = asset_requests[i].active_next;
		if (!asset_requests[i].in_use || !asset_requests[i].in_active_set)
		{
			SDL_UnlockMutex(asset_mutex);
			idx = next_idx;
			continue;
		}

		if (asset_requests[i].stage == ASSET_STAGE_IO_PENDING && asset_requests[i].fs_handle)
		{
			h = ASSET_MAKE_HANDLE(i, asset_requests[i].generation);
			fs_handle = asset_requests[i].fs_handle;
		}
		else if (asset_requests[i].stage == ASSET_STAGE_DECODE_PENDING && !asset_requests[i].decode_queued && asset_requests[i].raw_data && asset_requests[i].raw_size)
		{
			h = ASSET_MAKE_HANDLE(i, asset_requests[i].generation);
			asset_requests[i].decode_queued = true;
			submit_decode = true;
		}
		SDL_UnlockMutex(asset_mutex);

		if (submit_decode)
		{
			if ((asset_decode_submit_policy == ASSET_DECODE_SUBMIT_POLICY_BLOCKING
				? Sys_Jobs_Submit(asset_decode_queue, Asset_DecodeWorker, (void *)(uintptr_t)h)
				: Sys_Jobs_TrySubmit(asset_decode_queue, Asset_DecodeWorker, (void *)(uintptr_t)h)))
			{
				SDL_LockMutex(asset_mutex);
				asset_queued_decode++;
				SDL_UnlockMutex(asset_mutex);
			}
			else
			{
				SDL_LockMutex(asset_mutex);
				asset_decode_submit_backpressure++;
				if (asset_requests[i].in_use && asset_requests[i].generation == ASSET_HANDLE_GEN(h) && asset_requests[i].stage == ASSET_STAGE_DECODE_PENDING)
					asset_requests[i].decode_queued = false;
				SDL_UnlockMutex(asset_mutex);
			}
			idx = next_idx;
			continue;
		}

		if (!fs_handle)
		{
			idx = next_idx;
			continue;
		}

		has_fs = FS_Poll(fs_handle, &fs);
		if (!has_fs)
		{
			idx = next_idx;
			continue;
		}

		SDL_LockMutex(asset_mutex);
		if (!asset_requests[i].in_use || asset_requests[i].generation != ASSET_HANDLE_GEN(h) || asset_requests[i].stage != ASSET_STAGE_IO_PENDING)
		{
			SDL_UnlockMutex(asset_mutex);
			idx = next_idx;
			continue;
		}
		asset_completed_fs++;
		if (fs.status == FS_ASYNC_STATUS_OK && fs.data && fs.size)
		{
			asset_requests[i].raw_data = malloc(fs.size);
			if (asset_requests[i].raw_data)
			{
				memcpy(asset_requests[i].raw_data, fs.data, fs.size);
				asset_requests[i].raw_size = fs.size;
				asset_requests[i].decode_queued = false;
				asset_inflight_raw_bytes += fs.size;
				Asset_SetStageLocked(i, ASSET_STAGE_DECODE_PENDING);
				asset_requests[i].kind = Asset_IsKTX2(fs.data, fs.size) ? ASSET_KIND_KTX2 : ASSET_KIND_PNG;
				asset_requests[i].result.kind = asset_requests[i].kind;
				asset_requests[i].result.io_us = (uint64_t)((SDL_GetPerformanceCounter() - asset_requests[i].t_io_start_us) * 1000000ull / SDL_GetPerformanceFrequency());
			}
			else
			{
				asset_requests[i].result.status = ASSET_RESULT_FAILED;
				asset_requests[i].result.error_code = ENOMEM;
				q_strlcpy(asset_requests[i].result.error, "malloc failed", sizeof(asset_requests[i].result.error));
				Asset_SetStageLocked(i, ASSET_STAGE_FAILED);
				SDL_CondBroadcast(asset_done_cond);
			}
		}
		else
		{
			asset_requests[i].result.status = (fs.status == FS_ASYNC_STATUS_NOT_FOUND) ? ASSET_RESULT_NOT_FOUND : ASSET_RESULT_FAILED;
			asset_requests[i].result.error_code = fs.error_code;
			q_snprintf(asset_requests[i].result.error, sizeof(asset_requests[i].result.error), "fs status=%d", (int)fs.status);
			Asset_SetStageLocked(i, ASSET_STAGE_FAILED);
			SDL_CondBroadcast(asset_done_cond);
		}
		FS_Release(asset_requests[i].fs_handle);
		asset_requests[i].fs_handle = 0;
		SDL_UnlockMutex(asset_mutex);

		idx = next_idx;
	}

	asset_pump_last_visited_slots = visited_slots;
	asset_pump_visited_slots += visited_slots;

#if ASSET_ASYNC_DEBUG_FULLSCAN_COMPARE
	{
		unsigned fullscan_active = 0;
		unsigned set_active = 0;
		int walk;
		SDL_LockMutex(asset_mutex);
		for (unsigned i = 0; i < ASSET_MAX_REQUESTS; ++i)
			if (asset_requests[i].in_use && Asset_StageNeedsActiveSet(asset_requests[i].stage))
				fullscan_active++;
		for (walk = asset_active_head; walk >= 0; walk = asset_requests[walk].active_next)
			set_active++;
		SDL_UnlockMutex(asset_mutex);
		if (fullscan_active != set_active)
			Con_Warning("asset_async active-set mismatch: fullscan=%u set=%u\n", fullscan_active, set_active);
	}
#endif

	if (asset_async_debug.value)
	{
		Con_DPrintf("asset_async: fs=%" SDL_PRIu64 "/%" SDL_PRIu64 " decode=%" SDL_PRIu64 "/%" SDL_PRIu64 " failed=%" SDL_PRIu64 " decode_backpressure=%" SDL_PRIu64 " decode_policy=%s inflight_raw=%zu inflight_decoded=%zu decode_ms[png]=%.3f decode_ms[ktx2]=%.3f pump_visited[last]=%u pump_visited_avg=%.2f\n",
			asset_completed_fs, asset_queued_fs, asset_completed_decode, asset_queued_decode, asset_failed_decode,
			asset_decode_submit_backpressure,
			asset_decode_submit_policy == ASSET_DECODE_SUBMIT_POLICY_BLOCKING ? "block" : "nonblock",
			asset_inflight_raw_bytes, asset_inflight_decoded_bytes,
			(double)asset_decode_us_png / 1000.0, (double)asset_decode_us_ktx2 / 1000.0,
			asset_pump_last_visited_slots,
			asset_pump_calls ? (double)asset_pump_visited_slots / (double)asset_pump_calls : 0.0);
	}
}

/*
 * Non-blocking query. Callers are expected to keep pumping async FS/decode work
 * from the frame loop via Asset_Async_Pump().
 */
qboolean Asset_Poll (asset_handle_t h, asset_result_t *out)
{
	unsigned idx = ASSET_HANDLE_INDEX(h), gen = ASSET_HANDLE_GEN(h);
	if (out)
		memset(out, 0, sizeof(*out));
	if (!h || idx >= ASSET_MAX_REQUESTS || !asset_initialized)
		return false;
	Asset_Async_Pump();
	SDL_LockMutex(asset_mutex);
	if (!asset_requests[idx].in_use || asset_requests[idx].generation != gen || (asset_requests[idx].stage != ASSET_STAGE_FINALIZE_PENDING && asset_requests[idx].stage != ASSET_STAGE_FAILED))
	{
		SDL_UnlockMutex(asset_mutex);
		return false;
	}
	if (out)
		*out = asset_requests[idx].result;
	SDL_UnlockMutex(asset_mutex);
	return true;
}

void Asset_Wait (asset_handle_t h, asset_result_t *out)
{
	unsigned idx = ASSET_HANDLE_INDEX(h), gen = ASSET_HANDLE_GEN(h);
	uint32_t waited_ms = 0;
	uint32_t next_warn_ms = ASSET_WAIT_WARN_THRESHOLD_MS;
	const qboolean can_pump = Asset_CanPumpCurrentThread();

	if (out)
		memset(out, 0, sizeof(*out));
	if (!h || idx >= ASSET_MAX_REQUESTS || !asset_initialized)
		return;

	SDL_LockMutex(asset_mutex);
	while (asset_requests[idx].in_use && asset_requests[idx].generation == gen && (asset_requests[idx].stage != ASSET_STAGE_FINALIZE_PENDING && asset_requests[idx].stage != ASSET_STAGE_FAILED))
	{
		if (SDL_CondWaitTimeout(asset_done_cond, asset_mutex, ASSET_WAIT_PUMP_INTERVAL_MS) == SDL_MUTEX_TIMEDOUT)
		{
			waited_ms += ASSET_WAIT_PUMP_INTERVAL_MS;
			if (waited_ms >= next_warn_ms)
			{
				Con_Warning("Asset_Wait: still waiting after %ums (handle=%u stage=%s path=%s%s)\n",
					waited_ms,
					h,
					Asset_StageName(asset_requests[idx].stage),
					asset_requests[idx].path,
					can_pump ? "" : ", non-main-thread");
				next_warn_ms += ASSET_WAIT_WARN_REPEAT_MS;
			}

			if (can_pump)
			{
				SDL_UnlockMutex(asset_mutex);
				Asset_Async_Pump();
				SDL_LockMutex(asset_mutex);
			}
		}
	}

	if (asset_requests[idx].in_use && asset_requests[idx].generation == gen && (asset_requests[idx].stage == ASSET_STAGE_FINALIZE_PENDING || asset_requests[idx].stage == ASSET_STAGE_FAILED) && out)
		*out = asset_requests[idx].result;
	SDL_UnlockMutex(asset_mutex);
}

void Asset_Release (asset_handle_t h)
{
	unsigned idx = ASSET_HANDLE_INDEX(h), gen = ASSET_HANDLE_GEN(h);
	if (!h || idx >= ASSET_MAX_REQUESTS || !asset_initialized)
		return;
	if (Asset_CanPumpCurrentThread())
		Asset_Async_Pump();
	Asset_Wait(h, NULL);
	SDL_LockMutex(asset_mutex);
	if (asset_requests[idx].in_use && asset_requests[idx].generation == gen)
	{
		if (asset_requests[idx].result.job)
		{
			size_t total = Asset_DecodeJobBytes(asset_requests[idx].result.job);
			if (asset_inflight_decoded_bytes >= total)
				asset_inflight_decoded_bytes -= total;
			else
				asset_inflight_decoded_bytes = 0;
			Asset_FreeDecodeJob(asset_requests[idx].result.job);
		}
		Asset_ResetRequestLocked(idx);
	}
	SDL_UnlockMutex(asset_mutex);
}
