#ifndef GL_BACKEND_H
#define GL_BACKEND_H

#include "render_api.h"

typedef void *(*gl_proc_address_loader_t)(const char *name);

void GL_Backend_SetProcAddressLoader (gl_proc_address_loader_t loader);
void *GL_Backend_GetProcAddress (const char *name);
const IRenderBackend *GL_Backend_GetInterface (void);
void GL_Backend_Register (void);

#endif
