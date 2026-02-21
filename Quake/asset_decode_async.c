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

static cvar_t asset_async = {"asset_async", "0", CVAR_ARCHIVE};
static cvar_t asset_async_max_inflight_mb = {"asset_async_max_inflight_mb", "256", CVAR_ARCHIVE};
static cvar_t asset_async_debug = {"asset_async_debug", "0", CVAR_NONE};
static cvar_t asset_async_decode_workers = {"asset_async_decode_workers", "1", CVAR_ARCHIVE};

static uint64_t asset_queued_fs;
static uint64_t asset_completed_fs;
static uint64_t asset_queued_decode;
static uint64_t asset_completed_decode;
static uint64_t asset_failed_decode;
static uint64_t asset_decode_us_png;
static uint64_t asset_decode_us_ktx2;
static size_t asset_inflight_raw_bytes;
static size_t asset_inflight_decoded_bytes;

static qboolean Asset_IsKTX2(const void *data, size_t size)
{
	static const uint8_t magic[12] = {0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
	return size >= sizeof(magic) && memcmp(data, magic, sizeof(magic)) == 0;
}


static asset_image_payload_t *Asset_DecodePNG(const void *raw_data, size_t raw_size)
{
	asset_image_payload_t *img;
	unsigned char *rgba = NULL;
	unsigned w = 0, h = 0;
	unsigned err;

	err = lodepng_decode32(&rgba, &w, &h, (const unsigned char *)raw_data, raw_size);
	if (err || !rgba)
		return NULL;

	img = (asset_image_payload_t *)calloc(1, sizeof(*img));
	if (!img)
	{
		free(rgba);
		return NULL;
	}

	img->width = (int)w;
	img->height = (int)h;
	img->mip_count = 1;
	img->format = ASSET_IMAGE_FMT_RGBA8;
	img->has_alpha = true;
	img->mips[0].data = rgba;
	img->mips[0].size = (size_t)w * (size_t)h * 4u;
	img->mips[0].pitch = (size_t)w * 4u;
	return img;
}

static asset_image_payload_t *Asset_DecodeKTX2(const void *raw_data, size_t raw_size)
{
	ktx2_header_t hdr;
	ktx2_decoded_image_t decoded;
	asset_image_payload_t *img;
	int i;

	memset(&decoded, 0, sizeof(decoded));
	if (!KTX2_ParseHeaderPublic(&hdr, (const uint8_t *)raw_data, raw_size))
		return NULL;
	if (!KTX2_TranscodeToRGBA((const uint8_t *)raw_data, raw_size, &hdr, &decoded))
		return NULL;

	img = (asset_image_payload_t *)calloc(1, sizeof(*img));
	if (!img)
	{
		KTX2_FreeDecodedImage(&decoded);
		return NULL;
	}

	img->width = decoded.width[0];
	img->height = decoded.height[0];
	img->mip_count = decoded.mip_count;
	img->format = ASSET_IMAGE_FMT_RGBA8;
	img->has_alpha = true;

	for (i = 0; i < decoded.mip_count; ++i)
	{
		img->mips[i].data = decoded.mip_data[i];
		img->mips[i].size = decoded.mip_size[i];
		img->mips[i].pitch = (size_t)decoded.width[i] * 4u;
		decoded.mip_data[i] = NULL;
		decoded.mip_size[i] = 0;
	}

	KTX2_FreeDecodedImage(&decoded);
	return img;
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
	asset_image_payload_t *img = NULL;

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
		img = Asset_DecodeKTX2(raw_data, raw_size);
	else
		img = Asset_DecodePNG(raw_data, raw_size);

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
		if (img)
		{
			asset_requests[idx].result.status = ASSET_RESULT_OK;
			asset_requests[idx].result.image = img;
			asset_requests[idx].stage = ASSET_STAGE_FINALIZE_PENDING;
			{ size_t total = 0; for (int m = 0; m < img->mip_count; ++m) total += img->mips[m].size; asset_inflight_decoded_bytes += total; }
		}
		else
		{
			asset_failed_decode++;
			asset_requests[idx].result.status = ASSET_RESULT_FAILED;
			q_snprintf(asset_requests[idx].result.error, sizeof(asset_requests[idx].result.error), "decode failed");
			asset_requests[idx].stage = ASSET_STAGE_FAILED;
		}
		if (asset_async_debug.value)
		{
			Con_DPrintf("asset_async_done: %s kind=%d status=%d io_ms=%.3f decode_ms=%.3f\n",
				asset_requests[idx].path, (int)asset_requests[idx].result.kind, (int)asset_requests[idx].result.status,
				(double)asset_requests[idx].result.io_us / 1000.0, (double)asset_requests[idx].result.decode_us / 1000.0);
		}
		SDL_CondBroadcast(asset_done_cond);
	}
	else if (img)
	{
		for (int i = 0; i < img->mip_count; ++i)
			free(img->mips[i].data);
		free(img);
	}
	SDL_UnlockMutex(asset_mutex);
}

