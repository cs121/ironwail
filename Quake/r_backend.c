#include "quakedef.h"

#include "gl_backend.h"
#include "r_framegraph.h"

enum
{
	R_BACKEND_MAX_REGISTERED = 8
};

static const IRenderBackend *s_registered_backends[R_BACKEND_MAX_REGISTERED];
static int s_registered_backend_count = 0;
static const IRenderBackend *s_active_backend = NULL;
static RenderBackendCaps s_active_backend_caps;
static qboolean s_backend_initialized = false;
static qboolean s_applying_backend_cvar = false;
static qboolean s_backend_active = false;
static qboolean s_backend_audit_cmd_registered = false;
static qboolean s_backend_vulkan_status_cmd_registered = false;
static int s_missing_resource_warn_frame[R_BACKEND_RESOURCE_SLOT_COUNT];

cvar_t r_backend = { "r_backend", "OpenGL", CVAR_ARCHIVE };

/*
================
R_Backend_WrapperAudit_f

Tracks high-value legacy wrappers still in heavy use so migration work can be
planned around backend-neutral pipeline/descriptors/dynamic-state APIs.
================
*/
static void R_Backend_WrapperAudit_f (void)
{
	Con_Printf ("Renderer wrapper migration priorities:\n");
	Con_Printf ("  P0: R_Backend_SetPipelineState (most draw-path callsites; migrate material/decal/particle state to pipeline + dynamic state)\n");
	Con_Printf ("  P1: direct glDraw*/GL_Draw* callsites (route through R_Backend_Draw/R_Backend_DrawIndexed + descriptor-driven draw plans)\n");
	Con_Printf ("  P2: bind-time texture/program glue in legacy passes (move to descriptor sets + explicit pipeline binding)\n");
	Con_Printf ("  P3: optional compute/dispatch paths (already abstracted via R_Backend_Dispatch where available)\n");
}

/*
================
R_Backend_ValidateContract

Minimal "functional backend" contract:
- must expose capability data via get_caps().
- must support framegraph resource translation/validation callbacks.
- must expose the legacy draw and viewport hooks used by current passes.
- must populate framegraph resources each frame.

Backends that fail this contract are rejected during registration/selection so
runtime framegraph code can assert these callbacks are safe to use.
================
*/
static qboolean R_Backend_ValidateContract (const IRenderBackend *backend, qboolean emit_warning)
{
	if (!backend)
		return false;

	if (!backend->get_caps
		|| !backend->resolve_resource_id
		|| !backend->is_resource_valid
		|| !backend->bind_render_target
		|| !backend->set_viewport
		|| !backend->draw
		|| !backend->draw_indexed
		|| !backend->populate_framegraph_resources)
	{
		if (emit_warning)
		{
			Con_Warning ("Renderer backend '%s' is missing required callbacks for functional operation.\n",
				backend->name ? backend->name : "<unnamed>");
		}
		SDL_assert (!"Renderer backend contract violation");
		return false;
	}

	return true;
}

static void R_Backend_ClearActiveCaps (void)
{
	memset (&s_active_backend_caps, 0, sizeof (s_active_backend_caps));
}

static const char *R_Backend_ResourceSlotName (render_backend_resource_slot_t slot)
{
	switch (slot)
	{
	case R_BACKEND_RESOURCE_SLOT_SCENE_FBO: return "scene_fbo";
	case R_BACKEND_RESOURCE_SLOT_SCENE_COLOR: return "scene_color";
	case R_BACKEND_RESOURCE_SLOT_SCENE_VELOCITY: return "scene_velocity";
	case R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH: return "scene_depth";
	case R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_FBO: return "resolved_scene_fbo";
	case R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_COLOR: return "resolved_scene_color";
	case R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_VELOCITY: return "resolved_scene_velocity";
	case R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO: return "composite_fbo";
	case R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR: return "composite_color";
	case R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH: return "composite_depth";
	case R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH: return "shadow_sun_depth";
	case R_BACKEND_RESOURCE_SLOT_VELOCITY: return "velocity";
	default: return "unknown";
	}
}

