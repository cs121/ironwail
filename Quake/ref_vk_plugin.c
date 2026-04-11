#include "renderer_plugin.h"

static const IRenderBackend *s_vk_source = NULL;

static const IRenderBackend *VK_Source (void)
{
	return s_vk_source;
}

static qboolean VK_Init (void)
{
	const IRenderBackend *b = VK_Source ();
	if (!b || !b->init)
		return false;
	return b->init ();
}

static void VK_Shutdown (void)
{
	const IRenderBackend *b = VK_Source ();
	if (b && b->shutdown)
		b->shutdown ();
}

static void VK_OnResize (int width, int height)
{
	const IRenderBackend *b = VK_Source ();
	if (b && b->on_resize)
		b->on_resize (width, height);
}

static qboolean VK_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return VK_Source () != NULL;
}

static void VK_BeginFrame (void) { const IRenderBackend *b = VK_Source (); if (b && b->begin_frame) b->begin_frame (); }
static void VK_EndFrame (void) { const IRenderBackend *b = VK_Source (); if (b && b->end_frame) b->end_frame (); }
static void VK_Present (void) { const IRenderBackend *b = VK_Source (); if (b && b->present) b->present (); }
static void VK_BeginPassEx (const RenderBackendPassDesc *pass_desc) { const IRenderBackend *b = VK_Source (); if (b && b->begin_pass_ex) b->begin_pass_ex (pass_desc); }
static void VK_EndPassEx (void) { const IRenderBackend *b = VK_Source (); if (b && b->end_pass_ex) b->end_pass_ex (); }
static void VK_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count) { const IRenderBackend *b = VK_Source (); if (b && b->resource_barrier) b->resource_barrier (resources, barriers, count); }
static void VK_BindPipeline (const RenderBackendPipelineDesc *pipeline) { const IRenderBackend *b = VK_Source (); if (b && b->bind_pipeline) b->bind_pipeline (pipeline); }
static void VK_SetDynamicState (const RenderBackendDynamicState *dynamic_state) { const IRenderBackend *b = VK_Source (); if (b && b->set_dynamic_state) b->set_dynamic_state (dynamic_state); }
static void VK_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count) { const IRenderBackend *b = VK_Source (); if (b && b->bind_descriptors) b->bind_descriptors (bindings, count); }
static void VK_PassSetupView (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_setup_view) b->pass_setup_view (ctx); }
static void VK_PassShadowmaps (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_shadowmaps) b->pass_shadowmaps (ctx); }
static void VK_PassRenderScene (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_render_scene) b->pass_render_scene (ctx); }
static void VK_PassWarpResolve (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_warp_resolve) b->pass_warp_resolve (ctx); }
static void VK_PassPostprocess (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_postprocess) b->pass_postprocess (ctx); }
static void VK_PassOverlayViewmodel (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_overlay_viewmodel) b->pass_overlay_viewmodel (ctx); }
static void VK_PassOverlayPolyblend (RenderPassContext *ctx) { const IRenderBackend *b = VK_Source (); if (b && b->pass_overlay_polyblend) b->pass_overlay_polyblend (ctx); }
static void VK_BeginPass (const char *name) { const IRenderBackend *b = VK_Source (); if (b && b->begin_pass) b->begin_pass (name); }
static void VK_EndPass (void) { const IRenderBackend *b = VK_Source (); if (b && b->end_pass) b->end_pass (); }
static void VK_ValidatePassState (const char *pass_name, qboolean before_pass) { const IRenderBackend *b = VK_Source (); if (b && b->validate_pass_state) b->validate_pass_state (pass_name, before_pass); }
static void VK_BeginTimer (int pass_id) { const IRenderBackend *b = VK_Source (); if (b && b->begin_timer) b->begin_timer (pass_id); }
static void VK_EndTimer (int pass_id) { const IRenderBackend *b = VK_Source (); if (b && b->end_timer) b->end_timer (pass_id); }
static void VK_ResolveTimers (void) { const IRenderBackend *b = VK_Source (); if (b && b->resolve_timers) b->resolve_timers (); }
static qboolean VK_ConsumeTimerSample (int pass_id, double *out_gpu_ms) { const IRenderBackend *b = VK_Source (); return (b && b->consume_timer_sample) ? b->consume_timer_sample (pass_id, out_gpu_ms) : false; }
static const RenderBackendCaps *VK_GetCaps (void) { const IRenderBackend *b = VK_Source (); return (b && b->get_caps) ? b->get_caps () : NULL; }
static unsigned VK_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { const IRenderBackend *b = VK_Source (); return (b && b->resolve_resource_id) ? b->resolve_resource_id (resources, resource) : 0u; }
static qboolean VK_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { const IRenderBackend *b = VK_Source (); return (b && b->is_resource_valid) ? b->is_resource_valid (resources, resource) : false; }
static void VK_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer) { const IRenderBackend *b = VK_Source (); if (b && b->bind_render_target) b->bind_render_target (resources, resource, backbuffer); }
static void VK_SetViewport (int x, int y, int width, int height) { const IRenderBackend *b = VK_Source (); if (b && b->set_viewport) b->set_viewport (x, y, width, height); }
static void VK_SetScissor (qboolean enabled, int x, int y, int width, int height) { const IRenderBackend *b = VK_Source (); if (b && b->set_scissor) b->set_scissor (enabled, x, y, width, height); }
static void VK_SetPipelineState (unsigned state_bits) { const IRenderBackend *b = VK_Source (); if (b && b->set_pipeline_state) b->set_pipeline_state (state_bits); }
static void VK_Draw (render_backend_primitive_t primitive, int first, int count) { const IRenderBackend *b = VK_Source (); if (b && b->draw) b->draw (primitive, first, count); }
static void VK_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes) { const IRenderBackend *b = VK_Source (); if (b && b->draw_indexed) b->draw_indexed (primitive, index_type, count, index_offset_bytes); }
static void VK_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count) { const IRenderBackend *b = VK_Source (); if (b && b->draw_instanced) b->draw_instanced (primitive, first, count, instance_count); }
static void VK_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count) { const IRenderBackend *b = VK_Source (); if (b && b->draw_indexed_instanced) b->draw_indexed_instanced (primitive, index_type, count, index_offset_bytes, instance_count); }
static void VK_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes) { const IRenderBackend *b = VK_Source (); if (b && b->draw_indexed_indirect) b->draw_indexed_indirect (primitive, index_type, indirect_offset_bytes); }
static void VK_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes) { const IRenderBackend *b = VK_Source (); if (b && b->multi_draw_indexed_indirect) b->multi_draw_indexed_indirect (primitive, index_type, indirect_offset_bytes, draw_count, stride_bytes); }
static void VK_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z) { const IRenderBackend *b = VK_Source (); if (b && b->dispatch) b->dispatch (group_x, group_y, group_z); }
static void VK_MemoryBarrier (unsigned barrier_bits) { const IRenderBackend *b = VK_Source (); if (b && b->memory_barrier) b->memory_barrier (barrier_bits); }
static void VK_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst) { const IRenderBackend *b = VK_Source (); if (b && b->set_blend_factors) b->set_blend_factors (src, dst); }
static void VK_SetDepthFunc (render_backend_depth_func_t depth_func) { const IRenderBackend *b = VK_Source (); if (b && b->set_depth_func) b->set_depth_func (depth_func); }
static unsigned VK_CreatePostFXLUTTexture (void) { const IRenderBackend *b = VK_Source (); return (b && b->create_postfx_lut_texture) ? b->create_postfx_lut_texture () : 0u; }
static void VK_ConfigurePostFXLUTTexture (unsigned texture_id) { const IRenderBackend *b = VK_Source (); if (b && b->configure_postfx_lut_texture) b->configure_postfx_lut_texture (texture_id); }
static void VK_Finish (void) { const IRenderBackend *b = VK_Source (); if (b && b->finish) b->finish (); }
static void VK_PopulateFramegraphResources (RenderGraphResourceHandle *out_handles) { const IRenderBackend *b = VK_Source (); if (b && b->populate_framegraph_resources) b->populate_framegraph_resources (out_handles); }
static int VK_GetSceneSampleCount (void) { const IRenderBackend *b = VK_Source (); return (b && b->get_scene_sample_count) ? b->get_scene_sample_count () : 1; }

