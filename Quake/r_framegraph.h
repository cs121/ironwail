#ifndef R_FRAMEGRAPH_H
#define R_FRAMEGRAPH_H

#include "quakedef.h"
#include "r_backend.h"

typedef struct render_frame_plan_s
{
	qboolean needs_scene_effects;
	qboolean needs_postprocess;
	qboolean run_shadowmaps;
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
	RENDER_RES_VELOCITY = 1u << 4,
	RENDER_RES_DECALS = 1u << 5,
	RENDER_RES_SSAO_FOG_STATE = 1u << 6
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
	render_backend_resource_ref_t refs[R_BACKEND_RESOURCE_SLOT_COUNT];
	int scene_samples;
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
	qboolean (*consume_timer_sample)(int pass_id, double *out_gpu_ms);
	unsigned (*resolve_resource_id)(const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource);
	qboolean (*is_resource_valid)(const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource);
	void (*bind_render_target)(const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer);
	void (*set_viewport)(int x, int y, int width, int height);
	void (*set_scissor)(qboolean enabled, int x, int y, int width, int height);
	void (*set_pipeline_state)(unsigned state_bits);
	void (*draw)(render_backend_primitive_t primitive, int first, int count);
	void (*dispatch)(unsigned group_x, unsigned group_y, unsigned group_z);
	void (*set_blend_factors)(render_blend_factor_t src, render_blend_factor_t dst);
	void (*finish)(void);
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
void R_FrameGraph_GetTimingSummary (double *out_gpu_ms, double *out_cpu_ms, qboolean *out_gpu_valid);
void R_FrameGraph_ResetPasses (void);
qboolean R_FrameGraph_AddPass (const RenderPassDesc *pass_desc);
void R_FrameGraph_RenderView (void);
void R_Backend_Init (void);
void R_Backend_Register (const IRenderBackend *backend);
qboolean R_Backend_Select (const char *backend_name);
const IRenderBackend *R_GetRenderBackend (void);
const render_backend_resource_ref_t *R_FrameGraph_GetResourceRef (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot);
unsigned R_FrameGraph_ResolveResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot);
qboolean R_FrameGraph_HasResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot);
void R_Backend_SetViewport (int x, int y, int width, int height);
void R_Backend_SetScissor (qboolean enabled, int x, int y, int width, int height);
void R_Backend_SetPipelineState (unsigned state_bits);
void R_Backend_Draw (render_backend_primitive_t primitive, int first, int count);
void R_Backend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z);
void R_Backend_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst);
void R_Backend_Finish (void);

#endif
