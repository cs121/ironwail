#include "quakedef.h"

#define RW_RENDERER_EXPORTS
#include "rw_renderer.h"

#include <stdlib.h>
#include <string.h>

#include "../Quake/vid.h"
#include "../Quake/glquake.h"
#include "../Quake/draw.h"
#include "../Quake/screen.h"
#include "../Quake/render.h"
#include "../Quake/gl_texmgr.h"

struct rw_renderer_t {
    rw_renderer_version_t version;
    rw_renderer_backend_t backend;
    int width;
    int height;
    int frame_x;
    int frame_y;
    int frame_width;
    int frame_height;
    qboolean frame_active;
};

static const rw_renderer_version_t g_rw_renderer_version = {
    RW_RENDERER_VERSION_MAJOR,
    RW_RENDERER_VERSION_MINOR,
    RW_RENDERER_VERSION_PATCH
};

const rw_renderer_version_t *RW_CALL rw_renderer_get_version(void)
{
    return &g_rw_renderer_version;
}

rw_renderer_t *RW_CALL rw_renderer_create(void)
{
    rw_renderer_t *renderer = (rw_renderer_t *)malloc(sizeof(*renderer));
    if (!renderer)
        return NULL;

    memset(renderer, 0, sizeof(*renderer));
    renderer->version = g_rw_renderer_version;
    renderer->backend = RW_RENDERER_BACKEND_OPENGL;
    return renderer;
}

void RW_CALL rw_renderer_destroy(rw_renderer_t *renderer)
{
    if (!renderer)
        return;

    // TODO(dll): route to dynamically linked shutdown routine when available.
    VID_Shutdown();

    free(renderer);
}

rw_status_t RW_CALL rw_renderer_init(rw_renderer_t *renderer, const rw_renderer_config_t *config)
{
    if (!renderer || !config)
        return RW_ERR_INVALID_ARG;

    if (config->version.major != g_rw_renderer_version.major)
        return RW_ERR_UNSUPPORTED_VERSION;

    if (config->backend != RW_RENDERER_BACKEND_OPENGL && config->backend != RW_RENDERER_BACKEND_AUTO)
        return RW_ERR_UNSUPPORTED_BACKEND;

    renderer->version = config->version;
    renderer->backend = config->backend == RW_RENDERER_BACKEND_AUTO ? RW_RENDERER_BACKEND_OPENGL : config->backend;
    renderer->width = config->width;
    renderer->height = config->height;

#if defined(RW_RENDERER_LINK_DYNAMIC)
    // TODO(dll): call into dynamically loaded renderer entry points.
    (void)renderer;
    return RW_ERR_GENERIC;
#else
    // Preserve the existing initialization sequence from Host_Init.
    VID_Init();
    TexMgr_Init();
    Draw_Init();
    SCR_Init();
    R_Init();

    if (vid.width > 0 && vid.height > 0) {
        renderer->width = vid.width;
        renderer->height = vid.height;
    }

    return RW_OK;
#endif
}

rw_status_t RW_CALL rw_renderer_resize(rw_renderer_t *renderer, int width, int height)
{
    if (!renderer)
        return RW_ERR_INVALID_ARG;

    renderer->width = width;
    renderer->height = height;

#if defined(RW_RENDERER_LINK_DYNAMIC)
    // TODO(dll): forward resize request to dynamic renderer implementation.
    (void)width;
    (void)height;
#endif

    return RW_OK;
}

rw_status_t RW_CALL rw_renderer_begin_frame(rw_renderer_t *renderer)
{
    if (!renderer)
        return RW_ERR_INVALID_ARG;

#if defined(RW_RENDERER_LINK_DYNAMIC)
    // TODO(dll): acquire frame context from dynamic renderer.
    return RW_ERR_GENERIC;
#else
    GL_BeginRendering(&renderer->frame_x, &renderer->frame_y, &renderer->frame_width, &renderer->frame_height);
    renderer->frame_active = true;
    renderer->width = renderer->frame_width;
    renderer->height = renderer->frame_height;
    return RW_OK;
#endif
}

rw_status_t RW_CALL rw_renderer_end_frame(rw_renderer_t *renderer)
{
    if (!renderer)
        return RW_ERR_INVALID_ARG;

    if (!renderer->frame_active)
        return RW_OK;

#if defined(RW_RENDERER_LINK_DYNAMIC)
    // TODO(dll): release frame context for dynamic renderer.
#endif

    renderer->frame_active = false;
    return RW_OK;
}

rw_status_t RW_CALL rw_renderer_present(rw_renderer_t *renderer)
{
    if (!renderer)
        return RW_ERR_INVALID_ARG;

#if defined(RW_RENDERER_LINK_DYNAMIC)
    // TODO(dll): present frame using dynamically linked renderer.
    return RW_ERR_GENERIC;
#else
    GL_EndRendering();
    renderer->frame_active = false;
    return RW_OK;
#endif
}

rw_status_t RW_CALL rw_renderer_get_framebuffer_info(const rw_renderer_t *renderer, int *x, int *y, int *width, int *height)
{
    if (!renderer)
        return RW_ERR_INVALID_ARG;

    if (x)
        *x = renderer->frame_x;
    if (y)
        *y = renderer->frame_y;
    if (width)
        *width = renderer->frame_width;
    if (height)
        *height = renderer->frame_height;

    return renderer->frame_active ? RW_OK : RW_ERR_NOT_READY;
}

rw_status_t RW_CALL rw_renderer_create_texture(rw_renderer_t *renderer, void **handle_out)
{
    (void)renderer;
    (void)handle_out;
    // TODO(dll): provide texture creation stub when dynamic renderer is linked.
    return RW_ERR_GENERIC;
}

rw_status_t RW_CALL rw_renderer_destroy_texture(rw_renderer_t *renderer, void *handle)
{
    (void)renderer;
    (void)handle;
    // TODO(dll): provide texture destruction stub when dynamic renderer is linked.
    return RW_ERR_GENERIC;
}

rw_status_t RW_CALL rw_renderer_create_shader(rw_renderer_t *renderer, void **handle_out)
{
    (void)renderer;
    (void)handle_out;
    // TODO(dll): provide shader creation stub when dynamic renderer is linked.
    return RW_ERR_GENERIC;
}

rw_status_t RW_CALL rw_renderer_destroy_shader(rw_renderer_t *renderer, void *handle)
{
    (void)renderer;
    (void)handle;
    // TODO(dll): provide shader destruction stub when dynamic renderer is linked.
    return RW_ERR_GENERIC;
}

rw_status_t RW_CALL rw_renderer_create_buffer(rw_renderer_t *renderer, void **handle_out)
{
    (void)renderer;
    (void)handle_out;
    // TODO(dll): provide buffer creation stub when dynamic renderer is linked.
    return RW_ERR_GENERIC;
}

rw_status_t RW_CALL rw_renderer_destroy_buffer(rw_renderer_t *renderer, void *handle)
{
    (void)renderer;
    (void)handle;
    // TODO(dll): provide buffer destruction stub when dynamic renderer is linked.
    return RW_ERR_GENERIC;
}

