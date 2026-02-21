#ifndef _ASSET_DECODE_ASYNC_H_
#define _ASSET_DECODE_ASYNC_H_

#include "q_stdinc.h"
#include "fs_async.h"

typedef struct qmodel_s qmodel_t;

typedef uint32_t asset_handle_t;

typedef enum
{
	ASSET_IMAGE_FMT_RGBA8 = 0
} asset_image_format_t;

typedef struct
{
	void *data;
	size_t size;
	size_t pitch;
} asset_mip_blob_t;

typedef struct
{
	int width;
	int height;
	int mip_count;
	asset_image_format_t format;
	qboolean srgb;
	qboolean has_alpha;
	asset_mip_blob_t mips[32];
} asset_image_payload_t;

typedef struct
{
	const char *name;
	unsigned flags;
	qmodel_t *owner;
} asset_tex_params_t;

typedef enum
{
	ASSET_RESULT_PENDING = 0,
	ASSET_RESULT_OK,
	ASSET_RESULT_FAILED,
	ASSET_RESULT_NOT_FOUND
} asset_result_status_t;

typedef struct
{
	asset_result_status_t status;
	asset_image_payload_t *image;
} asset_result_t;

void Asset_Async_Init (void);
void Asset_Async_Shutdown (void);
asset_handle_t Asset_LoadTextureAsync (const char *path, const asset_tex_params_t *params);
qboolean Asset_Poll (asset_handle_t h, asset_result_t *out);
void Asset_Wait (asset_handle_t h, asset_result_t *out);
void Asset_Release (asset_handle_t h);
void Asset_Async_Pump (void);

#endif