void Asset_Async_Init (void)
{
	if (asset_initialized)
		return;
	asset_mutex = SDL_CreateMutex();
	asset_done_cond = SDL_CreateCond();
	asset_decode_queue = Sys_Jobs_CreateQueue("Asset Decode", 256, (size_t)q_max(1.0f, asset_async_decode_workers.value));
	if (!asset_mutex || !asset_done_cond || !asset_decode_queue)
		Sys_Error("Asset_Async_Init failed");
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
		if (asset_requests[i].result.image)
		{
			for (int j = 0; j < asset_requests[i].result.image->mip_count; ++j)
				free(asset_requests[i].result.image->mips[j].data);
			free(asset_requests[i].result.image);
		}
		memset(&asset_requests[i], 0, sizeof(asset_requests[i]));
	}
	SDL_UnlockMutex(asset_mutex);
	SDL_DestroyCond(asset_done_cond);
	SDL_DestroyMutex(asset_mutex);
	asset_done_cond = NULL;
	asset_mutex = NULL;
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
		asset_requests[i].stage = ASSET_STAGE_IO_PENDING;
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
			memset(&asset_requests[idx], 0, sizeof(asset_requests[idx]));
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
	unsigned i;
	if (!asset_initialized)
		return;

	for (i = 0; i < ASSET_MAX_REQUESTS; ++i)
	{
		asset_handle_t h = 0;
		fs_async_handle_t fs_handle = 0;
		fs_async_result_t fs;
		qboolean has_fs;
		qboolean submit_decode = false;

		SDL_LockMutex(asset_mutex);
		if (!asset_requests[i].in_use)
		{
			SDL_UnlockMutex(asset_mutex);
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
			if (Sys_Jobs_TrySubmit(asset_decode_queue, Asset_DecodeWorker, (void *)(uintptr_t)h))
			{
				SDL_LockMutex(asset_mutex);
				asset_queued_decode++;
				SDL_UnlockMutex(asset_mutex);
			}
			else
			{
				SDL_LockMutex(asset_mutex);
				if (asset_requests[i].in_use && asset_requests[i].generation == ASSET_HANDLE_GEN(h) && asset_requests[i].stage == ASSET_STAGE_DECODE_PENDING)
					asset_requests[i].decode_queued = false;
				SDL_UnlockMutex(asset_mutex);
			}
			continue;
		}

		if (!fs_handle)
			continue;

		has_fs = FS_Poll(fs_handle, &fs);
		if (!has_fs)
			continue;

		SDL_LockMutex(asset_mutex);
		if (!asset_requests[i].in_use || asset_requests[i].generation != ASSET_HANDLE_GEN(h) || asset_requests[i].stage != ASSET_STAGE_IO_PENDING)
		{
			SDL_UnlockMutex(asset_mutex);
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
				asset_requests[i].stage = ASSET_STAGE_DECODE_PENDING;
				asset_requests[i].kind = Asset_IsKTX2(fs.data, fs.size) ? ASSET_KIND_KTX2 : ASSET_KIND_PNG;
				asset_requests[i].result.kind = asset_requests[i].kind;
				asset_requests[i].result.io_us = (uint64_t)((SDL_GetPerformanceCounter() - asset_requests[i].t_io_start_us) * 1000000ull / SDL_GetPerformanceFrequency());
			}
			else
			{
				asset_requests[i].result.status = ASSET_RESULT_FAILED;
				asset_requests[i].result.error_code = ENOMEM;
				q_strlcpy(asset_requests[i].result.error, "malloc failed", sizeof(asset_requests[i].result.error));
				asset_requests[i].stage = ASSET_STAGE_FAILED;
				SDL_CondBroadcast(asset_done_cond);
			}
		}
		else
		{
			asset_requests[i].result.status = (fs.status == FS_ASYNC_STATUS_NOT_FOUND) ? ASSET_RESULT_NOT_FOUND : ASSET_RESULT_FAILED;
			asset_requests[i].result.error_code = fs.error_code;
			q_snprintf(asset_requests[i].result.error, sizeof(asset_requests[i].result.error), "fs status=%d", (int)fs.status);
			asset_requests[i].stage = ASSET_STAGE_FAILED;
			SDL_CondBroadcast(asset_done_cond);
		}
		FS_Release(asset_requests[i].fs_handle);
		asset_requests[i].fs_handle = 0;
		SDL_UnlockMutex(asset_mutex);
	}

	if (asset_async_debug.value)
	{
		Con_DPrintf("asset_async: fs=%" SDL_PRIu64 "/%" SDL_PRIu64 " decode=%" SDL_PRIu64 "/%" SDL_PRIu64 " failed=%" SDL_PRIu64 " inflight_raw=%zu inflight_decoded=%zu decode_ms[png]=%.3f decode_ms[ktx2]=%.3f\n",
			asset_completed_fs, asset_queued_fs, asset_completed_decode, asset_queued_decode, asset_failed_decode,
			asset_inflight_raw_bytes, asset_inflight_decoded_bytes,
			(double)asset_decode_us_png / 1000.0, (double)asset_decode_us_ktx2 / 1000.0);
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

	if (out)
		memset(out, 0, sizeof(*out));
	if (!h || idx >= ASSET_MAX_REQUESTS || !asset_initialized)
		return;

	/*
	 * Blocking wait for completion only; this does not poll filesystem progress.
	 * The caller (usually the frame loop) must continue calling Asset_Async_Pump().
	 */
	SDL_LockMutex(asset_mutex);
	while (asset_requests[idx].in_use && asset_requests[idx].generation == gen && (asset_requests[idx].stage != ASSET_STAGE_FINALIZE_PENDING && asset_requests[idx].stage != ASSET_STAGE_FAILED))
		SDL_CondWait(asset_done_cond, asset_mutex);

	if (asset_requests[idx].in_use && asset_requests[idx].generation == gen && (asset_requests[idx].stage == ASSET_STAGE_FINALIZE_PENDING || asset_requests[idx].stage == ASSET_STAGE_FAILED) && out)
		*out = asset_requests[idx].result;
	SDL_UnlockMutex(asset_mutex);
}

void Asset_Release (asset_handle_t h)
{
	unsigned idx = ASSET_HANDLE_INDEX(h), gen = ASSET_HANDLE_GEN(h);
	if (!h || idx >= ASSET_MAX_REQUESTS || !asset_initialized)
		return;
	Asset_Wait(h, NULL);
	SDL_LockMutex(asset_mutex);
	if (asset_requests[idx].in_use && asset_requests[idx].generation == gen)
	{
		if (asset_requests[idx].result.image)
		{
			for (int i = 0; i < asset_requests[idx].result.image->mip_count; ++i)
				free(asset_requests[idx].result.image->mips[i].data);
			{ size_t total = 0; for (int m = 0; m < asset_requests[idx].result.image->mip_count; ++m) total += asset_requests[idx].result.image->mips[m].size; if (asset_inflight_decoded_bytes >= total) asset_inflight_decoded_bytes -= total; else asset_inflight_decoded_bytes = 0; }
			free(asset_requests[idx].result.image);
		}
		memset(&asset_requests[idx], 0, sizeof(asset_requests[idx]));
	}
	SDL_UnlockMutex(asset_mutex);
}