static qboolean R_Backend_RefreshActiveCaps (const IRenderBackend *backend, qboolean emit_warning)
{
	const RenderBackendCaps *caps;

	R_Backend_ClearActiveCaps ();
	if (!backend || !backend->get_caps)
	{
		if (emit_warning)
			Con_Warning ("Renderer backend '%s' did not provide a caps callback.\n",
				(backend && backend->name) ? backend->name : "<none>");
		return false;
	}

	caps = backend->get_caps ();
	if (!caps)
	{
		if (emit_warning)
			Con_Warning ("Renderer backend '%s' returned null caps.\n",
				backend->name ? backend->name : "<unnamed>");
		return false;
	}

	s_active_backend_caps = *caps;
	return true;
}

static const IRenderBackend *R_Backend_FindByName (const char *backend_name)
{
	int i;

	if (!backend_name || !backend_name[0])
		return NULL;

	for (i = 0; i < s_registered_backend_count; ++i)
	{
		const IRenderBackend *backend = s_registered_backends[i];
		if (backend && backend->name && !q_strcasecmp (backend->name, backend_name))
			return backend;
	}

	return NULL;
}

static void R_Backend_ApplySelectionToCvar (void)
{
	if (!s_active_backend || !s_active_backend->name)
		return;

	s_applying_backend_cvar = true;
	Cvar_SetQuick (&r_backend, s_active_backend->name);
	s_applying_backend_cvar = false;
}

static void R_Backend_Changed_f (cvar_t *var)
{
	if (!var || s_applying_backend_cvar)
		return;

	if (!R_Backend_Select (var->string))
	{
		Con_Warning ("Renderer backend change to '%s' rejected; keeping '%s'\n",
			var->string,
			(s_active_backend && s_active_backend->name) ? s_active_backend->name : "<none>");
		R_Backend_ApplySelectionToCvar ();
	}
}

static const RenderBackendCaps *R_VulkanStub_GetCaps (void)
{
	static const RenderBackendCaps caps = {0};
	return &caps;
}

static qboolean R_VulkanStub_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return false;
}

static qboolean R_VulkanStub_Init (void)
{
	return false;
}

