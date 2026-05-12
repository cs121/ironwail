#ifndef RENDER_API_H
#define RENDER_API_H

#include "q_stdinc.h"

#define R_BACKEND_MAX_PROFILE_SLOTS 128

typedef enum render_blend_factor_e
{
	R_BLEND_FACTOR_INVALID = 0,
	R_BLEND_FACTOR_ZERO,
	R_BLEND_FACTOR_ONE,
	R_BLEND_FACTOR_SRC_ALPHA,
	R_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	R_BLEND_FACTOR_DST_ALPHA,
	R_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
	R_BLEND_FACTOR_SRC_COLOR,
	R_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
	R_BLEND_FACTOR_DST_COLOR,
	R_BLEND_FACTOR_ONE_MINUS_DST_COLOR
} render_blend_factor_t;

typedef enum render_backend_resource_type_e
{
	R_BACKEND_RESOURCE_NONE = 0,
	R_BACKEND_RESOURCE_FRAMEBUFFER,
	R_BACKEND_RESOURCE_TEXTURE,
	R_BACKEND_RESOURCE_BUFFER
} render_backend_resource_type_t;

typedef enum render_backend_resource_slot_e
{
	R_BACKEND_RESOURCE_SLOT_NONE = 0,
	R_BACKEND_RESOURCE_SLOT_SCENE_FBO,
	R_BACKEND_RESOURCE_SLOT_SCENE_COLOR,
	R_BACKEND_RESOURCE_SLOT_SCENE_VELOCITY,
	R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH,
	R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_FBO,
	R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_COLOR,
	R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_VELOCITY,
	R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO,
	R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR,
	R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH,
	R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH,
	R_BACKEND_RESOURCE_SLOT_VELOCITY,
	R_BACKEND_RESOURCE_SLOT_COUNT
} render_backend_resource_slot_t;

typedef struct render_backend_resource_ref_s
{
	unsigned char type;
	unsigned short slot;
	unsigned short opaque_id;
} render_backend_resource_ref_t;

typedef enum render_backend_primitive_e
{
	R_BACKEND_PRIMITIVE_TRIANGLES = 0,
	R_BACKEND_PRIMITIVE_TRIANGLE_FAN,
	R_BACKEND_PRIMITIVE_TRIANGLE_STRIP,
	R_BACKEND_PRIMITIVE_LINES,
	R_BACKEND_PRIMITIVE_POINTS
} render_backend_primitive_t;

typedef enum render_backend_index_type_e
{
	R_BACKEND_INDEX_TYPE_UINT16 = 0,
	R_BACKEND_INDEX_TYPE_UINT32
} render_backend_index_type_t;

typedef enum render_backend_barrier_bits_e
{
	R_BACKEND_BARRIER_NONE = 0,
	R_BACKEND_BARRIER_TEXTURE_FETCH = 1u << 0,
	R_BACKEND_BARRIER_SHADER_IMAGE_ACCESS = 1u << 1,
	R_BACKEND_BARRIER_SHADER_STORAGE = 1u << 2,
	R_BACKEND_BARRIER_COMMAND = 1u << 3,
	R_BACKEND_BARRIER_ELEMENT_ARRAY = 1u << 4,
	R_BACKEND_BARRIER_FRAMEBUFFER = 1u << 5
} render_backend_barrier_bits_t;

typedef struct render_backend_caps_s
{
	qboolean supports_timestamps;
	qboolean supports_compute;
	qboolean supports_draw_instanced;
	qboolean supports_draw_indirect;
	qboolean supports_multi_draw_indirect;
	qboolean supports_memory_barrier;
	unsigned msaa_mode_mask;
	unsigned max_msaa_samples;
	unsigned shader_model;
	qboolean supports_bindless;
	unsigned max_textures;
	unsigned max_samplers;
	unsigned max_ubos;
	unsigned max_ssbos;
} RenderBackendCaps;

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

typedef enum render_backend_depth_func_e
{
	R_BACKEND_DEPTH_FUNC_LEQUAL = 0,
	R_BACKEND_DEPTH_FUNC_LESS,
	R_BACKEND_DEPTH_FUNC_EQUAL,
	R_BACKEND_DEPTH_FUNC_GREATER,
	R_BACKEND_DEPTH_FUNC_GEQUAL,
	R_BACKEND_DEPTH_FUNC_ALWAYS,
	R_BACKEND_DEPTH_FUNC_NEVER
} render_backend_depth_func_t;

typedef enum render_backend_resource_state_e
{
	R_BACKEND_RESOURCE_STATE_UNKNOWN = 0,
	R_BACKEND_RESOURCE_STATE_ATTACHMENT_READ,
	R_BACKEND_RESOURCE_STATE_ATTACHMENT_WRITE,
	R_BACKEND_RESOURCE_STATE_ATTACHMENT_READ_WRITE,
	R_BACKEND_RESOURCE_STATE_SAMPLED,
	R_BACKEND_RESOURCE_STATE_STORAGE_READ,
	R_BACKEND_RESOURCE_STATE_STORAGE_WRITE,
	R_BACKEND_RESOURCE_STATE_PRESENT
} render_backend_resource_state_t;

