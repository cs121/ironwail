#include "renderer_plugin.h"
#include <stdarg.h>
#include <stdio.h>

static void Con_Printf (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	vfprintf (stdout, fmt, args);
	va_end (args);
}

static const RenderBackendCaps s_dx12_caps = {
	false, /* supports_timestamps */
	false, /* supports_compute */
	false, /* supports_draw_instanced */
	false, /* supports_draw_indirect */
	false, /* supports_multi_draw_indirect */
	false, /* supports_memory_barrier */
	0u,    /* msaa_mode_mask */
	1u,    /* max_msaa_samples */
	0u,    /* shader_model */
	false, /* supports_bindless */
	0u,    /* max_textures */
	0u,    /* max_samplers */
	0u,    /* max_ubos */
	0u     /* max_ssbos */
};

static qboolean DX12_ContextInit (void *window_handle) { (void)window_handle; return false; }
static void DX12_ContextShutdown (void) {}
static void DX12_SwapBuffers (void) {}

static qboolean DX12_Init (void) { return false; }
static void DX12_Shutdown (void) {}
static void DX12_OnResize (int width, int height) { (void)width; (void)height; }

static qboolean DX12_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return false;
}

static void DX12_BeginFrame (void) {}
static void DX12_EndFrame (void) {}
static void DX12_Present (void) {}
static void DX12_BeginPassEx (const RenderBackendPassDesc *pass_desc) { (void)pass_desc; }
static void DX12_EndPassEx (void) {}
static void DX12_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count) { (void)resources; (void)barriers; (void)count; }
static void DX12_BindPipeline (const RenderBackendPipelineDesc *pipeline) { (void)pipeline; }
static void DX12_SetDynamicState (const RenderBackendDynamicState *dynamic_state) { (void)dynamic_state; }
static void DX12_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count) { (void)bindings; (void)count; }
static void DX12_PassSetupView (RenderPassContext *ctx) { (void)ctx; }
static void DX12_PassShadowmaps (RenderPassContext *ctx) { (void)ctx; }
static void DX12_PassRenderScene (RenderPassContext *ctx) { (void)ctx; }
static void DX12_PassWarpResolve (RenderPassContext *ctx) { (void)ctx; }
static void DX12_PassPostprocess (RenderPassContext *ctx) { (void)ctx; }
static void DX12_PassOverlayViewmodel (RenderPassContext *ctx) { (void)ctx; }
static void DX12_PassOverlayPolyblend (RenderPassContext *ctx) { (void)ctx; }
static qboolean DX12_HasRequiredPassCallbacks (void) { return true; }
static void DX12_BeginPass (const char *name) { (void)name; }
static void DX12_EndPass (void) {}
static void DX12_ValidatePassState (const char *pass_name, qboolean before_pass) { (void)pass_name; (void)before_pass; }
static void DX12_BeginTimer (int pass_id) { (void)pass_id; }
static void DX12_EndTimer (int pass_id) { (void)pass_id; }
static void DX12_ResolveTimers (void) {}
static qboolean DX12_ConsumeTimerSample (int pass_id, double *out_gpu_ms) { (void)pass_id; (void)out_gpu_ms; return false; }
static const RenderBackendCaps *DX12_GetCaps (void) { return &s_dx12_caps; }
static unsigned DX12_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { (void)resources; (void)resource; return 0u; }
static qboolean DX12_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { (void)resources; (void)resource; return false; }
static void DX12_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer) { (void)resources; (void)resource; (void)backbuffer; }
static void DX12_SetViewport (int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; }
static void DX12_SetScissor (qboolean enabled, int x, int y, int width, int height) { (void)enabled; (void)x; (void)y; (void)width; (void)height; }
static void DX12_SetPipelineState (unsigned state_bits) { (void)state_bits; }
static void DX12_Draw (render_backend_primitive_t primitive, int first, int count) { (void)primitive; (void)first; (void)count; }
static void DX12_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes) { (void)primitive; (void)index_type; (void)count; (void)index_offset_bytes; }
static void DX12_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count) { (void)primitive; (void)first; (void)count; (void)instance_count; }
static void DX12_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count) { (void)primitive; (void)index_type; (void)count; (void)index_offset_bytes; (void)instance_count; }
static void DX12_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes) { (void)primitive; (void)index_type; (void)indirect_offset_bytes; }
static void DX12_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes) { (void)primitive; (void)index_type; (void)indirect_offset_bytes; (void)draw_count; (void)stride_bytes; }
static void DX12_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z) { (void)group_x; (void)group_y; (void)group_z; }
static void DX12_MemoryBarrier (unsigned barrier_bits) { (void)barrier_bits; }
static void DX12_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst) { (void)src; (void)dst; }
static void DX12_SetDepthFunc (render_backend_depth_func_t depth_func) { (void)depth_func; }
static unsigned DX12_CreatePostFXLUTTexture (void) { return 0u; }
static void DX12_ConfigurePostFXLUTTexture (unsigned texture_id) { (void)texture_id; }
static void DX12_Finish (void) {}
static qboolean DX12_QuerySurfaceMetrics (RenderBackendSurfaceMetrics *out_metrics) { (void)out_metrics; return false; }
static qboolean DX12_NeedsSceneEffects (void) { return false; }
static qboolean DX12_NeedsPostprocess (void) { return false; }
static void DX12_PopulateFramegraphResources (RenderGraphResourceHandle *out_handles) { if (out_handles) memset (out_handles, 0, sizeof (*out_handles)); }
static int DX12_GetSceneSampleCount (void) { return 1; }

static const IRenderBackend s_ref_dx12_backend = {
	"DX12",
	DX12_ContextInit,
	DX12_ContextShutdown,
	DX12_SwapBuffers,
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
	DX12_HasRequiredPassCallbacks,
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
	DX12_QuerySurfaceMetrics,
	DX12_NeedsSceneEffects,
	DX12_NeedsPostprocess,
	DX12_PopulateFramegraphResources,
	DX12_GetSceneSampleCount
};

static qboolean IW_RendererRefDX12_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V4_SIZE)
		return false;
	if (!host_api->register_backend)
		return false;

	Con_Printf ("[ref_dx12] capability banner: STUB backend loaded (experimental bring-up only).\n");
	Con_Printf ("[ref_dx12] milestones: init=no pass_callbacks=no present=no resource_translation=no\n");
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