static void R_VulkanStub_Shutdown (void) {}
static void R_VulkanStub_OnResize (int width, int height) { (void)width; (void)height; }
static void R_VulkanStub_BeginFrame (void) {}
static void R_VulkanStub_EndFrame (void) {}
static void R_VulkanStub_Present (void) {}
static void R_VulkanStub_BeginPassEx (const RenderBackendPassDesc *pass_desc) { (void)pass_desc; }
static void R_VulkanStub_EndPassEx (void) {}
static void R_VulkanStub_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count) { (void)resources; (void)barriers; (void)count; }
static void R_VulkanStub_BindPipeline (const RenderBackendPipelineDesc *pipeline) { (void)pipeline; }
static void R_VulkanStub_SetDynamicState (const RenderBackendDynamicState *dynamic_state) { (void)dynamic_state; }
static void R_VulkanStub_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count) { (void)bindings; (void)count; }
static void R_VulkanStub_PassSetupView (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_PassShadowmaps (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_PassRenderScene (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_PassWarpResolve (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_PassPostprocess (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_PassOverlayViewmodel (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_PassOverlayPolyblend (RenderPassContext *ctx) { (void)ctx; }
static void R_VulkanStub_BeginPass (const char *name) { (void)name; }
static void R_VulkanStub_EndPass (void) {}
static void R_VulkanStub_ValidatePassState (const char *pass_name, qboolean before_pass) { (void)pass_name; (void)before_pass; }
static void R_VulkanStub_BeginTimer (int pass_id) { (void)pass_id; }
static void R_VulkanStub_EndTimer (int pass_id) { (void)pass_id; }
static void R_VulkanStub_ResolveTimers (void) {}
static qboolean R_VulkanStub_ConsumeTimerSample (int pass_id, double *out_gpu_ms) { (void)pass_id; (void)out_gpu_ms; return false; }
static unsigned R_VulkanStub_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { (void)resources; (void)resource; return 0u; }
static qboolean R_VulkanStub_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource) { (void)resources; (void)resource; return false; }
static void R_VulkanStub_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer) { (void)resources; (void)resource; (void)backbuffer; }
static void R_VulkanStub_SetViewport (int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; }
static void R_VulkanStub_SetScissor (qboolean enabled, int x, int y, int width, int height) { (void)enabled; (void)x; (void)y; (void)width; (void)height; }
static void R_VulkanStub_SetPipelineState (unsigned state_bits) { (void)state_bits; }
static void R_VulkanStub_Draw (render_backend_primitive_t primitive, int first, int count) { (void)primitive; (void)first; (void)count; }
static void R_VulkanStub_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes) { (void)primitive; (void)index_type; (void)count; (void)index_offset_bytes; }
static void R_VulkanStub_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count) { (void)primitive; (void)first; (void)count; (void)instance_count; }
static void R_VulkanStub_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count) { (void)primitive; (void)index_type; (void)count; (void)index_offset_bytes; (void)instance_count; }
static void R_VulkanStub_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes) { (void)primitive; (void)index_type; (void)indirect_offset_bytes; }
static void R_VulkanStub_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes) { (void)primitive; (void)index_type; (void)indirect_offset_bytes; (void)draw_count; (void)stride_bytes; }
static void R_VulkanStub_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z) { (void)group_x; (void)group_y; (void)group_z; }
static void R_VulkanStub_MemoryBarrier (unsigned barrier_bits) { (void)barrier_bits; }
static void R_VulkanStub_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst) { (void)src; (void)dst; }
static void R_VulkanStub_Finish (void) {}
static void R_VulkanStub_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles) { (void)out_handles; }
static int R_VulkanStub_GetSceneSampleCount (void) { return 1; }

static const IRenderBackend s_vulkan_stub_backend = {
	"Vulkan",
	R_VulkanStub_Init,
	R_VulkanStub_Shutdown,
	R_VulkanStub_OnResize,
	R_VulkanStub_CanActivate,
	R_VulkanStub_BeginFrame,
	R_VulkanStub_EndFrame,
	R_VulkanStub_Present,
	R_VulkanStub_BeginPassEx,
	R_VulkanStub_EndPassEx,
	R_VulkanStub_ResourceBarrier,
	R_VulkanStub_BindPipeline,
	R_VulkanStub_SetDynamicState,
	R_VulkanStub_BindDescriptors,
	R_VulkanStub_PassSetupView,
	R_VulkanStub_PassShadowmaps,
	R_VulkanStub_PassRenderScene,
	R_VulkanStub_PassWarpResolve,
	R_VulkanStub_PassPostprocess,
	R_VulkanStub_PassOverlayViewmodel,
	R_VulkanStub_PassOverlayPolyblend,
	R_VulkanStub_BeginPass,
	R_VulkanStub_EndPass,
	R_VulkanStub_ValidatePassState,
	R_VulkanStub_BeginTimer,
	R_VulkanStub_EndTimer,
	R_VulkanStub_ResolveTimers,
	R_VulkanStub_ConsumeTimerSample,
	R_VulkanStub_GetCaps,
	R_VulkanStub_ResolveResourceId,
	R_VulkanStub_IsResourceValid,
	R_VulkanStub_BindRenderTarget,
	R_VulkanStub_SetViewport,
	R_VulkanStub_SetScissor,
	R_VulkanStub_SetPipelineState,
	R_VulkanStub_Draw,
	R_VulkanStub_DrawIndexed,
	R_VulkanStub_DrawInstanced,
	R_VulkanStub_DrawIndexedInstanced,
	R_VulkanStub_DrawIndexedIndirect,
	R_VulkanStub_MultiDrawIndexedIndirect,
	R_VulkanStub_Dispatch,
	R_VulkanStub_MemoryBarrier,
	R_VulkanStub_SetBlendFactors,
	R_VulkanStub_Finish,
	R_VulkanStub_PopulateFrameGraphResources,
	R_VulkanStub_GetSceneSampleCount
};