typedef enum render_backend_resource_lifetime_e
{
	R_BACKEND_RESOURCE_LIFETIME_FRAME = 0,
	R_BACKEND_RESOURCE_LIFETIME_LEVEL,
	R_BACKEND_RESOURCE_LIFETIME_DEVICE,
	R_BACKEND_RESOURCE_LIFETIME_PERSISTENT
} render_backend_resource_lifetime_t;

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

typedef enum render_backend_command_encoder_flags_e
{
	R_BACKEND_COMMAND_ENCODER_NONE = 0,
	R_BACKEND_COMMAND_ENCODER_DEBUG_LABEL = 1u << 0
} render_backend_command_encoder_flags_t;

typedef struct render_backend_command_encoder_desc_s
{
	const char *name;
	unsigned frame_index;
	unsigned flags;
} RenderBackendCommandEncoderDesc;

typedef enum render_backend_draw_packet_flags_e
{
	R_BACKEND_DRAW_PACKET_NONE = 0,
	R_BACKEND_DRAW_PACKET_INDEXED = 1u << 0,
	R_BACKEND_DRAW_PACKET_INSTANCED = 1u << 1
} render_backend_draw_packet_flags_t;

typedef struct render_backend_draw_packet_s
{
	render_backend_primitive_t primitive;
	render_backend_index_type_t index_type;
	int first;
	int count;
	int instance_count;
	intptr_t index_offset_bytes;
	unsigned flags;
} RenderBackendDrawPacket;

typedef struct render_backend_surface_metrics_s
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
} RenderBackendSurfaceMetrics;

typedef struct render_pass_context_s RenderPassContext;

typedef struct i_render_backend_s
{
	const char *name;

	/* Lifecycle: init/shutdown/context/present/resize ownership. */
	qboolean (*context_init)(void *window_handle);
	void (*context_shutdown)(void);
	void (*swap_buffers)(void);
	qboolean (*init)(void);
	void (*shutdown)(void);
	void (*on_resize)(int width, int height);
	qboolean (*can_activate)(qboolean runtime_switch);

	/* Frame: per-frame backend bookends. */
	void (*begin_frame)(void);
	void (*end_frame)(void);
	void (*present)(void);

	/* Pass: explicit framegraph pass boundaries and resource barriers. */
	void (*begin_pass_ex)(const RenderBackendPassDesc *pass_desc);
	void (*end_pass_ex)(void);
	void (*resource_barrier)(const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count);

	/* Draw/Dispatch: API-neutral pipeline, descriptor, draw, compute, and sync hooks. */
	void (*bind_pipeline)(const RenderBackendPipelineDesc *pipeline);
	void (*set_dynamic_state)(const RenderBackendDynamicState *dynamic_state);
	void (*bind_descriptors)(const RenderBackendDescriptorBinding *bindings, unsigned count);

	/*
	 * Legacy Bridge: callbacks below still assume current GL/legacy renderer
	 * state baselines in Phase 1. They are intentionally documented as bridge
	 * surface, not as a final API-neutral renderer contract.
	 */
	void (*pass_setup_view)(RenderPassContext *ctx);
	void (*pass_shadowmaps)(RenderPassContext *ctx);
	void (*pass_render_scene)(RenderPassContext *ctx);
	void (*pass_warp_resolve)(RenderPassContext *ctx);
	void (*pass_postprocess)(RenderPassContext *ctx);
	void (*pass_overlay_viewmodel)(RenderPassContext *ctx);
	void (*pass_overlay_polyblend)(RenderPassContext *ctx);
	qboolean (*has_required_pass_callbacks)(void);
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
	void (*set_depth_func)(render_backend_depth_func_t depth_func);
	unsigned (*create_postfx_lut_texture)(void);
	void (*configure_postfx_lut_texture)(unsigned texture_id);
	void (*finish)(void);
	qboolean (*query_surface_metrics)(RenderBackendSurfaceMetrics *out_metrics);
	qboolean (*needs_scene_effects)(void);
	qboolean (*needs_postprocess)(void);
	void (*populate_framegraph_resources)(RenderGraphResourceHandle *out_handles);
	int (*get_scene_sample_count)(void);
	unsigned (*get_active_shader_id)(void);
	qboolean (*query_shader_metadata)(unsigned shader_id, const char **out_debug_name, const char **out_entry_point, const char **out_stage, unsigned *out_permutation_key);
	void (*apply_framegraph_baseline)(unsigned baseline_bits);

	/*
	 * TODO Future API-neutral resource model:
	 * - Texture
	 * - Buffer
	 * - Sampler
	 * - ImageView
	 * - RenderTarget
	 * - Pipeline
	 * - DescriptorSet
	 */
} IRenderBackend;

#endif
