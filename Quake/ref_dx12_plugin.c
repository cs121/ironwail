#include "renderer_plugin.h"

static const IRenderBackend *s_dx12_source = NULL;

static const IRenderBackend *DX12_Source (void)
{
	return s_dx12_source;
}

static qboolean DX12_Init (void)
{
	const IRenderBackend *b = DX12_Source ();
	if (!b || !b->init)
		return false;
	return b->init ();
}

static void DX12_Shutdown (void)
{
	const IRenderBackend *b = DX12_Source ();
	if (b && b->shutdown)
		b->shutdown ();
}

static void DX12_OnResize (int width, int height)
{
	const IRenderBackend *b = DX12_Source ();
	if (b && b->on_resize)
		b->on_resize (width, height);
}

static qboolean DX12_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return DX12_Source () != NULL;
}

static void DX12_BeginFrame (void) { const IRenderBackend *b = DX12_Source (); if (b && b->begin_frame) b->begin_frame (); }
static void DX12_EndFrame (void) { const IRenderBackend *b = DX12_Source (); if (b && b->end_frame) b->end_frame (); }
static void DX12_Present (void) { const IRenderBackend *b = DX12_Source (); if (b && b->present) b->present (); }
static void DX12_BeginPassEx (const RenderBackendPassDesc *pass_desc) { const IRenderBackend *b = DX12_Source (); if (b && b->begin_pass_ex) b->begin_pass_ex (pass_desc); }
static void DX12_EndPassEx (void) { const IRenderBackend *b = DX12_Source (); if (b && b->end_pass_ex) b->end_pass_ex (); }
static void DX12_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count) { const IRenderBackend *b = DX12_Source (); if (b && b->resource_barrier) b->resource_barrier (resources, barriers, count); }
static void DX12_BindPipeline (const RenderBackendPipelineDesc *pipeline) { const IRenderBackend *b = DX12_Source (); if (b && b->bind_pipeline) b->bind_pipeline (pipeline); }
static void DX12_SetDynamicState (const RenderBackendDynamicState *dynamic_state) { const IRenderBackend *b = DX12_Source (); if (b && b->set_dynamic_state) b->set_dynamic_state (dynamic_state); }
static void DX12_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count) { const IRenderBackend *b = DX12_Source (); if (b && b->bind_descriptors) b->bind_descriptors (bindings, count); }
static void DX12_PassSetupView (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_setup_view) b->pass_setup_view (ctx); }
static void DX12_PassShadowmaps (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_shadowmaps) b->pass_shadowmaps (ctx); }
static void DX12_PassRenderScene (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_render_scene) b->pass_render_scene (ctx); }
static void DX12_PassWarpResolve (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_warp_resolve) b->pass_warp_resolve (ctx); }
static void DX12_PassPostprocess (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_postprocess) b->pass_postprocess (ctx); }
static void DX12_PassOverlayViewmodel (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_overlay_viewmodel) b->pass_overlay_viewmodel (ctx); }
static void DX12_PassOverlayPolyblend (RenderPassContext *ctx) { const IRenderBackend *b = DX12_Source (); if (b && b->pass_overlay_polyblend) b->pass_overlay_polyblend (ctx); }
static void DX12_BeginPass (const char *name) { const IRenderBackend *b = DX12_Source (); if (b && b->begin_pass) b->begin_pass (name); }
static void DX12_EndPass (void) { const IRenderBackend *b = DX12_Source (); if (b && b->end_pass) b->end_pass (); }
static void DX12_ValidatePassState (const char *pass_name, qboolean before_pass) { const IRenderBackend *b = DX12_Source (); if (b && b->validate_pass_state) b->validate_pass_state (pass_name, before_pass); }
static void DX12_BeginTimer (int pass_id) { const IRenderBackend *b = DX12_Source (); if (b && b->begin_timer) b->begin_timer (pass_id); }
static void DX12_EndTimer (int pass_id) { const IRenderBackend *b = DX12_Source (); if (b && b->end_timer) b->end_timer (pass_id); }
static void DX12_ResolveTimers (void) { const IRenderBackend *b = DX12_Source (); if (b && b->resolve_timers) b->resolve_timers (); }
static qboolean DX12_ConsumeTimerSample (int pass_id, double *out_gpu_ms) { const IRenderBackend *b = DX12_Source (); return (b && b->consume_timer_sample) ? b->consume_timer_sample (pass_id, out_gpu_ms) : false; }
static const RenderBackendCaps *DX12_GetCaps (void) { const IRenderBackend *b = DX12_Source (); return (b && b->get_caps) ? b->get_caps () : NULL; }
static unsigned DX12_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { const IRenderBackend *b = DX12_Source (); return (b && b->resolve_resource_id) ? b->resolve_resource_id (resources, resource) : 0u; }
static qboolean DX12_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { const IRenderBackend *b = DX12_Source (); return (b && b->is_resource_valid) ? b->is_resource_valid (resources, resource) : false; }
static void DX12_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer) { const IRenderBackend *b = DX12_Source (); if (b && b->bind_render_target) b->bind_render_target (resources, resource, backbuffer); }
static void DX12_SetViewport (int x, int y, int width, int height) { const IRenderBackend *b = DX12_Source (); if (b && b->set_viewport) b->set_viewport (x, y, width, height); }
static void DX12_SetScissor (qboolean enabled, int x, int y, int width, int height) { const IRenderBackend *b = DX12_Source (); if (b && b->set_scissor) b->set_scissor (enabled, x, y, width, height); }
static void DX12_SetPipelineState (unsigned state_bits) { const IRenderBackend *b = DX12_Source (); if (b && b->set_pipeline_state) b->set_pipeline_state (state_bits); }
static void DX12_Draw (render_backend_primitive_t primitive, int first, int count) { const IRenderBackend *b = DX12_Source (); if (b && b->draw) b->draw (primitive, first, count); }
static void DX12_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes) { const IRenderBackend *b = DX12_Source (); if (b && b->draw_indexed) b->draw_indexed (primitive, index_type, count, index_offset_bytes); }
static void DX12_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count) { const IRenderBackend *b = DX12_Source (); if (b && b->draw_instanced) b->draw_instanced (primitive, first, count, instance_count); }
static void DX12_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count) { const IRenderBackend *b = DX12_Source (); if (b && b->draw_indexed_instanced) b->draw_indexed_instanced (primitive, index_type, count, index_offset_bytes, instance_count); }
static void DX12_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes) { const IRenderBackend *b = DX12_Source (); if (b && b->draw_indexed_indirect) b->draw_indexed_indirect (primitive, index_type, indirect_offset_bytes); }
static void DX12_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes) { const IRenderBackend *b = DX12_Source (); if (b && b->multi_draw_indexed_indirect) b->multi_draw_indexed_indirect (primitive, index_type, indirect_offset_bytes, draw_count, stride_bytes); }
static void DX12_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z) { const IRenderBackend *b = DX12_Source (); if (b && b->dispatch) b->dispatch (group_x, group_y, group_z); }
static void DX12_MemoryBarrier (unsigned barrier_bits) { const IRenderBackend *b = DX12_Source (); if (b && b->memory_barrier) b->memory_barrier (barrier_bits); }
static void DX12_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst) { const IRenderBackend *b = DX12_Source (); if (b && b->set_blend_factors) b->set_blend_factors (src, dst); }
static void DX12_SetDepthFunc (render_backend_depth_func_t depth_func) { const IRenderBackend *b = DX12_Source (); if (b && b->set_depth_func) b->set_depth_func (depth_func); }
static unsigned DX12_CreatePostFXLUTTexture (void) { const IRenderBackend *b = DX12_Source (); return (b && b->create_postfx_lut_texture) ? b->create_postfx_lut_texture () : 0u; }
static void DX12_ConfigurePostFXLUTTexture (unsigned texture_id) { const IRenderBackend *b = DX12_Source (); if (b && b->configure_postfx_lut_texture) b->configure_postfx_lut_texture (texture_id); }
static void DX12_Finish (void) { const IRenderBackend *b = DX12_Source (); if (b && b->finish) b->finish (); }
static void DX12_PopulateFramegraphResources (RenderGraphResourceHandle *out_handles) { const IRenderBackend *b = DX12_Source (); if (b && b->populate_framegraph_resources) b->populate_framegraph_resources (out_handles); }
static int DX12_GetSceneSampleCount (void) { const IRenderBackend *b = DX12_Source (); return (b && b->get_scene_sample_count) ? b->get_scene_sample_count () : 1; }