static void R_Backend_VulkanStatus_f (void)
{
	Con_Printf ("Vulkan backend status:\n");
	Con_Printf ("  registration: present as stub backend ('Vulkan').\n");
	Con_Printf ("  activation gate: blocked (can_activate=false).\n");
	Con_Printf ("  implemented callbacks: contract no-op stubs only.\n");
	Con_Printf ("  remaining work: swapchain + command buffers + pass graph execution + resource lifetime + descriptor/pipeline cache.\n");
}

void R_Backend_Register (const IRenderBackend *backend)
{
	int i;

	if (!backend || !backend->name || !backend->name[0])
		return;
	if (!R_Backend_ValidateContract (backend, true))
		return;

	for (i = 0; i < s_registered_backend_count; ++i)
	{
		if (s_registered_backends[i] == backend
			|| !q_strcasecmp (s_registered_backends[i]->name, backend->name))
			return;
	}

	if (s_registered_backend_count >= R_BACKEND_MAX_REGISTERED)
	{
		Con_Warning ("Renderer backend registry full (%d), cannot register '%s'\n",
			R_BACKEND_MAX_REGISTERED,
			backend->name);
		return;
	}

	s_registered_backends[s_registered_backend_count++] = backend;
	if (!s_active_backend)
		s_active_backend = backend;
}

qboolean R_Backend_Select (const char *backend_name)
{
	const IRenderBackend *backend = R_Backend_FindByName (backend_name);
	const IRenderBackend *previous = s_active_backend;
	const qboolean runtime_switch = s_backend_active && previous && backend && (previous != backend);
	qboolean activated = false;

	if (!backend)
		return false;
	if (!R_Backend_ValidateContract (backend, true))
		return false;

	if (runtime_switch && (!backend->can_activate || !backend->can_activate (true)))
	{
		Con_Warning (
			"Renderer backend '%s' cannot be activated at runtime; set r_backend and restart the engine.\n",
			backend->name ? backend->name : "<unnamed>");
		return false;
	}

	if (backend->can_activate && !backend->can_activate (false))
	{
		Con_Warning ("Renderer backend '%s' is not ready for activation.\n",
			backend->name ? backend->name : "<unnamed>");
		return false;
	}

	if (s_backend_active && previous && previous->shutdown)
		previous->shutdown ();

	R_Backend_ClearActiveCaps ();
	s_backend_active = false;
	s_active_backend = backend;
	if (!backend->init || backend->init ())
		activated = true;

	if (!activated)
	{
		Con_Warning ("Renderer backend '%s' failed to activate.\n",
			backend->name ? backend->name : "<unnamed>");

		if (previous && previous != backend)
		{
			Con_Warning ("Reverting renderer backend to '%s'.\n",
				previous->name ? previous->name : "<unnamed>");
			s_active_backend = previous;
			if (!R_Backend_ValidateContract (previous, true))
			{
				s_active_backend = NULL;
				return false;
			}
			if (!previous->init || previous->init ())
				activated = true;
		}

		if (!activated)
		{
			Con_Warning ("Failed to restore previous renderer backend; no active backend available.\n");
			s_active_backend = NULL;
			s_backend_active = false;
			return false;
		}
	}

	if (!R_Backend_RefreshActiveCaps (s_active_backend, true))
	{
		Con_Warning ("Renderer backend '%s' has no valid caps after activation.\n",
			(s_active_backend && s_active_backend->name) ? s_active_backend->name : "<none>");
		if (s_active_backend && s_active_backend->shutdown)
			s_active_backend->shutdown ();
		s_active_backend = NULL;
		s_backend_active = false;
		R_Backend_ClearActiveCaps ();
		return false;
	}

	s_backend_active = true;
	R_Backend_ApplySelectionToCvar ();
	return true;
}

