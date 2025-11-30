#include "draw_surface.h"

#define MAX_DRAW_SURFACES 8192

render_backend_t *rb;

static draw_surf_t draw_surfaces[MAX_DRAW_SURFACES];
static size_t draw_surface_count = 0;

void R_SubmitDrawSurface(const draw_surf_t *surf)
{
    if (!surf)
        return;

    if (draw_surface_count == MAX_DRAW_SURFACES)
        R_FlushDrawSurfaces();

    draw_surfaces[draw_surface_count++] = *surf;
}

void R_FlushDrawSurfaces(void)
{
    if (!rb || !rb->DrawSurface)
    {
        draw_surface_count = 0;
        return;
    }

    for (size_t i = 0; i < draw_surface_count; ++i)
        rb->DrawSurface(&draw_surfaces[i]);

    draw_surface_count = 0;
}

