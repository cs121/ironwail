#ifndef RENDER_DISPATCH_H
#define RENDER_DISPATCH_H

#include "renderer_host_bridge.h"

extern const iw_renderer_entry_points_t *g_rend;

void RenderDispatch_Init (void);
void RenderDispatch_SetEntryPoints (const iw_renderer_entry_points_t *entry_points);
void RenderDispatch_UpdateScreen (void);

#endif