void R_Backend_Init (void)
{
	if (s_backend_initialized)
		return;

	s_backend_initialized = true;
	memset (s_missing_resource_warn_frame, 0xff, sizeof (s_missing_resource_warn_frame));
	Cvar_RegisterVariable (&r_backend);
	Cvar_SetCallback (&r_backend, R_Backend_Changed_f);
	if (!s_backend_audit_cmd_registered)
	{
		Cmd_AddCommand ("r_backend_wrapper_audit", R_Backend_WrapperAudit_f);
		s_backend_audit_cmd_registered = true;
	}
	if (!s_backend_vulkan_status_cmd_registered)
	{
		Cmd_AddCommand ("r_backend_vulkan_status", R_Backend_VulkanStatus_f);
		s_backend_vulkan_status_cmd_registered = true;
	}

	GL_Backend_Register ();
	R_Backend_Register (&s_vulkan_stub_backend);

	if (!s_active_backend && s_registered_backend_count > 0)
		s_active_backend = s_registered_backends[0];

	if (!R_Backend_Select (r_backend.string))
		R_Backend_ApplySelectionToCvar ();
}

void R_Backend_Shutdown (void)
{
	if (s_backend_active && s_active_backend && s_active_backend->shutdown)
		s_active_backend->shutdown ();
	s_backend_active = false;
	R_Backend_ClearActiveCaps ();
}

void R_Backend_OnResize (int width, int height)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (backend && backend->on_resize)
		backend->on_resize (width, height);
}

const IRenderBackend *R_GetRenderBackend (void)
{
	if (!s_backend_initialized)
		R_Backend_Init ();
	return s_active_backend;
}

const RenderBackendCaps *R_Backend_GetCaps (void)
{
	if (!s_backend_initialized)
		R_Backend_Init ();
	if (s_backend_active && s_active_backend)
	{
		if (!R_Backend_RefreshActiveCaps (s_active_backend, false))
			R_Backend_ClearActiveCaps ();
	}
	return &s_active_backend_caps;
}

void R_Backend_BeginFrame (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->begin_frame)
		backend->begin_frame ();
}

void R_Backend_EndFrame (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->end_frame)
		backend->end_frame ();
}

void R_Backend_Present (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->present)
		backend->present ();
}

void R_Backend_BeginPassEx (const RenderBackendPassDesc *pass_desc)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!backend)
		return;

	if (backend->begin_pass_ex)
		backend->begin_pass_ex (pass_desc);
	else if (backend->begin_pass)
		backend->begin_pass ((pass_desc && pass_desc->name) ? pass_desc->name : "<unnamed>");
}

void R_Backend_EndPassEx (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!backend)
		return;

	if (backend->end_pass_ex)
		backend->end_pass_ex ();
	else if (backend->end_pass)
		backend->end_pass ();
}

void R_Backend_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->resource_barrier && barriers && count > 0u)
		backend->resource_barrier (resources, barriers, count);
}

void R_Backend_BindPipeline (const RenderBackendPipelineDesc *pipeline)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!backend)
		return;

	if (backend->bind_pipeline)
		backend->bind_pipeline (pipeline);
	else if (pipeline && backend->set_pipeline_state)
		backend->set_pipeline_state (pipeline->state_bits);
}

void R_Backend_SetDynamicState (const RenderBackendDynamicState *dynamic_state)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	(void)dynamic_state;

	if (backend && backend->set_dynamic_state)
		backend->set_dynamic_state (dynamic_state);
}

void R_Backend_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->bind_descriptors && bindings && count > 0u)
		backend->bind_descriptors (bindings, count);
}

