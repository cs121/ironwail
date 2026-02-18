#ifndef _FS_ASYNC_H_
#define _FS_ASYNC_H_

#include "q_stdinc.h"

typedef uint32_t fs_async_handle_t;

enum
{
	FS_ASYNC_FLAG_NONE = 0,
	FS_ASYNC_FLAG_ALLOW_DECOMPRESS = (1 << 0)
};

typedef enum
{
	FS_ASYNC_STATUS_PENDING = 0,
	FS_ASYNC_STATUS_OK,
	FS_ASYNC_STATUS_NOT_FOUND,
	FS_ASYNC_STATUS_IO_ERROR,
	FS_ASYNC_STATUS_CANCELLED,
	FS_ASYNC_STATUS_BAD_HANDLE
} fs_async_status_t;

typedef struct
{
	void *data;
	size_t size;
	fs_async_status_t status;
	int error_code;
} fs_async_result_t;

void FS_Async_Init (void);
void FS_Async_Shutdown (void);

fs_async_handle_t FS_ReadFileAsync (const char *path, int flags);
qboolean FS_Poll (fs_async_handle_t h, fs_async_result_t *out);
void FS_Wait (fs_async_handle_t h, fs_async_result_t *out);
void FS_Release (fs_async_handle_t h);
void FS_Async_Pump (void);

#endif
