#include "renderer/rw_renderer.h"

static rw_renderer_t *s_active_renderer = NULL;

RW_API void RW_CALL rw_renderer_set_active(rw_renderer_t *renderer)
{
    s_active_renderer = renderer;
    // TODO(dll): replace with DLL binding logic when renderer is externalized.
}

RW_API rw_renderer_t *RW_CALL rw_renderer_get_active(void)
{
    return s_active_renderer;
}

RW_API rw_status_t RW_CALL rw_renderer_begin_frame_active(int *x, int *y, int *width, int *height)
{
    if (!s_active_renderer)
        return RW_ERR_NOT_READY;

    rw_status_t status = rw_renderer_begin_frame(s_active_renderer);
    if (status != RW_OK)
        return status;

    rw_renderer_get_framebuffer_info(s_active_renderer, x, y, width, height);
    return status;
}

RW_API rw_status_t RW_CALL rw_renderer_end_frame_active(void)
{
    if (!s_active_renderer)
        return RW_ERR_NOT_READY;

    return rw_renderer_end_frame(s_active_renderer);
}

RW_API rw_status_t RW_CALL rw_renderer_present_active(void)
{
    if (!s_active_renderer)
        return RW_ERR_NOT_READY;

    return rw_renderer_present(s_active_renderer);
}

