#ifndef GL_BACKEND_H
#define GL_BACKEND_H

typedef void *(*gl_proc_address_loader_t)(const char *name);

void GL_Backend_SetProcAddressLoader (gl_proc_address_loader_t loader);
void *GL_Backend_GetProcAddress (const char *name);
void GL_Backend_Register (void);

#endif
