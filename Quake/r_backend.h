#ifndef R_BACKEND_H
#define R_BACKEND_H

#include "render_api.h"

typedef struct render_backend_surface_info_s
{
	int surface_x;
	int surface_y;
	int surface_width;
	int surface_height;
	int view_x;
	int view_y;
	int view_width;
	int view_height;
	int scene_width;
	int scene_height;
	unsigned int scene_samples;
	unsigned int frame_index;
	qboolean needs_scene_effects;
	qboolean needs_postprocess;
} RenderBackendSurfaceInfo;

void R_Backend_QuerySurfaceInfo (RenderBackendSurfaceInfo *out_info);

#endif
