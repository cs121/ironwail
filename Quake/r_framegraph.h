#ifndef R_FRAMEGRAPH_H
#define R_FRAMEGRAPH_H

#include "quakedef.h"
#include "r_ssao.h"

typedef struct render_frame_plan_s
{
	qboolean needs_scene_effects;
	qboolean needs_postprocess;
	qboolean run_shadowmaps;
	qboolean run_fogvol;
	qboolean run_postprocess;
	qboolean run_viewmodel;
	qboolean run_polyblend;
	qboolean run_store_prev;
} RenderFramePlan;

typedef enum render_graph_resource_bits_e
{
	RENDER_RES_NONE = 0,
	RENDER_RES_SCENE_COLOR = 1u << 0,
	RENDER_RES_SCENE_DEPTH = 1u << 1,
	RENDER_RES_COMPOSITE_COLOR = 1u << 2,
	RENDER_RES_SHADOW_SUN_DEPTH = 1u << 3,
	RENDER_RES_FOGVOL_HISTORY = 1u << 4,
	RENDER_RES_VELOCITY = 1u << 5
} render_graph_resource_bits_t;

typedef struct render_graph_resource_handle_s
{
	unsigned scene_fbo;
	unsigned composite_fbo;
	unsigned scene_depth_tex;
	unsigned shadow_sun_depth_tex;
	unsigned fogvol_history_tex;
	unsigned velocity_tex;
} RenderGraphResourceHandle;

typedef struct render_pass_context_s RenderPassContext;

typedef struct i_render_backend_s
{
	const char *name;
	void (*begin_pass)(const char *name);
	void (*end_pass)(void);
	void (*validate_pass_state)(const char *pass_name, qboolean before_pass);
	void (*begin_timer)(int pass_id);
	void (*end_timer)(int pass_id);
	void (*resolve_timers)(void);
} IRenderBackend;

typedef struct render_pass_desc_s
{
	const char *name;
	unsigned reads;
	unsigned writes;
	unsigned side_effects;
	qboolean (*enabled)(const RenderPassContext *ctx);
	void (*execute)(RenderPassContext *ctx);
} RenderPassDesc;

typedef struct r_framegraph_state_s
{
	int *fogvol_update_called;
	int *fogvol_draw_called;
	void (*prepare_fogvol_inputs)(void);
	qboolean *frame_rendered_this_update;
	r_ssao_fog_state_t *ssao_fog_state;
} r_framegraph_state_t;

typedef struct render_pass_context_s
{
	const r_framegraph_state_t *legacy_state;
	const RenderFramePlan *frame_plan;
	const RenderGraphResourceHandle *resources;
	const IRenderBackend *backend;
} RenderPassContext;

void R_FrameGraph_BuildRenderFramePlan (RenderFramePlan *out_plan);
void R_FrameGraph_SetRenderFramePlan (const RenderFramePlan *plan);
qboolean R_FrameGraph_GetRenderFramePlan (RenderFramePlan *out_plan);
void R_FrameGraph_RenderView (const r_framegraph_state_t *state);

#endif
