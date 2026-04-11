#include "quakedef.h"
#include "r_framegraph.h"
#include "renderer_plugin.h"

enum
{
	R_BACKEND_MAX_REGISTERED = 8,
	R_BACKEND_MAX_PLUGIN_LIBS = 8
};

static const char *s_renderer_plugin_prefix = "ironwail_renderer_";

static const IRenderBackend *s_registered_backends[R_BACKEND_MAX_REGISTERED];
static int s_registered_backend_count = 0;
static const IRenderBackend *s_active_backend = NULL;
static RenderBackendCaps s_active_backend_caps;
static qboolean s_backend_initialized = false;
static qboolean s_applying_backend_cvar = false;
static qboolean s_backend_active = false;
static qboolean s_backend_audit_cmd_registered = false;
static qboolean s_backend_vulkan_status_cmd_registered = false;
static qboolean s_backend_dx12_status_cmd_registered = false;
static qboolean s_warned_deprecated_builtin_register = false;
static int s_missing_resource_warn_frame[R_BACKEND_RESOURCE_SLOT_COUNT];
static void *s_plugin_libs[R_BACKEND_MAX_PLUGIN_LIBS];
static int s_plugin_lib_count = 0;
static qboolean s_command_encoder_recording = false;
static int s_command_encoder_frame = -1;

cvar_t r_backend = { "r_backend", "OpenGL", CVAR_ARCHIVE };
cvar_t r_backend_legacy_fallbacks = { "r_backend_legacy_fallbacks", "1", CVAR_ARCHIVE };

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

static void R_Backend_ValidateDescriptorBindings (const RenderBackendDescriptorBinding *bindings, unsigned count)
{
	unsigned i;
	unsigned j;

	if (!bindings || count == 0u)
		return;

	for (i = 0; i < count; ++i)
	{
		const RenderBackendDescriptorBinding *binding = &bindings[i];

		if (binding->resource_id == 0u)
		{
			Con_DWarning ("Renderer descriptor binding[%u] has invalid resource_id=0 (type=%u slot=%u)\n",
				i, (unsigned)binding->type, binding->slot);
			SDL_assert (!"Renderer descriptor binding has resource_id=0");
		}

		for (j = i + 1u; j < count; ++j)
		{
			const RenderBackendDescriptorBinding *other = &bindings[j];
			if (binding->type != other->type || binding->slot != other->slot)
				continue;
			if (binding->resource_id == other->resource_id
				&& binding->offset == other->offset
				&& binding->range == other->range)
				continue;

			Con_DWarning ("Renderer descriptor conflict: bindings[%u] and [%u] target same type=%u slot=%u with different resources/ranges\n",
				i, j, (unsigned)binding->type, binding->slot);
			SDL_assert (!"Renderer descriptor conflict on same type/slot");
		}
	}
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

static qboolean R_Backend_HasRegisteredName (const char *backend_name)
{
	return R_Backend_FindByName (backend_name) != NULL;
}

static qboolean R_Backend_RegisterViaHostApi (const IRenderBackend *backend);
static void R_Backend_FillPluginHostApi (iw_renderer_plugin_host_api_t *host_api);

static qboolean R_Backend_RegisterBuiltinByName (const char *backend_name)
{
	const IRenderBackend *backend;

	if (!backend_name || !backend_name[0])
		return false;

	if (!s_warned_deprecated_builtin_register)
	{
		Con_DWarning ("Renderer plugin used deprecated host callback register_builtin_backend(); switch to register_backend().\n");
		s_warned_deprecated_builtin_register = true;
	}

	if (!q_strcasecmp (backend_name, "OpenGL"))
	{
		backend = IW_RendererPlugin_GetBuiltinOpenGLBackend ();
		return R_Backend_RegisterViaHostApi (backend);
	}

	return false;
}

static void R_Backend_FillPluginHostApi (iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api)
		return;

	memset (host_api, 0, sizeof (*host_api));
	host_api->struct_size = sizeof (*host_api);
	host_api->abi_major = IW_RENDERER_PLUGIN_ABI_MAJOR;
	host_api->abi_minor = IW_RENDERER_PLUGIN_ABI_MINOR;
	host_api->register_backend = R_Backend_RegisterViaHostApi;
	host_api->builtin_opengl_backend = IW_RendererPlugin_GetBuiltinOpenGLBackend ();
	host_api->register_builtin_backend = R_Backend_RegisterBuiltinByName;
}

