#ifndef R_SHADOWS_H
#define R_SHADOWS_H

#include "quakedef.h"

#ifndef R_SHADOWMAPS
#define R_SHADOWMAPS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(GLQUAKE_VERSION)
typedef struct shadow_gl_backend_s
{
    /* Placeholder for GL shadow resources. */
    int unused;
} shadow_gl_backend_t;
#endif

#if defined(VKQUAKE)
typedef struct shadow_vk_backend_s
{
    /* Placeholder for VK shadow resources. */
    int unused;
} shadow_vk_backend_t;
#endif

typedef struct shadow_context_s
{
    int size;
    int atlas_cols;
    int atlas_rows;
#if defined(GLQUAKE_VERSION)
    /* TODO(backlog): add OpenGL handles for depth atlas and pipelines. */
    shadow_gl_backend_t *gl_backend_data;
#endif
#if defined(VKQUAKE)
    /* TODO(backlog): add Vulkan handles for depth atlas and pipelines. */
    shadow_vk_backend_t *vk_backend_data;
#endif
    void *backend_data;
} shadow_context_t;

#if R_SHADOWMAPS
extern shadow_context_t r_shadow_context;
extern cvar_t r_shadows;
extern cvar_t r_shadows_debug;
extern cvar_t r_shadows_size;
#endif

void R_InitShadowMaps(void);
void R_ShutdownShadowMaps(void);
void R_BeginShadowPass(void);
void R_EndShadowPass(void);
void R_UploadShadowMatrices(const float *viewProj, int lightIndex);
void R_DrawShadowDebug(void);

#ifdef __cplusplus
}
#endif

#endif /* R_SHADOWS_H */
