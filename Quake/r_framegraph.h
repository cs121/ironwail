#ifndef R_FRAMEGRAPH_H
#define R_FRAMEGRAPH_H

#include "quakedef.h"
#include "render_api.h"

typedef struct render_frame_plan_s
{
	qboolean needs_scene_effects;
	qboolean needs_postprocess;
	qboolean run_shadowmaps;
	qboolean run_postprocess;
	qboolean run_viewmodel;
	qboolean run_polyblend;
	qboolean run_store_prev;
	qboolean run_gpu_timers;
} RenderFramePlan;

typedef enum render_graph_resource_bits_e
{
	RENDER_RES_NONE = 0,
	RENDER_RES_SCENE_COLOR = 1u << 0,
	RENDER_RES_SCENE_DEPTH = 1u << 1,
	RENDER_RES_COMPOSITE_COLOR = 1u << 2,
	RENDER_RES_COMPOSITE_DEPTH = 1u << 3,
	RENDER_RES_SHADOW_SUN_DEPTH = 1u << 4,
	RENDER_RES_VELOCITY = 1u << 5,
	RENDER_RES_DECALS = 1u << 6,
	RENDER_RES_SSAO_FOG_STATE = 1u << 7
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

typedef enum fg_pass_baseline_bits_e
{
	FG_PASS_BASELINE_RESET_SCISSOR = 1u << 0,
	FG_PASS_BASELINE_REQUIRE_AUTOBIND = 1u << 1
} fg_pass_baseline_bits_t;

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

typedef struct fg_pass_attachment_config_s
{
	unsigned resource_bit;
	render_backend_load_op_t load_op;
	render_backend_store_op_t store_op;
} FGPassAttachmentConfig;

typedef struct render_pass_desc_s
{
	const char *name;
	unsigned reads;
	unsigned writes;
	unsigned side_effects;
	unsigned baseline_bits;
	unsigned char output_target;
	unsigned char viewport_mode;
	qboolean (*enabled)(const RenderPassContext *ctx);
	void (*execute)(RenderPassContext *ctx);
	const FGPassAttachmentConfig *color_attachments;
	unsigned char num_color_attachments;
	const FGPassAttachmentConfig *depth_attachment;
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
void R_Backend_Shutdown (void);
void R_Backend_Register (const IRenderBackend *backend);
qboolean R_Backend_Select (const char *backend_name);
void R_Backend_OnResize (int width, int height);
const IRenderBackend *R_GetRenderBackend (void);
const RenderBackendCaps *R_Backend_GetCaps (void);
void R_Backend_BeginFrame (void);
void R_Backend_EndFrame (void);
void R_Backend_Present (void);
void R_Backend_BeginPassEx (const RenderBackendPassDesc *pass_desc);
void R_Backend_EndPassEx (void);
void R_Backend_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count);
void R_Backend_BindPipeline (const RenderBackendPipelineDesc *pipeline);
void R_Backend_SetDynamicState (const RenderBackendDynamicState *dynamic_state);
void R_Backend_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count);
void R_Backend_BeginCommandEncoder (const RenderBackendCommandEncoderDesc *desc);
void R_Backend_EndCommandEncoder (void);
void R_Backend_SubmitCommandEncoder (void);
const render_backend_resource_ref_t *R_FrameGraph_GetResourceRef (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot);
unsigned R_FrameGraph_ResolveResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot);
unsigned R_FrameGraph_ResolveRequiredResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot, const char *usage_tag);
qboolean R_FrameGraph_HasResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot);
void R_Backend_SetViewport (int x, int y, int width, int height);
void R_Backend_SetScissor (qboolean enabled, int x, int y, int width, int height);
void R_Backend_SetPipelineState (unsigned state_bits);
void R_Backend_Draw (render_backend_primitive_t primitive, int first, int count);
void R_Backend_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes);
void R_Backend_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count);
void R_Backend_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count);
void R_Backend_DrawPacket (const RenderBackendDrawPacket *packet);
void R_Backend_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes);
void R_Backend_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes);
void R_Backend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z);
void R_Backend_MemoryBarrier (unsigned barrier_bits);
void R_Backend_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst);
void R_Backend_SetDepthFunc (render_backend_depth_func_t depth_func);
unsigned R_Backend_CreatePostFXLUTTexture (void);
void R_Backend_ConfigurePostFXLUTTexture (unsigned texture_id);
void R_Backend_Finish (void);
void R_Backend_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles);
int R_Backend_GetSceneSampleCount (void);

#endif