static qboolean R_Backend_RegisterBuiltinPluginOpenGL (const iw_renderer_plugin_host_api_t *host_api)
{
	const IRenderBackend *backend;

	if (!host_api || host_api->struct_size < sizeof (*host_api))
		return false;

	backend = host_api->builtin_opengl_backend;
	if (backend && host_api->register_backend)
		return host_api->register_backend (backend);
	if (host_api->register_builtin_backend)
		return host_api->register_builtin_backend ("OpenGL");
	return false;
}

static void R_Backend_RecordPluginLibrary (void *lib)
{
	if (!lib)
		return;
	if (s_plugin_lib_count >= R_BACKEND_MAX_PLUGIN_LIBS)
	{
		Sys_CloseLibrary (lib);
		Con_Warning ("Renderer plugin loader reached capacity (%d); ignoring extra plugin library.\n", R_BACKEND_MAX_PLUGIN_LIBS);
		return;
	}

	s_plugin_libs[s_plugin_lib_count++] = lib;
}

static void R_Backend_UnloadPluginLibraries (void)
{
	while (s_plugin_lib_count > 0)
	{
		void *lib = s_plugin_libs[--s_plugin_lib_count];
		if (lib)
			Sys_CloseLibrary (lib);
		s_plugin_libs[s_plugin_lib_count] = NULL;
	}
}

static qboolean R_Backend_LoadPluginFromPath (const char *path)
{
	iw_renderer_plugin_query_fn query_fn;
	const iw_renderer_plugin_descriptor_t *descriptor;
	iw_renderer_plugin_host_api_t host_api;
	void *lib;
	const char *plugin_name;

	if (!path || !path[0])
		return false;

	lib = Sys_LoadLibrary (path);
	if (!lib)
		return false;

	query_fn = (iw_renderer_plugin_query_fn) Sys_GetLibraryFunction (lib, "IW_RendererPlugin_Query");
	if (!query_fn)
	{
		Con_Warning ("Renderer plugin '%s' does not export IW_RendererPlugin_Query.\n", path);
		Sys_CloseLibrary (lib);
		return false;
	}

	descriptor = query_fn ();
	if (!descriptor || descriptor->struct_size < sizeof (*descriptor))
	{
		Con_Warning ("Renderer plugin '%s' returned an invalid descriptor.\n", path);
		Sys_CloseLibrary (lib);
		return false;
	}

	plugin_name = (descriptor->plugin_name && descriptor->plugin_name[0]) ? descriptor->plugin_name : path;
	if (descriptor->abi_major != IW_RENDERER_PLUGIN_ABI_MAJOR)
	{
		Con_Warning (
			"Renderer plugin '%s' ABI mismatch: host=%u.%u plugin=%u.%u (major mismatch)\n",
			plugin_name,
			IW_RENDERER_PLUGIN_ABI_MAJOR,
			IW_RENDERER_PLUGIN_ABI_MINOR,
			descriptor->abi_major,
			descriptor->abi_minor);
		Sys_CloseLibrary (lib);
		return false;
	}
	if (descriptor->abi_minor > IW_RENDERER_PLUGIN_ABI_MINOR)
	{
		Con_Warning (
			"Renderer plugin '%s' requires newer ABI %u.%u than host %u.%u.\n",
			plugin_name,
			descriptor->abi_major,
			descriptor->abi_minor,
			IW_RENDERER_PLUGIN_ABI_MAJOR,
			IW_RENDERER_PLUGIN_ABI_MINOR);
		Sys_CloseLibrary (lib);
		return false;
	}
	if (!descriptor->register_plugin)
	{
		Con_Warning ("Renderer plugin '%s' has no register_plugin callback.\n", plugin_name);
		Sys_CloseLibrary (lib);
		return false;
	}

	R_Backend_FillPluginHostApi (&host_api);

	if (!descriptor->register_plugin (&host_api))
	{
		Con_Warning ("Renderer plugin '%s' registration callback failed.\n", plugin_name);
		Sys_CloseLibrary (lib);
		return false;
	}

	Con_DPrintf ("Renderer plugin loaded: %s (%s)\n", plugin_name, path);
	R_Backend_RecordPluginLibrary (lib);
	return true;
}

