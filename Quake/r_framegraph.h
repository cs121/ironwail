#ifndef R_FRAMEGRAPH_H
#define R_FRAMEGRAPH_H

#include "quakedef.h"

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
	RENDER_RES_VELOCITY = 1u << 5,
	RENDER_RES_DECALS = 1u << 6,
	RENDER_RES_FOGVOL_INPUTS = 1u << 7,
	RENDER_RES_SSAO_FOG_STATE = 1u << 8
} render_graph_resource_bits_t;

typedef enum fg_pass_output_target_e
{
	FG_PASS_OUTPUT_KEEP = 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_OUTPUT_SCENE_FBO,
	FG_PASS_OUTPUT_COMPOSITE_FBO,
	FG_PASS_OUTPUT_AUTO_SCENE,
	FG_PASS_OUTPUT_AUTO_WARP
} fg_pass_output_target_t;

typedef enum fg_pass_viewport_mode_e
{
	FG_PASS_VIEWPORT_KEEP = 0,
	FG_PASS_VIEWPORT_FULL_WINDOW,
	FG_PASS_VIEWPORT_VIEW_RECT,
	FG_PASS_VIEWPORT_VIEW_RECT_SCALED
} fg_pass_viewport_mode_t;

typedef enum fg_pass_stage_e
{
	FG_PASS_STAGE_MAIN = 0,
	FG_PASS_STAGE_SETUP
} fg_pass_stage_t;

typedef enum fg_pass_stats_channel_e
{
	FG_PASS_STATS_NONE = 0,
	FG_PASS_STATS_SETUP,
	FG_PASS_STATS_SHADOW,
	FG_PASS_STATS_SCENE,
	FG_PASS_STATS_WARP,
	FG_PASS_STATS_FOG,
	FG_PASS_STATS_POST,
	FG_PASS_STATS_OVERLAY,
	FG_PASS_STATS_COUNT
} fg_pass_stats_channel_t;

typedef struct render_graph_resource_handle_s
{
	unsigned scene_fbo;
	unsigned scene_color_tex;
	unsigned scene_velocity_tex;
	unsigned scene_depth_tex;
	int scene_samples;
	unsigned resolved_scene_fbo;
	unsigned resolved_scene_color_tex;
	unsigned resolved_scene_velocity_tex;
	unsigned composite_fbo;
	unsigned composite_color_tex;
	unsigned composite_depth_tex;
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
	unsigned char output_target;
	unsigned char viewport_mode;
	qboolean (*enabled)(const RenderPassContext *ctx);
	void (*execute)(RenderPassContext *ctx);
	unsigned char stage;
	unsigned char stats_channel;
} RenderPassDesc;

typedef struct render_pass_context_s
{
	const RenderFramePlan *frame_plan;
	const RenderGraphResourceHandle *resources;
	const IRenderBackend *backend;
} RenderPassContext;

void R_FrameGraph_BuildRenderFramePlan (RenderFramePlan *out_plan);
void R_FrameGraph_SetRenderFramePlan (const RenderFramePlan *plan);
qboolean R_FrameGraph_GetRenderFramePlan (RenderFramePlan *out_plan);
void R_FrameGraph_ResetPasses (void);
qboolean R_FrameGraph_AddPass (const RenderPassDesc *pass_desc);
void R_FrameGraph_RenderView (void);

#endif
