#include "renderer_plugin.h"

static const IRenderBackend *s_gl_source = NULL;

static const IRenderBackend *REFGL_Source (void)
{
	return s_gl_source;
}

static qboolean REFGL_Init (void)
{
	const IRenderBackend *b = REFGL_Source ();
	if (!b || !b->init)
		return false;
	return b->init ();
}

static void REFGL_Shutdown (void)
{
	const IRenderBackend *b = REFGL_Source ();
	if (b && b->shutdown)
		b->shutdown ();
}

static void REFGL_OnResize (int width, int height)
{
	const IRenderBackend *b = REFGL_Source ();
	if (b && b->on_resize)
		b->on_resize (width, height);
}

static qboolean REFGL_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return REFGL_Source () != NULL;
}

static void REFGL_BeginFrame (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->begin_frame) b->begin_frame (); }
static void REFGL_EndFrame (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->end_frame) b->end_frame (); }
static void REFGL_Present (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->present) b->present (); }
static void REFGL_BeginPassEx (const RenderBackendPassDesc *pass_desc) { const IRenderBackend *b = REFGL_Source (); if (b && b->begin_pass_ex) b->begin_pass_ex (pass_desc); }
static void REFGL_EndPassEx (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->end_pass_ex) b->end_pass_ex (); }
static void REFGL_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count) { const IRenderBackend *b = REFGL_Source (); if (b && b->resource_barrier) b->resource_barrier (resources, barriers, count); }
static void REFGL_BindPipeline (const RenderBackendPipelineDesc *pipeline) { const IRenderBackend *b = REFGL_Source (); if (b && b->bind_pipeline) b->bind_pipeline (pipeline); }
static void REFGL_SetDynamicState (const RenderBackendDynamicState *dynamic_state) { const IRenderBackend *b = REFGL_Source (); if (b && b->set_dynamic_state) b->set_dynamic_state (dynamic_state); }
static void REFGL_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count) { const IRenderBackend *b = REFGL_Source (); if (b && b->bind_descriptors) b->bind_descriptors (bindings, count); }
static void REFGL_PassSetupView (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_setup_view) b->pass_setup_view (ctx); }
static void REFGL_PassShadowmaps (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_shadowmaps) b->pass_shadowmaps (ctx); }
static void REFGL_PassRenderScene (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_render_scene) b->pass_render_scene (ctx); }
static void REFGL_PassWarpResolve (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_warp_resolve) b->pass_warp_resolve (ctx); }
static void REFGL_PassPostprocess (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_postprocess) b->pass_postprocess (ctx); }
static void REFGL_PassOverlayViewmodel (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_overlay_viewmodel) b->pass_overlay_viewmodel (ctx); }
static void REFGL_PassOverlayPolyblend (RenderPassContext *ctx) { const IRenderBackend *b = REFGL_Source (); if (b && b->pass_overlay_polyblend) b->pass_overlay_polyblend (ctx); }
static qboolean REFGL_HasRequiredPassCallbacks (void) { const IRenderBackend *b = REFGL_Source (); return (b && b->has_required_pass_callbacks) ? b->has_required_pass_callbacks () : false; }
static void REFGL_BeginPass (const char *name) { const IRenderBackend *b = REFGL_Source (); if (b && b->begin_pass) b->begin_pass (name); }
static void REFGL_EndPass (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->end_pass) b->end_pass (); }
static void REFGL_ValidatePassState (const char *pass_name, qboolean before_pass) { const IRenderBackend *b = REFGL_Source (); if (b && b->validate_pass_state) b->validate_pass_state (pass_name, before_pass); }
static void REFGL_BeginTimer (int pass_id) { const IRenderBackend *b = REFGL_Source (); if (b && b->begin_timer) b->begin_timer (pass_id); }
static void REFGL_EndTimer (int pass_id) { const IRenderBackend *b = REFGL_Source (); if (b && b->end_timer) b->end_timer (pass_id); }
static void REFGL_ResolveTimers (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->resolve_timers) b->resolve_timers (); }
static qboolean REFGL_ConsumeTimerSample (int pass_id, double *out_gpu_ms) { const IRenderBackend *b = REFGL_Source (); return (b && b->consume_timer_sample) ? b->consume_timer_sample (pass_id, out_gpu_ms) : false; }
static const RenderBackendCaps *REFGL_GetCaps (void) { const IRenderBackend *b = REFGL_Source (); return (b && b->get_caps) ? b->get_caps () : NULL; }
static unsigned REFGL_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { const IRenderBackend *b = REFGL_Source (); return (b && b->resolve_resource_id) ? b->resolve_resource_id (resources, resource) : 0u; }
static qboolean REFGL_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { const IRenderBackend *b = REFGL_Source (); return (b && b->is_resource_valid) ? b->is_resource_valid (resources, resource) : false; }
static void REFGL_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer) { const IRenderBackend *b = REFGL_Source (); if (b && b->bind_render_target) b->bind_render_target (resources, resource, backbuffer); }
static void REFGL_SetViewport (int x, int y, int width, int height) { const IRenderBackend *b = REFGL_Source (); if (b && b->set_viewport) b->set_viewport (x, y, width, height); }
static void REFGL_SetScissor (qboolean enabled, int x, int y, int width, int height) { const IRenderBackend *b = REFGL_Source (); if (b && b->set_scissor) b->set_scissor (enabled, x, y, width, height); }
static void REFGL_SetPipelineState (unsigned state_bits) { const IRenderBackend *b = REFGL_Source (); if (b && b->set_pipeline_state) b->set_pipeline_state (state_bits); }
static void REFGL_Draw (render_backend_primitive_t primitive, int first, int count) { const IRenderBackend *b = REFGL_Source (); if (b && b->draw) b->draw (primitive, first, count); }
static void REFGL_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes) { const IRenderBackend *b = REFGL_Source (); if (b && b->draw_indexed) b->draw_indexed (primitive, index_type, count, index_offset_bytes); }
static void REFGL_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count) { const IRenderBackend *b = REFGL_Source (); if (b && b->draw_instanced) b->draw_instanced (primitive, first, count, instance_count); }
static void REFGL_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count) { const IRenderBackend *b = REFGL_Source (); if (b && b->draw_indexed_instanced) b->draw_indexed_instanced (primitive, index_type, count, index_offset_bytes, instance_count); }
static void REFGL_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes) { const IRenderBackend *b = REFGL_Source (); if (b && b->draw_indexed_indirect) b->draw_indexed_indirect (primitive, index_type, indirect_offset_bytes); }
static void REFGL_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes) { const IRenderBackend *b = REFGL_Source (); if (b && b->multi_draw_indexed_indirect) b->multi_draw_indexed_indirect (primitive, index_type, indirect_offset_bytes, draw_count, stride_bytes); }
static void REFGL_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z) { const IRenderBackend *b = REFGL_Source (); if (b && b->dispatch) b->dispatch (group_x, group_y, group_z); }
static void REFGL_MemoryBarrier (unsigned barrier_bits) { const IRenderBackend *b = REFGL_Source (); if (b && b->memory_barrier) b->memory_barrier (barrier_bits); }
static void REFGL_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst) { const IRenderBackend *b = REFGL_Source (); if (b && b->set_blend_factors) b->set_blend_factors (src, dst); }
static void REFGL_SetDepthFunc (render_backend_depth_func_t depth_func) { const IRenderBackend *b = REFGL_Source (); if (b && b->set_depth_func) b->set_depth_func (depth_func); }
static unsigned REFGL_CreatePostFXLUTTexture (void) { const IRenderBackend *b = REFGL_Source (); return (b && b->create_postfx_lut_texture) ? b->create_postfx_lut_texture () : 0u; }
static void REFGL_ConfigurePostFXLUTTexture (unsigned texture_id) { const IRenderBackend *b = REFGL_Source (); if (b && b->configure_postfx_lut_texture) b->configure_postfx_lut_texture (texture_id); }
static void REFGL_Finish (void) { const IRenderBackend *b = REFGL_Source (); if (b && b->finish) b->finish (); }
static void REFGL_PopulateFramegraphResources (RenderGraphResourceHandle *out_handles) { const IRenderBackend *b = REFGL_Source (); if (b && b->populate_framegraph_resources) b->populate_framegraph_resources (out_handles); }
static int REFGL_GetSceneSampleCount (void) { const IRenderBackend *b = REFGL_Source (); return (b && b->get_scene_sample_count) ? b->get_scene_sample_count () : 1; }