static void R_Backend_LoadRendererPlugins (void)
{
	findfile_t *find;
	iw_renderer_plugin_host_api_t host_api;
	const iw_renderer_plugin_descriptor_t builtin_plugin = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"builtin-opengl",
		R_Backend_RegisterBuiltinPluginOpenGL
	};
	const char *search_dirs[3];
	const char *ext_find;
	int i;

#ifdef _WIN32
	ext_find = "dll";
#elif defined(__APPLE__)
	ext_find = "dylib";
#else
	ext_find = "so";
#endif

	search_dirs[0] = (host_parms && host_parms->exedir && host_parms->exedir[0]) ? host_parms->exedir : NULL;
	search_dirs[1] = (host_parms && host_parms->basedir && host_parms->basedir[0]) ? host_parms->basedir : NULL;
	search_dirs[2] = ".";

	for (i = 0; i < (int)Q_COUNTOF (search_dirs); ++i)
	{
		const char *dir = search_dirs[i];
		if (!dir || !dir[0])
			continue;
		if (i > 0 && search_dirs[0] && !q_strcasecmp (dir, search_dirs[0]))
			continue;

		for (find = Sys_FindFirst (dir, ext_find); find; find = Sys_FindNext (find))
		{
			char plugin_path[MAX_OSPATH];

			if (find->attribs & FA_DIRECTORY)
				continue;
			if (q_strncasecmp (find->name, s_renderer_plugin_prefix, strlen (s_renderer_plugin_prefix)) != 0)
				continue;
			if ((size_t) q_snprintf (plugin_path, sizeof (plugin_path), "%s/%s", dir, find->name) >= sizeof (plugin_path))
				continue;

			R_Backend_LoadPluginFromPath (plugin_path);
		}
	}

	/* Keep a deterministic in-process OpenGL plugin fallback for installs without DLL/SO deployment. */
	if (!R_Backend_HasRegisteredName ("OpenGL"))
	{
		R_Backend_FillPluginHostApi (&host_api);
		(void)builtin_plugin.register_plugin (&host_api);
	}
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
static void R_VulkanStub_SetDepthFunc (render_backend_depth_func_t depth_func) { (void)depth_func; }
static unsigned R_VulkanStub_CreatePostFXLUTTexture (void) { return 0u; }
static void R_VulkanStub_ConfigurePostFXLUTTexture (unsigned texture_id) { (void)texture_id; }
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
	R_VulkanStub_SetDepthFunc,
	R_VulkanStub_CreatePostFXLUTTexture,
	R_VulkanStub_ConfigurePostFXLUTTexture,
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

static const IRenderBackend s_dx12_stub_backend = {
	"DX12",
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
	R_VulkanStub_SetDepthFunc,
	R_VulkanStub_CreatePostFXLUTTexture,
	R_VulkanStub_ConfigurePostFXLUTTexture,
	R_VulkanStub_Finish,
	R_VulkanStub_PopulateFrameGraphResources,
	R_VulkanStub_GetSceneSampleCount
};

static void R_Backend_DX12Status_f (void)
{
	Con_Printf ("DX12 backend status:\n");
	Con_Printf ("  registration: present as stub backend ('DX12').\n");
	Con_Printf ("  activation gate: blocked (can_activate=false).\n");
	Con_Printf ("  implemented callbacks: contract no-op stubs only.\n");
	Con_Printf ("  remaining work: device init + swapchain + command lists + descriptor heaps + resource barriers.\n");
}

static qboolean R_Backend_RegisterViaHostApi (const IRenderBackend *backend)
{
	int i;

	if (!backend || !backend->name || !backend->name[0])
		return false;
	if (!R_Backend_ValidateContract (backend, true))
		return false;

	for (i = 0; i < s_registered_backend_count; ++i)
	{
		if (s_registered_backends[i] == backend
			|| !q_strcasecmp (s_registered_backends[i]->name, backend->name))
			return true;
	}

	if (s_registered_backend_count >= R_BACKEND_MAX_REGISTERED)
	{
		Con_Warning ("Renderer backend registry full (%d), cannot register '%s'\n",
			R_BACKEND_MAX_REGISTERED,
			backend->name);
		return false;
	}

	s_registered_backends[s_registered_backend_count++] = backend;
	if (!s_active_backend)
		s_active_backend = backend;
	return true;
}

