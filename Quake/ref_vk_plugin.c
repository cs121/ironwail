#include "renderer_plugin.h"

static const RenderBackendCaps s_vk_caps = {
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

static qboolean VK_Init (void) { return false; }
static void VK_Shutdown (void) {}
static void VK_OnResize (int width, int height) { (void)width; (void)height; }

static qboolean VK_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return false;
}

static void VK_BeginFrame (void) {}
static void VK_EndFrame (void) {}
static void VK_Present (void) {}
static void VK_BeginPassEx (const RenderBackendPassDesc *pass_desc) { (void)pass_desc; }
static void VK_EndPassEx (void) {}
static void VK_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count) { (void)resources; (void)barriers; (void)count; }
static void VK_BindPipeline (const RenderBackendPipelineDesc *pipeline) { (void)pipeline; }
static void VK_SetDynamicState (const RenderBackendDynamicState *dynamic_state) { (void)dynamic_state; }
static void VK_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count) { (void)bindings; (void)count; }
static void VK_PassSetupView (RenderPassContext *ctx) { (void)ctx; }
static void VK_PassShadowmaps (RenderPassContext *ctx) { (void)ctx; }
static void VK_PassRenderScene (RenderPassContext *ctx) { (void)ctx; }
static void VK_PassWarpResolve (RenderPassContext *ctx) { (void)ctx; }
static void VK_PassPostprocess (RenderPassContext *ctx) { (void)ctx; }
static void VK_PassOverlayViewmodel (RenderPassContext *ctx) { (void)ctx; }
static void VK_PassOverlayPolyblend (RenderPassContext *ctx) { (void)ctx; }
static qboolean VK_HasRequiredPassCallbacks (void) { return true; }
static void VK_BeginPass (const char *name) { (void)name; }
static void VK_EndPass (void) {}
static void VK_ValidatePassState (const char *pass_name, qboolean before_pass) { (void)pass_name; (void)before_pass; }
static void VK_BeginTimer (int pass_id) { (void)pass_id; }
static void VK_EndTimer (int pass_id) { (void)pass_id; }
static void VK_ResolveTimers (void) {}
static qboolean VK_ConsumeTimerSample (int pass_id, double *out_gpu_ms) { (void)pass_id; (void)out_gpu_ms; return false; }
static const RenderBackendCaps *VK_GetCaps (void) { return &s_vk_caps; }
static unsigned VK_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { (void)resources; (void)resource; return 0u; }
static qboolean VK_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { (void)resources; (void)resource; return false; }
static void VK_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer) { (void)resources; (void)resource; (void)backbuffer; }
static void VK_SetViewport (int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; }
static void VK_SetScissor (qboolean enabled, int x, int y, int width, int height) { (void)enabled; (void)x; (void)y; (void)width; (void)height; }
static void VK_SetPipelineState (unsigned state_bits) { (void)state_bits; }
static void VK_Draw (render_backend_primitive_t primitive, int first, int count) { (void)primitive; (void)first; (void)count; }
static void VK_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes) { (void)primitive; (void)index_type; (void)count; (void)index_offset_bytes; }
static void VK_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count) { (void)primitive; (void)first; (void)count; (void)instance_count; }
static void VK_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count) { (void)primitive; (void)index_type; (void)count; (void)index_offset_bytes; (void)instance_count; }
static void VK_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes) { (void)primitive; (void)index_type; (void)indirect_offset_bytes; }
static void VK_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes) { (void)primitive; (void)index_type; (void)indirect_offset_bytes; (void)draw_count; (void)stride_bytes; }
static void VK_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z) { (void)group_x; (void)group_y; (void)group_z; }
static void VK_MemoryBarrier (unsigned barrier_bits) { (void)barrier_bits; }
static void VK_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst) { (void)src; (void)dst; }
static void VK_SetDepthFunc (render_backend_depth_func_t depth_func) { (void)depth_func; }
static unsigned VK_CreatePostFXLUTTexture (void) { return 0u; }
static void VK_ConfigurePostFXLUTTexture (unsigned texture_id) { (void)texture_id; }
static void VK_Finish (void) {}
static void VK_PopulateFramegraphResources (RenderGraphResourceHandle *out_handles) { if (out_handles) memset (out_handles, 0, sizeof (*out_handles)); }
static int VK_GetSceneSampleCount (void) { return 1; }

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
	VK_HasRequiredPassCallbacks,
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
	if (!host_api->register_backend)
		return false;

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