const render_backend_resource_ref_t *R_FrameGraph_GetResourceRef (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot)
{
	if (!resources)
		return NULL;
	if (slot <= R_BACKEND_RESOURCE_SLOT_NONE || slot >= R_BACKEND_RESOURCE_SLOT_COUNT)
		return NULL;
	return &resources->refs[slot];
}

unsigned R_FrameGraph_ResolveResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	const render_backend_resource_ref_t *resource = R_FrameGraph_GetResourceRef (resources, slot);

	if (!backend || !backend->resolve_resource_id || !resource)
		return 0u;
	return backend->resolve_resource_id (resources, resource);
}

unsigned R_FrameGraph_ResolveRequiredResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot, const char *usage_tag)
{
	unsigned resolved;
	const char *resolved_usage = (usage_tag && usage_tag[0]) ? usage_tag : "unknown";

	if (!resources)
		return 0u;

	resolved = R_FrameGraph_ResolveResourceBySlot (resources, slot);
	if (resolved != 0u)
		return resolved;
	if (slot <= R_BACKEND_RESOURCE_SLOT_NONE || slot >= R_BACKEND_RESOURCE_SLOT_COUNT)
		return 0u;

	if (s_missing_resource_warn_frame[slot] != r_framecount)
	{
		Con_DWarning ("FrameGraph resource contract: '%s' requires slot '%s' but it resolved to 0\n",
			resolved_usage,
			R_Backend_ResourceSlotName (slot));
		s_missing_resource_warn_frame[slot] = r_framecount;
	}

	return 0u;
}

qboolean R_FrameGraph_HasResourceBySlot (const RenderGraphResourceHandle *resources, render_backend_resource_slot_t slot)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	const render_backend_resource_ref_t *resource = R_FrameGraph_GetResourceRef (resources, slot);

	if (!backend || !backend->is_resource_valid || !resource)
		return false;
	return backend->is_resource_valid (resources, resource);
}

void R_Backend_SetViewport (int x, int y, int width, int height)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->set_viewport)
		backend->set_viewport (x, y, width, height);
}

void R_Backend_SetScissor (qboolean enabled, int x, int y, int width, int height)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->set_scissor)
		backend->set_scissor (enabled, x, y, width, height);
}

void R_Backend_SetPipelineState (unsigned state_bits)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->set_pipeline_state)
		backend->set_pipeline_state (state_bits);
}

void R_Backend_Draw (render_backend_primitive_t primitive, int first, int count)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->draw)
		backend->draw (primitive, first, count);
}

void R_Backend_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->draw_indexed)
		backend->draw_indexed (primitive, index_type, count, index_offset_bytes);
}

void R_Backend_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->draw_instanced)
		backend->draw_instanced (primitive, first, count, instance_count);
}

void R_Backend_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->draw_indexed_instanced)
		backend->draw_indexed_instanced (primitive, index_type, count, index_offset_bytes, instance_count);
}

void R_Backend_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->draw_indexed_indirect)
		backend->draw_indexed_indirect (primitive, index_type, indirect_offset_bytes);
}

void R_Backend_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->multi_draw_indexed_indirect)
		backend->multi_draw_indexed_indirect (primitive, index_type, indirect_offset_bytes, draw_count, stride_bytes);
}

void R_Backend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->dispatch)
		backend->dispatch (group_x, group_y, group_z);
}

void R_Backend_MemoryBarrier (unsigned barrier_bits)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->memory_barrier && barrier_bits != R_BACKEND_BARRIER_NONE)
		backend->memory_barrier (barrier_bits);
}

void R_Backend_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->set_blend_factors)
		backend->set_blend_factors (src, dst);
}

void R_Backend_Finish (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->finish)
		backend->finish ();
}

void R_Backend_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!out_handles)
		return;

	memset (out_handles, 0, sizeof (*out_handles));
	if (backend && backend->populate_framegraph_resources)
		backend->populate_framegraph_resources (out_handles);
}

int R_Backend_GetSceneSampleCount (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (backend && backend->get_scene_sample_count)
		return backend->get_scene_sample_count ();
	return 1;
}
