#ifndef _SYS_JOBS_H_
#define _SYS_JOBS_H_

#include "q_stdinc.h"

typedef struct sys_job_queue_s sys_job_queue_t;
typedef void (*sys_job_execute_fn)(void *job_data);

// Worker-safe work only: file IO/decompression/hash/allocations. No renderer/GL/global state mutation.
sys_job_queue_t *Sys_Jobs_CreateQueue (const char *name, size_t capacity);
void Sys_Jobs_DestroyQueue (sys_job_queue_t *queue);
qboolean Sys_Jobs_Submit (sys_job_queue_t *queue, sys_job_execute_fn execute, void *job_data);
qboolean Sys_Jobs_TrySubmit (sys_job_queue_t *queue, sys_job_execute_fn execute, void *job_data);

#endif