static const IRenderBackend s_ref_vk_backend = {
	"Vulkan",
	VK_Init,
	VK_Shutdown,
	VK_OnResize,
	VK_CanActivate,
	VK_BeginFrame,
	VK_EndFrame,
	VK_Present,
	VK_BeginPassEx,
	VK_EndPassEx,
	VK_ResourceBarrier,
	VK_BindPipeline,
	VK_SetDynamicState,
	VK_BindDescriptors,
	VK_PassSetupView,
	VK_PassShadowmaps,
	VK_PassRenderScene,
	VK_PassWarpResolve,
	VK_PassPostprocess,
	VK_PassOverlayViewmodel,
	VK_PassOverlayPolyblend,
	VK_BeginPass,
	VK_EndPass,
	VK_ValidatePassState,
	VK_BeginTimer,
	VK_EndTimer,
	VK_ResolveTimers,
	VK_ConsumeTimerSample,
	VK_GetCaps,
	VK_ResolveResourceId,
	VK_IsResourceValid,
	VK_BindRenderTarget,
	VK_SetViewport,
	VK_SetScissor,
	VK_SetPipelineState,
	VK_Draw,
	VK_DrawIndexed,
	VK_DrawInstanced,
	VK_DrawIndexedInstanced,
	VK_DrawIndexedIndirect,
	VK_MultiDrawIndexedIndirect,
	VK_Dispatch,
	VK_MemoryBarrier,
	VK_SetBlendFactors,
	VK_SetDepthFunc,
	VK_CreatePostFXLUTTexture,
	VK_ConfigurePostFXLUTTexture,
	VK_Finish,
	VK_PopulateFramegraphResources,
	VK_GetSceneSampleCount
};

static qboolean IW_RendererRefVK_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V2_SIZE)
		return false;
	if (!host_api->register_backend || !host_api->builtin_opengl_backend)
		return false;

	s_vk_source = host_api->builtin_opengl_backend;
	return host_api->register_backend (&s_ref_vk_backend);
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"ref_vk",
		IW_RendererRefVK_Register
	};

	return &descriptor;
}