static const IRenderBackend s_ref_dx12_backend = {
	"DX12",
	DX12_Init,
	DX12_Shutdown,
	DX12_OnResize,
	DX12_CanActivate,
	DX12_BeginFrame,
	DX12_EndFrame,
	DX12_Present,
	DX12_BeginPassEx,
	DX12_EndPassEx,
	DX12_ResourceBarrier,
	DX12_BindPipeline,
	DX12_SetDynamicState,
	DX12_BindDescriptors,
	DX12_PassSetupView,
	DX12_PassShadowmaps,
	DX12_PassRenderScene,
	DX12_PassWarpResolve,
	DX12_PassPostprocess,
	DX12_PassOverlayViewmodel,
	DX12_PassOverlayPolyblend,
	DX12_BeginPass,
	DX12_EndPass,
	DX12_ValidatePassState,
	DX12_BeginTimer,
	DX12_EndTimer,
	DX12_ResolveTimers,
	DX12_ConsumeTimerSample,
	DX12_GetCaps,
	DX12_ResolveResourceId,
	DX12_IsResourceValid,
	DX12_BindRenderTarget,
	DX12_SetViewport,
	DX12_SetScissor,
	DX12_SetPipelineState,
	DX12_Draw,
	DX12_DrawIndexed,
	DX12_DrawInstanced,
	DX12_DrawIndexedInstanced,
	DX12_DrawIndexedIndirect,
	DX12_MultiDrawIndexedIndirect,
	DX12_Dispatch,
	DX12_MemoryBarrier,
	DX12_SetBlendFactors,
	DX12_SetDepthFunc,
	DX12_CreatePostFXLUTTexture,
	DX12_ConfigurePostFXLUTTexture,
	DX12_Finish,
	DX12_PopulateFramegraphResources,
	DX12_GetSceneSampleCount
};

static qboolean IW_RendererRefDX12_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api || host_api->struct_size < sizeof (*host_api))
		return false;
	if (!host_api->register_backend || !host_api->builtin_opengl_backend)
		return false;

	s_dx12_source = host_api->builtin_opengl_backend;
	return host_api->register_backend (&s_ref_dx12_backend);
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"ref_dx12",
		IW_RendererRefDX12_Register
	};

	return &descriptor;
}