void R_Backend_Register (const IRenderBackend *backend)
{
	(void)R_Backend_RegisterViaHostApi (backend);
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
		if (backend->name && !q_strcasecmp (backend->name, "Vulkan"))
			Con_Warning ("Use command 'r_backend_vulkan_status' for detailed bring-up status.\n");
		if (backend->name && !q_strcasecmp (backend->name, "DX12"))
			Con_Warning ("Use command 'r_backend_dx12_status' for detailed bring-up status.\n");
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
	Cvar_RegisterVariable (&r_backend_legacy_fallbacks);
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
	if (!s_backend_dx12_status_cmd_registered)
	{
		Cmd_AddCommand ("r_backend_dx12_status", R_Backend_DX12Status_f);
		s_backend_dx12_status_cmd_registered = true;
	}

	R_Backend_Register (&s_vulkan_stub_backend);
	R_Backend_Register (&s_dx12_stub_backend);
	R_Backend_LoadRendererPlugins ();

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
	R_Backend_UnloadPluginLibraries ();
	{
		int i;
		for (i = 0; i < R_BACKEND_MAX_REGISTERED; ++i)
			s_registered_backends[i] = NULL;
	}
	s_registered_backend_count = 0;
	s_active_backend = NULL;
	s_backend_initialized = false;
	s_command_encoder_recording = false;
	s_command_encoder_frame = -1;
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
	{
		R_Backend_ValidateDescriptorBindings (bindings, count);
		backend->bind_descriptors (bindings, count);
	}
}

void R_Backend_BeginCommandEncoder (const RenderBackendCommandEncoderDesc *desc)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	const char *label = (desc && desc->name && desc->name[0]) ? desc->name : "framegraph-encoder";

	if (s_command_encoder_recording)
	{
		if (r_framegraph_debug.value > 0.f)
			Con_DWarning ("Renderer command encoder already recording (frame=%d)\n", s_command_encoder_frame);
		return;
	}

	s_command_encoder_recording = true;
	s_command_encoder_frame = r_framecount;
	if (backend && backend->begin_pass)
		backend->begin_pass (label);
}

void R_Backend_EndCommandEncoder (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!s_command_encoder_recording)
		return;

	if (backend && backend->end_pass)
		backend->end_pass ();
}

void R_Backend_SubmitCommandEncoder (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!s_command_encoder_recording)
		return;

	if (backend && backend->memory_barrier)
		backend->memory_barrier (R_BACKEND_BARRIER_COMMAND);
	s_command_encoder_recording = false;
	s_command_encoder_frame = -1;
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
	if (!backend)
		return;

	if (backend->bind_pipeline)
	{
		RenderBackendPipelineDesc pipeline_desc;
		pipeline_desc.pipeline_id = 0u;
		pipeline_desc.state_bits = state_bits;
		backend->bind_pipeline (&pipeline_desc);
		return;
	}

	if (backend->set_pipeline_state)
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

void R_Backend_DrawPacket (const RenderBackendDrawPacket *packet)
{
	if (!packet || packet->count <= 0)
		return;

	if ((packet->flags & R_BACKEND_DRAW_PACKET_INDEXED) != 0u)
	{
		if ((packet->flags & R_BACKEND_DRAW_PACKET_INSTANCED) != 0u)
		{
			R_Backend_DrawIndexedInstanced (
				packet->primitive,
				packet->index_type,
				packet->count,
				packet->index_offset_bytes,
				packet->instance_count);
			return;
		}

		R_Backend_DrawIndexed (
			packet->primitive,
			packet->index_type,
			packet->count,
			packet->index_offset_bytes);
		return;
	}

	if ((packet->flags & R_BACKEND_DRAW_PACKET_INSTANCED) != 0u)
	{
		R_Backend_DrawInstanced (
			packet->primitive,
			packet->first,
			packet->count,
			packet->instance_count);
		return;
	}

	R_Backend_Draw (packet->primitive, packet->first, packet->count);
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

void R_Backend_SetDepthFunc (render_backend_depth_func_t depth_func)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->set_depth_func)
		backend->set_depth_func (depth_func);
}

unsigned R_Backend_CreatePostFXLUTTexture (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->create_postfx_lut_texture)
		return backend->create_postfx_lut_texture ();
	return 0u;
}

void R_Backend_ConfigurePostFXLUTTexture (unsigned texture_id)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->configure_postfx_lut_texture)
		backend->configure_postfx_lut_texture (texture_id);
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
