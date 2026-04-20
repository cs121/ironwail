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

typedef enum render_backend_runtime_status_e
{
	R_BACKEND_RUNTIME_IMPLEMENTED = 0,
	R_BACKEND_RUNTIME_EXPERIMENTAL,
	R_BACKEND_RUNTIME_STUB
} render_backend_runtime_status_t;

typedef struct render_backend_milestones_s
{
	qboolean init_ready;
	qboolean pass_callbacks_ready;
	qboolean present_ready;
	qboolean resource_translation_ready;
} RenderBackendMilestones;

render_backend_runtime_status_t R_Backend_GetRuntimeStatusForName (const char *backend_name);
const char *R_Backend_GetRuntimeStatusLabel (render_backend_runtime_status_t status);
qboolean R_Backend_GetMilestonesForName (const char *backend_name, RenderBackendMilestones *out_milestones);
void R_Backend_QuerySurfaceInfo (RenderBackendSurfaceInfo *out_info);

#endif