static const IRenderBackend s_ref_gl_backend = {
	"OpenGL",
	REFGL_Init,
	REFGL_Shutdown,
	REFGL_OnResize,
	REFGL_CanActivate,
	REFGL_BeginFrame,
	REFGL_EndFrame,
	REFGL_Present,
	REFGL_BeginPassEx,
	REFGL_EndPassEx,
	REFGL_ResourceBarrier,
	REFGL_BindPipeline,
	REFGL_SetDynamicState,
	REFGL_BindDescriptors,
	REFGL_PassSetupView,
	REFGL_PassShadowmaps,
	REFGL_PassRenderScene,
	REFGL_PassWarpResolve,
	REFGL_PassPostprocess,
	REFGL_PassOverlayViewmodel,
	REFGL_PassOverlayPolyblend,
	REFGL_HasRequiredPassCallbacks,
	REFGL_BeginPass,
	REFGL_EndPass,
	REFGL_ValidatePassState,
	REFGL_BeginTimer,
	REFGL_EndTimer,
	REFGL_ResolveTimers,
	REFGL_ConsumeTimerSample,
	REFGL_GetCaps,
	REFGL_ResolveResourceId,
	REFGL_IsResourceValid,
	REFGL_BindRenderTarget,
	REFGL_SetViewport,
	REFGL_SetScissor,
	REFGL_SetPipelineState,
	REFGL_Draw,
	REFGL_DrawIndexed,
	REFGL_DrawInstanced,
	REFGL_DrawIndexedInstanced,
	REFGL_DrawIndexedIndirect,
	REFGL_MultiDrawIndexedIndirect,
	REFGL_Dispatch,
	REFGL_MemoryBarrier,
	REFGL_SetBlendFactors,
	REFGL_SetDepthFunc,
	REFGL_CreatePostFXLUTTexture,
	REFGL_ConfigurePostFXLUTTexture,
	REFGL_Finish,
	REFGL_PopulateFramegraphResources,
	REFGL_GetSceneSampleCount
};

static qboolean IW_RendererRefGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V2_SIZE)
		return false;
	if (!host_api->register_backend || !host_api->builtin_opengl_backend)
		return false;

	/* TODO(ref_gl autark, Quake/ref_gl_plugin.c): replace host_api->builtin_opengl_backend
	 * source wiring with a plugin-local OpenGL backend implementation. */
	s_gl_source = host_api->builtin_opengl_backend;
	return host_api->register_backend (&s_ref_gl_backend);
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"ref_gl",
		IW_RendererRefGL_Register
	};

	return &descriptor;
}
