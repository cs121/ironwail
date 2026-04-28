#ifndef GL_BACKEND_H
#define GL_BACKEND_H

#include "render_api.h"

typedef void *(*gl_proc_address_loader_t)(const char *name);

typedef enum gl_backend_resource_key_e
{
	GL_BACKEND_RESOURCE_KEY_NONE = 0,
	GL_BACKEND_RESOURCE_KEY_GLOBAL_VAO
} gl_backend_resource_key_t;

void GL_Backend_SetProcAddressLoader (gl_proc_address_loader_t loader);
void *GL_Backend_GetProcAddress (const char *name);
void GL_Backend_ResetResources (void);
unsigned short GL_Backend_RegisterResource (render_backend_resource_type_t type, render_backend_resource_slot_t slot, render_backend_resource_lifetime_t lifetime, unsigned native_id);
unsigned short GL_Backend_RegisterNamedResource (render_backend_resource_type_t type, gl_backend_resource_key_t key, render_backend_resource_lifetime_t lifetime, unsigned native_id);
void GL_Backend_UnregisterResourceBySlot (render_backend_resource_slot_t slot);
void GL_Backend_UnregisterNamedResource (gl_backend_resource_key_t key);
unsigned GL_Backend_ResolveOpaqueResource (unsigned short opaque_id);
void GL_Backend_ResetStateCache (void);
void GL_Backend_SetViewportCached (int x, int y, int width, int height);
void GL_Backend_SetColorMaskCached (int r, int g, int b, int a);
void GL_Backend_SetDepthMaskCached (int enabled);
void GL_Backend_SetDepthFuncCached (unsigned func);
void GL_Backend_SetStencilTestCached (qboolean enabled);
void GL_Backend_SetStencilMaskCached (unsigned mask);
void GL_Backend_SetStencilFuncCached (unsigned func, int ref, unsigned mask);
void GL_Backend_SetStencilOpCached (unsigned sfail, unsigned dpfail, unsigned dppass);
const IRenderBackend *GL_Backend_GetInterface (void);

typedef struct ref_gl_stats_s
{
	int textures_created;
	int textures_destroyed;
	int textures_alive;
	int fbos_created;
	int fbos_destroyed;
	int fbos_alive;
	int buffers_created;
	int buffers_destroyed;
	int buffers_alive;
	int shaders_created;
	int shaders_destroyed;
	int programs_created;
	int programs_destroyed;
	int programs_alive;
	int gl_errors_detected;
	int frames_rendered;
} ref_gl_stats_t;

extern ref_gl_stats_t ref_gl_stats;

void REFGL_StatsLogSummary (void);

#endif
