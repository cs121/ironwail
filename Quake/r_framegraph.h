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
	qboolean run_gpu_timers;
} RenderFramePlan;

typedef struct render_backend_caps_s
{
	qboolean supports_timestamps;
	qboolean supports_compute;
	qboolean supports_draw_instanced;
	qboolean supports_draw_indirect;
	qboolean supports_multi_draw_indirect;
	qboolean supports_memory_barrier;
	qboolean supports_legacy_pass_fallbacks;
	unsigned msaa_mode_mask;
	unsigned max_msaa_samples;
	unsigned shader_model;
	qboolean supports_bindless;
	unsigned max_textures;
	unsigned max_samplers;
	unsigned max_ubos;
	unsigned max_ssbos;
} RenderBackendCaps;

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

typedef enum render_backend_load_op_e
{
	R_BACKEND_LOAD_OP_LOAD = 0,
	R_BACKEND_LOAD_OP_CLEAR,
	R_BACKEND_LOAD_OP_DONT_CARE
} render_backend_load_op_t;

typedef enum render_backend_store_op_e
{
	R_BACKEND_STORE_OP_STORE = 0,
	R_BACKEND_STORE_OP_DONT_CARE
} render_backend_store_op_t;

typedef enum render_backend_resource_state_e
{
	R_BACKEND_RESOURCE_STATE_UNKNOWN = 0,
	R_BACKEND_RESOURCE_STATE_COLOR_ATTACHMENT,
	R_BACKEND_RESOURCE_STATE_DEPTH_ATTACHMENT,
	R_BACKEND_RESOURCE_STATE_SHADER_READ,
	R_BACKEND_RESOURCE_STATE_SHADER_WRITE,
	R_BACKEND_RESOURCE_STATE_PRESENT
} render_backend_resource_state_t;

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
	unsigned short slot_resource_ids[R_BACKEND_RESOURCE_SLOT_COUNT];
	struct render_graph_backend_resource_entry_s
	{
		unsigned resource_id;
		unsigned native_id;
		unsigned char type;
		unsigned char lifetime;
		unsigned short slot;
	} registry[32];
	unsigned char registry_count;
} RenderGraphResourceHandle;

typedef enum render_backend_resource_lifetime_e
{
	R_BACKEND_RESOURCE_LIFETIME_FRAME = 0,
	R_BACKEND_RESOURCE_LIFETIME_PERSISTENT
} render_backend_resource_lifetime_t;

typedef struct render_backend_pass_attachment_desc_s
{
	const render_backend_resource_ref_t *resource;
	render_backend_load_op_t load_op;
	render_backend_store_op_t store_op;
} RenderBackendPassAttachmentDesc;

typedef struct render_backend_pass_desc_s
{
	const char *name;
	const RenderGraphResourceHandle *resources;
	const RenderBackendPassAttachmentDesc *color_attachments;
	unsigned num_color_attachments;
	const RenderBackendPassAttachmentDesc *depth_attachment;
	qboolean backbuffer;
} RenderBackendPassDesc;

typedef struct render_backend_resource_barrier_s
{
	const render_backend_resource_ref_t *resource;
	render_backend_resource_state_t before;
	render_backend_resource_state_t after;
} RenderBackendResourceBarrier;

typedef struct render_backend_pipeline_desc_s
{
	unsigned pipeline_id;
	unsigned state_bits;
} RenderBackendPipelineDesc;

typedef struct render_backend_dynamic_state_s
{
	unsigned blend_state;
	unsigned depth_state;
	unsigned raster_state;
} RenderBackendDynamicState;

typedef enum render_backend_descriptor_type_e
{
	R_BACKEND_DESCRIPTOR_TEXTURE = 0,
	R_BACKEND_DESCRIPTOR_SAMPLER,
	R_BACKEND_DESCRIPTOR_UNIFORM_BUFFER,
	R_BACKEND_DESCRIPTOR_STORAGE_BUFFER
} render_backend_descriptor_type_t;

typedef struct render_backend_descriptor_binding_s
{
	render_backend_descriptor_type_t type;
	unsigned slot;
	unsigned resource_id;
	unsigned offset;
	unsigned range;
} RenderBackendDescriptorBinding;

typedef struct fg_pass_attachment_config_s
{
	unsigned resource_bit;
	render_backend_load_op_t load_op;
	render_backend_store_op_t store_op;
} FGPassAttachmentConfig;

typedef struct render_pass_context_s RenderPassContext;

typedef struct i_render_backend_s
{
	const char *name;
	qboolean (*init)(void);
	void (*shutdown)(void);
	void (*on_resize)(int width, int height);
	qboolean (*can_activate)(qboolean runtime_switch);
	void (*begin_frame)(void);
	void (*end_frame)(void);
	void (*present)(void);
	void (*begin_pass_ex)(const RenderBackendPassDesc *pass_desc);
	void (*end_pass_ex)(void);
	void (*resource_barrier)(const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count);
	void (*bind_pipeline)(const RenderBackendPipelineDesc *pipeline);
	void (*set_dynamic_state)(const RenderBackendDynamicState *dynamic_state);
	void (*bind_descriptors)(const RenderBackendDescriptorBinding *bindings, unsigned count);
	void (*pass_setup_view)(RenderPassContext *ctx);
	void (*pass_shadowmaps)(RenderPassContext *ctx);
	void (*pass_render_scene)(RenderPassContext *ctx);
	void (*pass_warp_resolve)(RenderPassContext *ctx);
	void (*pass_postprocess)(RenderPassContext *ctx);
	void (*pass_overlay_viewmodel)(RenderPassContext *ctx);
	void (*pass_overlay_polyblend)(RenderPassContext *ctx);

	/* Compatibility path for legacy OpenGL backend integration. */
	void (*begin_pass)(const char *name);
	void (*end_pass)(void);
	void (*validate_pass_state)(const char *pass_name, qboolean before_pass);
	void (*begin_timer)(int pass_id);
	void (*end_timer)(int pass_id);
	void (*resolve_timers)(void);
	qboolean (*consume_timer_sample)(int pass_id, double *out_gpu_ms);
	const RenderBackendCaps *(*get_caps)(void);
	unsigned (*resolve_resource_id)(const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource);
	qboolean (*is_resource_valid)(const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource);
	void (*bind_render_target)(const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer);
	void (*set_viewport)(int x, int y, int width, int height);
	void (*set_scissor)(qboolean enabled, int x, int y, int width, int height);
	void (*set_pipeline_state)(unsigned state_bits);
	void (*draw)(render_backend_primitive_t primitive, int first, int count);
	void (*draw_indexed)(render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes);
	void (*draw_instanced)(render_backend_primitive_t primitive, int first, int count, int instance_count);
	void (*draw_indexed_instanced)(render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count);
	void (*draw_indexed_indirect)(render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes);
	void (*multi_draw_indexed_indirect)(render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes);
	void (*dispatch)(unsigned group_x, unsigned group_y, unsigned group_z);
	void (*memory_barrier)(unsigned barrier_bits);
	void (*set_blend_factors)(render_blend_factor_t src, render_blend_factor_t dst);
	void (*finish)(void);
	void (*populate_framegraph_resources)(RenderGraphResourceHandle *out_handles);
	int (*get_scene_sample_count)(void);
} IRenderBackend;

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
void R_Backend_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes);
void R_Backend_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes);
void R_Backend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z);
void R_Backend_MemoryBarrier (unsigned barrier_bits);
void R_Backend_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst);
void R_Backend_Finish (void);
void R_Backend_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles);
int R_Backend_GetSceneSampleCount (void);

#endif
