#include "quakedef.h"
#include "r_framegraph.h"
#include "renderer_plugin.h"
#include "renderer_host_bridge.h"
#include "render_dispatch.h"
#include "gl_backend.h" /* obsolete migration seam: compile-time symbols only */
#include "glquake.h"    /* required legacy state constants/types until Phase 7 cleanup */

enum
{
	R_BACKEND_MAX_REGISTERED = 8,
	R_BACKEND_MAX_PLUGIN_LIBS = 8
};

static const char *s_renderer_plugin_prefix = "ironwail_renderer_";
#ifdef _WIN32
static const char *s_ref_gl_plugin_filename = "ref_gl.dll";
static const char *s_ref_vk_plugin_filename = "ref_vk.dll";
static const char *s_ref_dx12_plugin_filename = "ref_dx12.dll";
#elif defined(__APPLE__)
static const char *s_ref_gl_plugin_filename = "ref_gl.dylib";
static const char *s_ref_vk_plugin_filename = "ref_vk.dylib";
static const char *s_ref_dx12_plugin_filename = "ref_dx12.dylib";
#else
static const char *s_ref_gl_plugin_filename = "ref_gl.so";
static const char *s_ref_vk_plugin_filename = "ref_vk.so";
static const char *s_ref_dx12_plugin_filename = "ref_dx12.so";
#endif

static const IRenderBackend *s_registered_backends[R_BACKEND_MAX_REGISTERED];
static int s_registered_backend_count = 0;
static const IRenderBackend *s_active_backend = NULL;
static const IRenderBackend *s_gl_backend = NULL;
static RenderBackendCaps s_active_backend_caps;
static qboolean s_backend_initialized = false;
static qboolean s_applying_backend_cvar = false;
static qboolean s_applying_backend_api_cvar = false;
static qboolean s_backend_active = false;
static qboolean s_backend_audit_cmd_registered = false;
static qboolean s_backend_vulkan_status_cmd_registered = false;
static qboolean s_backend_dx12_status_cmd_registered = false;
static qboolean s_ref_gl_plugin_candidate_found = false;
static qboolean s_ref_gl_plugin_loaded = false;
static qboolean s_ref_gl_plugin_load_failed = false;
static qboolean s_renderer_plugins_scanned = false;
static int s_missing_resource_warn_frame[R_BACKEND_RESOURCE_SLOT_COUNT];
static void *s_plugin_libs[R_BACKEND_MAX_PLUGIN_LIBS];
static int s_plugin_lib_count = 0;
static int s_renderer_plugin_search_dir_count = 0;
static char s_renderer_plugin_search_dirs[3][MAX_OSPATH];
static qboolean s_command_encoder_recording = false;
static int s_command_encoder_frame = -1;
static RenderGraphResourceHandle s_last_populated_resources;
static qboolean s_last_populated_resources_valid = false;
static int s_last_populated_resources_frame = -1;
static int s_warned_missing_descriptor_binding_support_frame = -1;
static int s_warned_missing_bind_pipeline_support_frame = -1;
static int s_warned_missing_pipeline_state_support_frame = -1;
static int s_warned_missing_dynamic_state_support_frame = -1;

enum
{
	R_BACKEND_MAX_EXTERNAL_RESOURCES = 1024,
	R_BACKEND_MAX_PIPELINE_METADATA = 512
};

typedef struct r_backend_external_resource_entry_s
{
	qboolean in_use;
	unsigned int assigned_resource_id;
	unsigned int producer_epoch;
	unsigned int last_touched_epoch;
	iw_renderer_host_resource_handle_t handle;
} r_backend_external_resource_entry_t;

typedef struct r_backend_pipeline_metadata_entry_s
{
	qboolean in_use;
	unsigned int pipeline_id;
	unsigned int state_bits;
	unsigned int shader_count;
	unsigned int shader_ids[8];
	char debug_name[96];
} r_backend_pipeline_metadata_entry_t;

static r_backend_external_resource_entry_t s_external_resource_registry[R_BACKEND_MAX_EXTERNAL_RESOURCES];
static r_backend_pipeline_metadata_entry_t s_pipeline_metadata_registry[R_BACKEND_MAX_PIPELINE_METADATA];
static unsigned int s_upload_transient_epoch = 1u;
static unsigned int s_upload_completed_epoch = 0u;
static int s_upload_last_begin_frame = -1;

static qboolean R_Backend_Host_GetSurfaceInfo (iw_renderer_host_surface_info_t *out_info);
static qboolean R_Backend_Host_ResolveResourceBySlot (render_backend_resource_slot_t slot, iw_renderer_host_resource_handle_t *out_handle);
static qboolean R_Backend_Host_ResolveResourceByRef (const render_backend_resource_ref_t *ref, iw_renderer_host_resource_handle_t *out_handle);
static qboolean R_Backend_Host_RegisterExternalResource (const iw_renderer_host_resource_handle_t *resource, unsigned int *out_resource_id);
static qboolean R_Backend_Host_QueryUploadEpoch (iw_renderer_host_upload_epoch_t *out_epoch);
static qboolean R_Backend_Host_IsTransientResourceAlive (unsigned int resource_id, unsigned int producer_epoch);
static qboolean R_Backend_Host_GetShaderMetadata (unsigned int shader_id, iw_renderer_host_shader_metadata_t *out_metadata);
static qboolean R_Backend_Host_GetPipelineMetadata (unsigned int pipeline_id, iw_renderer_host_pipeline_metadata_t *out_metadata);
static qboolean R_Backend_ValidatePluginHostApi (const iw_renderer_plugin_host_api_t *host_api, qboolean emit_warning);
static qboolean R_VulkanStub_Init (void);
static qboolean R_VulkanStub_HasRequiredPassCallbacks (void);
static void R_VulkanStub_Present (void);
static unsigned R_VulkanStub_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource);
static qboolean R_VulkanStub_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource);

static const iw_renderer_plugin_surface_services_t s_plugin_surface_services = {
	sizeof (iw_renderer_plugin_surface_services_t),
	R_Backend_Host_GetSurfaceInfo
};

static const iw_renderer_plugin_resource_services_t s_plugin_resource_services = {
	sizeof (iw_renderer_plugin_resource_services_t),
	R_Backend_Host_ResolveResourceBySlot,
	R_Backend_Host_ResolveResourceByRef,
	R_Backend_Host_RegisterExternalResource
};

static const iw_renderer_plugin_upload_services_t s_plugin_upload_services = {
	sizeof (iw_renderer_plugin_upload_services_t),
	R_Backend_Host_QueryUploadEpoch,
	R_Backend_Host_IsTransientResourceAlive
};

static const iw_renderer_plugin_pipeline_services_t s_plugin_pipeline_services = {
	sizeof (iw_renderer_plugin_pipeline_services_t),
	R_Backend_Host_GetShaderMetadata,
	R_Backend_Host_GetPipelineMetadata
};

cvar_t r_backend = { "r_backend", "ref_gl", CVAR_ARCHIVE };
cvar_t r_backend_api = { "r_backend_api", "ref_gl", CVAR_ARCHIVE };
extern cvar_t r_refgl_debug;

extern cvar_t r_refgl_log_init;
extern cvar_t r_refgl_log_passes;
extern cvar_t r_refgl_log_resources;
extern cvar_t r_refgl_log_state;
extern cvar_t r_refgl_validate_state;
extern cvar_t r_refgl_validate_fbo;
extern cvar_t r_refgl_validate_lifetime;
extern cvar_t r_ref_enable_postfx;
extern cvar_t r_ref_enable_shadows;
extern cvar_t r_ref_enable_fog;
extern cvar_t r_ref_enable_lighting;

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
	Con_Printf ("  P0: explicit bind_pipeline + set_dynamic_state migration complete in current draw paths\n");
	Con_Printf ("  P1: direct glDraw*/GL_Draw* draw-path migration complete (dedicated backend files only; non-draw utility calls like glDrawPixels remain explicitly scoped)\n");
	Con_Printf ("  P2: bind-time texture/program glue in legacy passes (move to descriptor sets + explicit pipeline binding)\n");
	Con_Printf ("  P3: optional compute/dispatch paths (already abstracted via R_Backend_Dispatch where available)\n");
	Con_Printf ("  note: internal builtin OpenGL registration path is obsolete; external ref_gl renderer is the default runtime path.\n");
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
	const qboolean has_required_pass_callbacks =
		backend
		&& backend->begin_pass_ex
		&& backend->end_pass_ex
		&& backend->pass_setup_view
		&& backend->pass_shadowmaps
		&& backend->pass_render_scene
		&& backend->pass_warp_resolve
		&& backend->pass_postprocess
		&& backend->pass_overlay_viewmodel
		&& backend->pass_overlay_polyblend;

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

	if (backend->has_required_pass_callbacks
		&& !backend->has_required_pass_callbacks ())
	{
		if (emit_warning)
		{
			Con_Warning ("Renderer backend '%s' does not advertise required pass callback support.\n",
				backend->name ? backend->name : "<unnamed>");
		}
		SDL_assert (!"Renderer backend pass callback contract violation");
		return false;
	}

	if (!has_required_pass_callbacks)
	{
		if (emit_warning)
		{
			Con_Warning ("Renderer backend '%s' is missing required render-pass callbacks (begin/end pass ex + pass callbacks).\n",
				backend->name ? backend->name : "<unnamed>");
		}
		SDL_assert (!"Renderer backend required pass callback missing");
		return false;
	}

	return true;
}

static void R_Backend_ClearActiveCaps (void)
{
	memset (&s_active_backend_caps, 0, sizeof (s_active_backend_caps));
}

const char *R_Backend_ResourceSlotName (render_backend_resource_slot_t slot)
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

typedef struct r_backend_framegraph_resource_binding_s
{
	unsigned bit;
	render_backend_resource_slot_t slot;
	qboolean requires_backend_resource;
} r_backend_framegraph_resource_binding_t;

static const r_backend_framegraph_resource_binding_t s_framegraph_resource_bindings[] = {
	{ RENDER_RES_SCENE_COLOR, R_BACKEND_RESOURCE_SLOT_SCENE_COLOR, true },
	{ RENDER_RES_SCENE_DEPTH, R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH, true },
	{ RENDER_RES_COMPOSITE_COLOR, R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR, true },
	{ RENDER_RES_COMPOSITE_DEPTH, R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH, true },
	{ RENDER_RES_SHADOW_SUN_DEPTH, R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH, true },
	{ RENDER_RES_VELOCITY, R_BACKEND_RESOURCE_SLOT_VELOCITY, true },
	{ RENDER_RES_DECALS, R_BACKEND_RESOURCE_SLOT_NONE, false },
	{ RENDER_RES_SSAO_FOG_STATE, R_BACKEND_RESOURCE_SLOT_NONE, false }
};

qboolean R_Backend_GetFrameGraphResourceBinding (unsigned resource_bit, render_backend_resource_slot_t *out_slot, qboolean *out_requires_backend_resource)
{
	unsigned i;

	for (i = 0; i < (unsigned)Q_COUNTOF (s_framegraph_resource_bindings); ++i)
	{
		const r_backend_framegraph_resource_binding_t *binding = &s_framegraph_resource_bindings[i];
		if (binding->bit != resource_bit)
			continue;
		if (out_slot)
			*out_slot = binding->slot;
		if (out_requires_backend_resource)
			*out_requires_backend_resource = binding->requires_backend_resource;
		return true;
	}

	if (out_slot)
		*out_slot = R_BACKEND_RESOURCE_SLOT_NONE;
	if (out_requires_backend_resource)
		*out_requires_backend_resource = false;
	return false;
}

static const struct render_graph_backend_resource_entry_s *R_Backend_FindResourceEntryById (const RenderGraphResourceHandle *resources, unsigned resource_id)
{
	unsigned i;

	if (!resources || resource_id == 0u)
		return NULL;

	for (i = 0; i < (unsigned)resources->registry_count; ++i)
	{
		const struct render_graph_backend_resource_entry_s *entry = &resources->registry[i];
		if (entry->resource_id == resource_id)
			return entry;
	}

	return NULL;
}

static qboolean R_Backend_FillHostResourceHandleFromRef (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource_ref, iw_renderer_host_resource_handle_t *out_handle)
{
	const struct render_graph_backend_resource_entry_s *entry;
	unsigned resource_id;

	if (!resources || !resource_ref || !out_handle)
		return false;

	resource_id = resource_ref->opaque_id;
	if (resource_id == 0u)
		return false;
	entry = R_Backend_FindResourceEntryById (resources, resource_id);
	if (!entry)
		return false;

	memset (out_handle, 0, sizeof (*out_handle));
	out_handle->struct_size = sizeof (*out_handle);
	out_handle->resource_id = entry->resource_id;
	out_handle->native_id = entry->native_id;
	out_handle->type = entry->type;
	out_handle->lifetime = entry->lifetime;
	out_handle->slot = entry->slot;
	return true;
}

static void R_Backend_ClearExternalResourceRegistry (void)
{
	memset (s_external_resource_registry, 0, sizeof (s_external_resource_registry));
	s_upload_transient_epoch = 1u;
	s_upload_completed_epoch = 0u;
	s_upload_last_begin_frame = -1;
}

static qboolean R_Backend_ExternalResourceHandleEquals (const iw_renderer_host_resource_handle_t *lhs, const iw_renderer_host_resource_handle_t *rhs)
{
	if (!lhs || !rhs)
		return false;

	if (lhs->resource_id != 0u && rhs->resource_id != 0u)
		return lhs->resource_id == rhs->resource_id;

	return lhs->native_id == rhs->native_id
		&& lhs->type == rhs->type
		&& lhs->lifetime == rhs->lifetime
		&& lhs->slot == rhs->slot;
}

static r_backend_external_resource_entry_t *R_Backend_FindExternalResourceEntry (const iw_renderer_host_resource_handle_t *handle)
{
	unsigned i;

	if (!handle)
		return NULL;

	for (i = 0; i < (unsigned)R_BACKEND_MAX_EXTERNAL_RESOURCES; ++i)
	{
		r_backend_external_resource_entry_t *entry = &s_external_resource_registry[i];
		if (!entry->in_use)
			continue;
		if (R_Backend_ExternalResourceHandleEquals (&entry->handle, handle))
			return entry;
	}

	return NULL;
}

static r_backend_external_resource_entry_t *R_Backend_AllocExternalResourceEntry (void)
{
	unsigned i;

	for (i = 0; i < (unsigned)R_BACKEND_MAX_EXTERNAL_RESOURCES; ++i)
	{
		if (!s_external_resource_registry[i].in_use)
			return &s_external_resource_registry[i];
	}

	return NULL;
}

static r_backend_external_resource_entry_t *R_Backend_FindExternalResourceEntryByAssignedId (unsigned int resource_id)
{
	unsigned i;

	if (resource_id == 0u)
		return NULL;

	for (i = 0; i < (unsigned)R_BACKEND_MAX_EXTERNAL_RESOURCES; ++i)
	{
		r_backend_external_resource_entry_t *entry = &s_external_resource_registry[i];
		if (!entry->in_use)
			continue;
		if (entry->assigned_resource_id == resource_id)
			return entry;
	}

	return NULL;
}

static void R_Backend_ReapTransientExternalResources (void)
{
	unsigned i;

	for (i = 0; i < (unsigned)R_BACKEND_MAX_EXTERNAL_RESOURCES; ++i)
	{
		r_backend_external_resource_entry_t *entry = &s_external_resource_registry[i];
		if (!entry->in_use)
			continue;
		if (entry->handle.lifetime != R_BACKEND_RESOURCE_LIFETIME_FRAME)
			continue;
		if (entry->last_touched_epoch > s_upload_completed_epoch)
			continue;

		memset (entry, 0, sizeof (*entry));
	}
}

static void R_Backend_ClearPipelineMetadataRegistry (void)
{
	memset (s_pipeline_metadata_registry, 0, sizeof (s_pipeline_metadata_registry));
}

static r_backend_pipeline_metadata_entry_t *R_Backend_FindPipelineMetadataEntry (unsigned int pipeline_id)
{
	unsigned i;

	if (pipeline_id == 0u)
		return NULL;

	for (i = 0; i < (unsigned)R_BACKEND_MAX_PIPELINE_METADATA; ++i)
	{
		r_backend_pipeline_metadata_entry_t *entry = &s_pipeline_metadata_registry[i];
		if (!entry->in_use)
			continue;
		if (entry->pipeline_id == pipeline_id)
			return entry;
	}

	return NULL;
}

static r_backend_pipeline_metadata_entry_t *R_Backend_AllocPipelineMetadataEntry (void)
{
	unsigned i;

	for (i = 0; i < (unsigned)R_BACKEND_MAX_PIPELINE_METADATA; ++i)
	{
		if (!s_pipeline_metadata_registry[i].in_use)
			return &s_pipeline_metadata_registry[i];
	}

	return NULL;
}

static void R_Backend_RecordPipelineMetadata (const RenderBackendPipelineDesc *pipeline)
{
	r_backend_pipeline_metadata_entry_t *entry;
	unsigned shader_id;
	const char *shader_name = NULL;
	const char *shader_entry = NULL;
	const char *shader_stage = NULL;
	unsigned permutation_key = 0u;
	char fallback_name[96];

	if (!pipeline || pipeline->pipeline_id == 0u)
		return;

	entry = R_Backend_FindPipelineMetadataEntry (pipeline->pipeline_id);
	if (!entry)
		entry = R_Backend_AllocPipelineMetadataEntry ();
	if (!entry)
		return;

	shader_id = (unsigned)GL_GetCurrentProgram ();
	memset (entry, 0, sizeof (*entry));
	entry->in_use = true;
	entry->pipeline_id = pipeline->pipeline_id;
	entry->state_bits = pipeline->state_bits;
	if (shader_id != 0u)
	{
		entry->shader_count = 1u;
		entry->shader_ids[0] = shader_id;
	}

	if (shader_id != 0u && GL_QueryProgramMetadata ((GLuint)shader_id, &shader_name, &shader_entry, &shader_stage, &permutation_key))
	{
		(void)shader_entry;
		(void)shader_stage;
		if (permutation_key != 0u)
			q_snprintf (entry->debug_name, sizeof (entry->debug_name), "pipeline %u|%s|perm %u", pipeline->pipeline_id, shader_name, permutation_key);
		else
			q_snprintf (entry->debug_name, sizeof (entry->debug_name), "pipeline %u|%s", pipeline->pipeline_id, shader_name);
	}
	else
	{
		q_snprintf (fallback_name, sizeof (fallback_name), "pipeline %u|state 0x%08x", pipeline->pipeline_id, pipeline->state_bits);
		q_strlcpy (entry->debug_name, fallback_name, sizeof (entry->debug_name));
	}
}

void R_Backend_QuerySurfaceInfo (RenderBackendSurfaceInfo *out_info)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	RenderBackendSurfaceMetrics metrics;
	qboolean has_metrics = false;

	if (!out_info)
		return;

	memset (out_info, 0, sizeof (*out_info));
	memset (&metrics, 0, sizeof (metrics));
	if (backend && backend->query_surface_metrics)
		has_metrics = backend->query_surface_metrics (&metrics);

	if (has_metrics)
	{
		out_info->surface_x = metrics.surface_x;
		out_info->surface_y = metrics.surface_y;
		out_info->surface_width = q_max (1, metrics.surface_width);
		out_info->surface_height = q_max (1, metrics.surface_height);
		out_info->view_x = metrics.view_x;
		out_info->view_y = metrics.view_y;
		out_info->view_width = q_max (1, metrics.view_width);
		out_info->view_height = q_max (1, metrics.view_height);
		out_info->scene_width = q_max (1, metrics.scene_width);
		out_info->scene_height = q_max (1, metrics.scene_height);
	}
	else
	{
		out_info->surface_x = 0;
		out_info->surface_y = 0;
		out_info->surface_width = q_max (1, vid.width);
		out_info->surface_height = q_max (1, vid.height);
		out_info->view_x = out_info->surface_x + r_refdef.vrect.x;
		out_info->view_y = out_info->surface_y + out_info->surface_height - r_refdef.vrect.y - r_refdef.vrect.height;
		out_info->view_width = q_max (1, r_refdef.vrect.width);
		out_info->view_height = q_max (1, r_refdef.vrect.height);
		out_info->scene_width = out_info->view_width;
		out_info->scene_height = out_info->view_height;
	}

	out_info->scene_samples = (unsigned)q_max (1, R_Backend_GetSceneSampleCount ());
	out_info->frame_index = (unsigned)q_max (0, r_framecount);
	out_info->needs_scene_effects = (backend && backend->needs_scene_effects) ? backend->needs_scene_effects () : false;
	out_info->needs_postprocess = (backend && backend->needs_postprocess) ? backend->needs_postprocess () : false;
}

static void R_Backend_ValidateFrameGraphResourceRegistry (const RenderGraphResourceHandle *resources)
{
	int slot;
	unsigned i;

	if (!resources)
		return;

	for (slot = R_BACKEND_RESOURCE_SLOT_NONE + 1; slot < R_BACKEND_RESOURCE_SLOT_COUNT; ++slot)
	{
		const render_backend_resource_ref_t *resource_ref = &resources->refs[slot];
		unsigned slot_resource_id = resources->slot_resource_ids[slot];
		const struct render_graph_backend_resource_entry_s *entry = NULL;

		if (resource_ref->opaque_id == 0u && slot_resource_id == 0u)
			continue;

		if (resource_ref->opaque_id != slot_resource_id)
		{
			Con_DWarning ("FrameGraph resource registry mismatch: slot '%s' ref_id=%u slot_id=%u\n",
				R_Backend_ResourceSlotName ((render_backend_resource_slot_t)slot),
				(unsigned)resource_ref->opaque_id,
				slot_resource_id);
			SDL_assert (!"FrameGraph resource slot id mismatch");
			continue;
		}

		entry = R_Backend_FindResourceEntryById (resources, slot_resource_id);
		if (!entry)
		{
			Con_DWarning ("FrameGraph resource registry missing entry for slot '%s' (id=%u)\n",
				R_Backend_ResourceSlotName ((render_backend_resource_slot_t)slot),
				slot_resource_id);
			SDL_assert (!"FrameGraph resource id missing from registry");
			continue;
		}

		if (entry->slot != (unsigned short)slot || entry->type != resource_ref->type)
		{
			Con_DWarning ("FrameGraph resource registry invalid mapping for slot '%s' (entry_slot=%u entry_type=%u ref_type=%u)\n",
				R_Backend_ResourceSlotName ((render_backend_resource_slot_t)slot),
				(unsigned)entry->slot,
				(unsigned)entry->type,
				(unsigned)resource_ref->type);
			SDL_assert (!"FrameGraph resource registry entry does not match slot/ref");
		}
	}

	for (i = 0; i < (unsigned)resources->registry_count; ++i)
	{
		const struct render_graph_backend_resource_entry_s *entry = &resources->registry[i];
		if (entry->resource_id == 0u)
		{
			Con_DWarning ("FrameGraph resource registry[%u] has invalid resource_id=0\n", i);
			SDL_assert (!"FrameGraph resource registry entry has id 0");
		}
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

static const char *R_Backend_ApiToCanonicalName (const char *api_name);
static qboolean R_Backend_IsExternalRendererRequest (const char *name);
static void R_Backend_LoadRendererPlugins (void);

static const IRenderBackend *R_Backend_FindByName (const char *backend_name)
{
	int i;
	const char *resolved_name = R_Backend_ApiToCanonicalName (backend_name);

	if (!resolved_name || !resolved_name[0])
		return NULL;

	for (i = 0; i < s_registered_backend_count; ++i)
	{
		const IRenderBackend *backend = s_registered_backends[i];
		if (backend && backend->name && !q_strcasecmp (backend->name, resolved_name))
			return backend;
	}

	return NULL;
}

static qboolean R_Backend_HasRegisteredName (const char *backend_name)
{
	return R_Backend_FindByName (backend_name) != NULL;
}

static const char *R_Backend_ApiToCanonicalName (const char *api_name);
static const char *R_Backend_ApiToCanonicalName (const char *api_name)
{
	if (!api_name || !api_name[0])
		return NULL;

	if (!q_strcasecmp (api_name, "gl") || !q_strcasecmp (api_name, "opengl"))
		return "OpenGL";
	if (!q_strcasecmp (api_name, "ref_gl") || !q_strcasecmp (api_name, s_ref_gl_plugin_filename))
		return "OpenGL";
	if (!q_strcasecmp (api_name, "vk") || !q_strcasecmp (api_name, "vulkan"))
		return "Vulkan";
	if (!q_strcasecmp (api_name, "ref_vk") || !q_strcasecmp (api_name, s_ref_vk_plugin_filename))
		return "Vulkan";
	if (!q_strcasecmp (api_name, "dx12") || !q_strcasecmp (api_name, "d3d12"))
		return "DX12";
	if (!q_strcasecmp (api_name, "ref_dx12") || !q_strcasecmp (api_name, s_ref_dx12_plugin_filename))
		return "DX12";

	return api_name;
}

static qboolean R_Backend_IsExternalRendererRequest (const char *name)
{
	if (!name || !name[0])
		return false;

	return !q_strcasecmp (name, "ref_gl")
		|| !q_strcasecmp (name, s_ref_gl_plugin_filename)
		|| !q_strcasecmp (name, "ref_vk")
		|| !q_strcasecmp (name, s_ref_vk_plugin_filename)
		|| !q_strcasecmp (name, "ref_dx12")
		|| !q_strcasecmp (name, s_ref_dx12_plugin_filename);
}

static const char *R_Backend_CanonicalNameToApi (const char *backend_name)
{
	if (!backend_name || !backend_name[0])
		return "gl";

	if (!q_strcasecmp (backend_name, "OpenGL"))
		return "gl";
	if (!q_strcasecmp (backend_name, "Vulkan"))
		return "vulkan";
	if (!q_strcasecmp (backend_name, "DX12"))
		return "dx12";

	return backend_name;
}

static render_backend_runtime_status_t R_Backend_GetRuntimeStatusForBackend (const IRenderBackend *backend)
{
	if (!backend)
		return R_BACKEND_RUNTIME_STUB;

	if ((backend->init == R_VulkanStub_Init
		&& backend->present == R_VulkanStub_Present
		&& backend->resolve_resource_id == R_VulkanStub_ResolveResourceId
		&& backend->is_resource_valid == R_VulkanStub_IsResourceValid)
		&& (backend->name && (!q_strcasecmp (backend->name, "Vulkan") || !q_strcasecmp (backend->name, "DX12"))))
		return R_BACKEND_RUNTIME_STUB;

	if (backend->name && !q_strcasecmp (backend->name, "OpenGL"))
		return R_BACKEND_RUNTIME_IMPLEMENTED;

	return R_BACKEND_RUNTIME_EXPERIMENTAL;
}

const char *R_Backend_GetRuntimeStatusLabel (render_backend_runtime_status_t status)
{
	switch (status)
	{
	case R_BACKEND_RUNTIME_IMPLEMENTED: return "implemented";
	case R_BACKEND_RUNTIME_EXPERIMENTAL: return "experimental";
	case R_BACKEND_RUNTIME_STUB: return "stub";
	default: return "unknown";
	}
}

qboolean R_Backend_GetMilestonesForName (const char *backend_name, RenderBackendMilestones *out_milestones)
{
	const IRenderBackend *backend = R_Backend_FindByName (backend_name);

	if (!out_milestones || !backend)
		return false;

	memset (out_milestones, 0, sizeof (*out_milestones));
	out_milestones->init_ready = (backend->init != NULL && backend->init != R_VulkanStub_Init);
	out_milestones->pass_callbacks_ready = (backend->has_required_pass_callbacks != NULL
		&& backend->has_required_pass_callbacks != R_VulkanStub_HasRequiredPassCallbacks);
	out_milestones->present_ready = (backend->present != NULL && backend->present != R_VulkanStub_Present);
	out_milestones->resource_translation_ready = (backend->resolve_resource_id != NULL
		&& backend->is_resource_valid != NULL
		&& backend->resolve_resource_id != R_VulkanStub_ResolveResourceId
		&& backend->is_resource_valid != R_VulkanStub_IsResourceValid);

	if (backend->name && !q_strcasecmp (backend->name, "OpenGL"))
	{
		out_milestones->init_ready = true;
		out_milestones->pass_callbacks_ready = true;
		out_milestones->present_ready = true;
		out_milestones->resource_translation_ready = true;
	}

	return true;
}

render_backend_runtime_status_t R_Backend_GetRuntimeStatusForName (const char *backend_name)
{
	return R_Backend_GetRuntimeStatusForBackend (R_Backend_FindByName (backend_name));
}

static void R_Backend_PrintApiHelp (void)
{
	const IRenderBackend *gl_backend = R_Backend_FindByName ("OpenGL");
	render_backend_runtime_status_t gl_status = R_BACKEND_RUNTIME_STUB;

	if (gl_backend)
		gl_status = R_Backend_GetRuntimeStatusForBackend (gl_backend);

	Con_Printf ("r_backend_api help: gl=%s, vulkan=%s, dx12=%s\n",
		R_Backend_GetRuntimeStatusLabel (gl_status),
		R_Backend_GetRuntimeStatusLabel (R_Backend_GetRuntimeStatusForName ("Vulkan")),
		R_Backend_GetRuntimeStatusLabel (R_Backend_GetRuntimeStatusForName ("DX12")));
	Con_Printf ("  gl: production path.\n");
	Con_Printf ("  vulkan/dx12: bring-up paths; see *_status commands for milestone detail.\n");
}

static qboolean R_Backend_RegisterViaHostApi (const IRenderBackend *backend);
static void R_Backend_FillPluginHostApi (iw_renderer_plugin_host_api_t *host_api);
void R_Backend_FillHostBridge (iw_renderer_host_bridge_t *out);

static qboolean R_Backend_RegisterEntryPoints (const iw_renderer_entry_points_t *entry_points)
{
#define IW_REQUIRE_RENDER_ENTRY(fn_name) \
	do \
	{ \
		if (!entry_points->fn_name) \
		{ \
			Con_Warning ("Renderer plugin entry-point table is missing required callback '%s'.\n", #fn_name); \
			return false; \
		} \
	} while (0)

	if (!entry_points || entry_points->struct_size < sizeof (*entry_points))
	{
		Con_Warning ("Renderer plugin entry-point table is missing or invalid.\n");
		return false;
	}

	IW_REQUIRE_RENDER_ENTRY (R_Init);
	IW_REQUIRE_RENDER_ENTRY (R_RenderView);
	IW_REQUIRE_RENDER_ENTRY (R_NewMap);
	IW_REQUIRE_RENDER_ENTRY (R_ClearEfrags);
	IW_REQUIRE_RENDER_ENTRY (R_CheckEfrags);
	IW_REQUIRE_RENDER_ENTRY (R_AddEfrags);
	IW_REQUIRE_RENDER_ENTRY (R_ParseParticleEffect);
	IW_REQUIRE_RENDER_ENTRY (R_RunParticleEffect);
	IW_REQUIRE_RENDER_ENTRY (R_RocketTrail);
	IW_REQUIRE_RENDER_ENTRY (R_EntityParticles);
	IW_REQUIRE_RENDER_ENTRY (R_BlobExplosion);
	IW_REQUIRE_RENDER_ENTRY (R_ParticleExplosion);
	IW_REQUIRE_RENDER_ENTRY (R_ParticleExplosion2);
	IW_REQUIRE_RENDER_ENTRY (R_LavaSplash);
	IW_REQUIRE_RENDER_ENTRY (R_TeleportSplash);
	IW_REQUIRE_RENDER_ENTRY (R_SpawnImpactDecal);
	IW_REQUIRE_RENDER_ENTRY (R_SpawnImpactDecalEx);
	IW_REQUIRE_RENDER_ENTRY (R_TranslatePlayerSkin);
	IW_REQUIRE_RENDER_ENTRY (R_TranslateNewPlayerSkin);
	IW_REQUIRE_RENDER_ENTRY (R_ClearBoundingBoxes);
	IW_REQUIRE_RENDER_ENTRY (R_ClearParticles);
	IW_REQUIRE_RENDER_ENTRY (R_ClearDecals);
	IW_REQUIRE_RENDER_ENTRY (R_ReloadDecals);
	IW_REQUIRE_RENDER_ENTRY (R_InitDecals);
	IW_REQUIRE_RENDER_ENTRY (R_StorePrevFrameState);
	IW_REQUIRE_RENDER_ENTRY (R_GetParticleDebugStats);
	IW_REQUIRE_RENDER_ENTRY (R_SetAlphaMode);
	IW_REQUIRE_RENDER_ENTRY (R_GetAlphaMode);
	IW_REQUIRE_RENDER_ENTRY (R_GetEffectiveAlphaMode);
	IW_REQUIRE_RENDER_ENTRY (R_AddStaticModels);
	IW_REQUIRE_RENDER_ENTRY (R_PushDlights);
	IW_REQUIRE_RENDER_ENTRY (R_ParseDlightEntities);
	IW_REQUIRE_RENDER_ENTRY (R_GetLightgridSample);
	IW_REQUIRE_RENDER_ENTRY (R_DrawPolyblendOverlay);
	IW_REQUIRE_RENDER_ENTRY (R_GetCanvasMetrics);
	IW_REQUIRE_RENDER_ENTRY (R_GetSceneSampleCount);
	IW_REQUIRE_RENDER_ENTRY (R_GetMaxSampleCount);
	IW_REQUIRE_RENDER_ENTRY (R_GetMaxAnisotropy);
	IW_REQUIRE_RENDER_ENTRY (R_IsClearEnabled);
	IW_REQUIRE_RENDER_ENTRY (R_NewGame);
	IW_REQUIRE_RENDER_ENTRY (R_CreateFrameBuffers);
	IW_REQUIRE_RENDER_ENTRY (R_DeleteFrameBuffers);
	IW_REQUIRE_RENDER_ENTRY (R_ResetDRSState);
	IW_REQUIRE_RENDER_ENTRY (R_ResetGodraysStabilization);
	/* Host keeps ownership of screen/UI orchestration; plugin callback is optional. */
	RenderDispatch_SetEntryPoints (entry_points);
#undef IW_REQUIRE_RENDER_ENTRY
	return true;
}

static qboolean R_Backend_Host_GetSurfaceInfo (iw_renderer_host_surface_info_t *out_info)
{
	RenderBackendSurfaceInfo surface_info;

	if (!out_info || out_info->struct_size < sizeof (*out_info))
		return false;

	R_Backend_QuerySurfaceInfo (&surface_info);
	memset (out_info, 0, sizeof (*out_info));
	out_info->struct_size = sizeof (*out_info);
	out_info->surface_x = surface_info.surface_x;
	out_info->surface_y = surface_info.surface_y;
	out_info->surface_width = surface_info.surface_width;
	out_info->surface_height = surface_info.surface_height;
	out_info->view_x = surface_info.view_x;
	out_info->view_y = surface_info.view_y;
	out_info->view_width = surface_info.view_width;
	out_info->view_height = surface_info.view_height;
	out_info->scene_width = surface_info.scene_width;
	out_info->scene_height = surface_info.scene_height;
	out_info->scene_samples = surface_info.scene_samples;
	out_info->frame_index = surface_info.frame_index;
	out_info->surface_origin = IW_RENDERER_SURFACE_ORIGIN_LOWER_LEFT;
	out_info->needs_scene_effects = surface_info.needs_scene_effects;
	out_info->needs_postprocess = surface_info.needs_postprocess;
	return true;
}

static qboolean R_Backend_Host_ResolveResourceBySlot (render_backend_resource_slot_t slot, iw_renderer_host_resource_handle_t *out_handle)
{
	const render_backend_resource_ref_t *resource_ref;

	if (!s_last_populated_resources_valid)
		return false;
	if (s_last_populated_resources_frame != r_framecount)
		return false;
	if (slot <= R_BACKEND_RESOURCE_SLOT_NONE || slot >= R_BACKEND_RESOURCE_SLOT_COUNT)
		return false;

	resource_ref = &s_last_populated_resources.refs[slot];
	return R_Backend_FillHostResourceHandleFromRef (&s_last_populated_resources, resource_ref, out_handle);
}

static qboolean R_Backend_Host_ResolveResourceByRef (const render_backend_resource_ref_t *ref, iw_renderer_host_resource_handle_t *out_handle)
{
	if (!s_last_populated_resources_valid)
		return false;
	if (s_last_populated_resources_frame != r_framecount)
		return false;
	return R_Backend_FillHostResourceHandleFromRef (&s_last_populated_resources, ref, out_handle);
}

static qboolean R_Backend_Host_RegisterExternalResource (const iw_renderer_host_resource_handle_t *resource, unsigned int *out_resource_id)
{
	r_backend_external_resource_entry_t *entry;
	iw_renderer_host_resource_handle_t normalized;
	unsigned slot_index;

	if (!resource || resource->struct_size < sizeof (*resource))
	{
		if (out_resource_id)
			*out_resource_id = 0u;
		return false;
	}

	normalized = *resource;
	normalized.struct_size = sizeof (normalized);

	entry = R_Backend_FindExternalResourceEntry (&normalized);
	if (entry)
	{
		entry->handle = normalized;
		entry->last_touched_epoch = s_upload_transient_epoch;
		if (normalized.lifetime == R_BACKEND_RESOURCE_LIFETIME_FRAME)
			entry->producer_epoch = s_upload_transient_epoch;
		else
			entry->producer_epoch = 0u;
		if (out_resource_id)
			*out_resource_id = entry->assigned_resource_id;
		return true;
	}

	entry = R_Backend_AllocExternalResourceEntry ();
	if (!entry)
	{
		if (out_resource_id)
			*out_resource_id = 0u;
		Con_DWarning ("Renderer plugin host API: external resource registry capacity reached (%d).\n",
			R_BACKEND_MAX_EXTERNAL_RESOURCES);
		return false;
	}

	memset (entry, 0, sizeof (*entry));
	entry->in_use = true;
	entry->handle = normalized;
	entry->last_touched_epoch = s_upload_transient_epoch;
	slot_index = (unsigned)(entry - s_external_resource_registry);
	if (normalized.resource_id != 0u)
		entry->assigned_resource_id = normalized.resource_id;
	else
		entry->assigned_resource_id = 0x40000000u + slot_index + 1u;
	if (normalized.lifetime == R_BACKEND_RESOURCE_LIFETIME_FRAME)
		entry->producer_epoch = s_upload_transient_epoch;

	if (out_resource_id)
		*out_resource_id = entry->assigned_resource_id;
	return true;
}

static qboolean R_Backend_Host_QueryUploadEpoch (iw_renderer_host_upload_epoch_t *out_epoch)
{
	if (!out_epoch || out_epoch->struct_size < sizeof (*out_epoch))
		return false;

	memset (out_epoch, 0, sizeof (*out_epoch));
	out_epoch->struct_size = sizeof (*out_epoch);
	out_epoch->frame_index = (unsigned)q_max (0, r_framecount);
	out_epoch->transient_epoch = s_upload_transient_epoch;
	out_epoch->completed_epoch = s_upload_completed_epoch;
	return true;
}

static qboolean R_Backend_Host_IsTransientResourceAlive (unsigned int resource_id, unsigned int producer_epoch)
{
	r_backend_external_resource_entry_t *entry;

	if (producer_epoch == 0u)
		return false;

	if (resource_id != 0u)
	{
		entry = R_Backend_FindExternalResourceEntryByAssignedId (resource_id);
		if (entry)
		{
			if (entry->handle.lifetime != R_BACKEND_RESOURCE_LIFETIME_FRAME)
				return true;
			if (entry->producer_epoch != 0u && producer_epoch != entry->producer_epoch)
				return false;
			return producer_epoch > s_upload_completed_epoch;
		}
	}

	return producer_epoch > s_upload_completed_epoch;
}

static qboolean R_Backend_Host_GetShaderMetadata (unsigned int shader_id, iw_renderer_host_shader_metadata_t *out_metadata)
{
	const char *debug_name = NULL;
	const char *entry_point = NULL;
	const char *stage = NULL;
	unsigned permutation_key = 0u;

	if (!out_metadata || out_metadata->struct_size < sizeof (*out_metadata))
		return false;
	if (shader_id == 0u)
		return false;
	if (!GL_QueryProgramMetadata ((GLuint)shader_id, &debug_name, &entry_point, &stage, &permutation_key))
		return false;

	memset (out_metadata, 0, sizeof (*out_metadata));
	out_metadata->struct_size = sizeof (*out_metadata);
	out_metadata->shader_id = shader_id;
	out_metadata->debug_name = debug_name ? debug_name : "unknown";
	out_metadata->entry_point = entry_point ? entry_point : "main";
	out_metadata->stage = stage ? stage : "graphics";
	out_metadata->permutation_key = permutation_key;
	return true;
}

static qboolean R_Backend_Host_GetPipelineMetadata (unsigned int pipeline_id, iw_renderer_host_pipeline_metadata_t *out_metadata)
{
	r_backend_pipeline_metadata_entry_t *entry;
	unsigned i;

	if (!out_metadata || out_metadata->struct_size < sizeof (*out_metadata))
		return false;
	if (pipeline_id == 0u)
		return false;

	entry = R_Backend_FindPipelineMetadataEntry (pipeline_id);
	if (!entry)
		return false;

	memset (out_metadata, 0, sizeof (*out_metadata));
	out_metadata->struct_size = sizeof (*out_metadata);
	out_metadata->pipeline_id = entry->pipeline_id;
	out_metadata->debug_name = entry->debug_name;
	out_metadata->state_bits = entry->state_bits;
	out_metadata->shader_count = q_min (entry->shader_count, (unsigned)Q_COUNTOF (out_metadata->shader_ids));
	for (i = 0; i < out_metadata->shader_count; ++i)
		out_metadata->shader_ids[i] = entry->shader_ids[i];
	return true;
}

/*
================
R_Backend_ValidatePluginHostApi

Validates host_api wiring before passing it to plugin registration callbacks so
ABI/service contract issues are deterministic and easy to diagnose in logs.
================
*/
static qboolean R_Backend_ValidatePluginHostApi (const iw_renderer_plugin_host_api_t *host_api, qboolean emit_warning)
{
	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V4_SIZE)
	{
		if (emit_warning)
			Con_Warning ("Renderer plugin host API is invalid (missing v4 fields).\n");
		return false;
	}

	if (host_api->abi_major != IW_RENDERER_PLUGIN_ABI_MAJOR || host_api->abi_minor != IW_RENDERER_PLUGIN_ABI_MINOR)
	{
		if (emit_warning)
		{
			Con_Warning ("Renderer plugin host API version mismatch: host_api=%u.%u expected=%u.%u\n",
				host_api->abi_major,
				host_api->abi_minor,
				IW_RENDERER_PLUGIN_ABI_MAJOR,
				IW_RENDERER_PLUGIN_ABI_MINOR);
		}
		return false;
	}

	if (!host_api->register_backend)
	{
		if (emit_warning)
			Con_Warning ("Renderer plugin host API is invalid: register_backend callback missing.\n");
		return false;
	}

	if (host_api->struct_size >= IW_RENDERER_PLUGIN_HOST_API_V3_SIZE)
	{
		if (!host_api->surface_services
			|| host_api->surface_services->struct_size < sizeof (*host_api->surface_services)
			|| !host_api->surface_services->get_surface_info)
		{
			if (emit_warning)
				Con_Warning ("Renderer plugin host API v3 invalid: surface_services missing/incomplete.\n");
			return false;
		}

		if (!host_api->resource_services
			|| host_api->resource_services->struct_size < sizeof (*host_api->resource_services)
			|| !host_api->resource_services->resolve_resource_by_slot
			|| !host_api->resource_services->resolve_resource_by_ref)
		{
			if (emit_warning)
				Con_Warning ("Renderer plugin host API v3 invalid: resource_services missing/incomplete.\n");
			return false;
		}

		if (!host_api->upload_services
			|| host_api->upload_services->struct_size < sizeof (*host_api->upload_services)
			|| !host_api->upload_services->query_upload_epoch
			|| !host_api->upload_services->is_transient_resource_alive)
		{
			if (emit_warning)
				Con_Warning ("Renderer plugin host API v3 invalid: upload_services missing/incomplete.\n");
			return false;
		}

		if (!host_api->pipeline_services
			|| host_api->pipeline_services->struct_size < sizeof (*host_api->pipeline_services)
			|| !host_api->pipeline_services->get_shader_metadata
			|| !host_api->pipeline_services->get_pipeline_metadata)
		{
			if (emit_warning)
				Con_Warning ("Renderer plugin host API v3 invalid: pipeline_services missing/incomplete.\n");
			return false;
		}
	}

	return true;
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
	host_api->surface_services = &s_plugin_surface_services;
	host_api->resource_services = &s_plugin_resource_services;
	host_api->upload_services = &s_plugin_upload_services;
	host_api->pipeline_services = &s_plugin_pipeline_services;
	{
		static iw_renderer_host_bridge_t s_bridge;
		R_Backend_FillHostBridge (&s_bridge);
		host_api->bridge = &s_bridge;
	}
	host_api->register_entry_points = R_Backend_RegisterEntryPoints;
	SDL_assert (R_Backend_ValidatePluginHostApi (host_api, true));
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

	Con_DPrintf ("R_Backend_LoadPluginFromPath: loading '%s'\n", path);

	lib = Sys_LoadLibrary (path);
	if (!lib)
	{
		Con_Warning ("R_Backend_LoadPluginFromPath: Sys_LoadLibrary failed for '%s'\n", path);
		return false;
	}

	query_fn = (iw_renderer_plugin_query_fn) Sys_GetLibraryFunction (lib, "IW_RendererPlugin_Query");
	if (!query_fn)
	{
		Con_Warning ("Renderer plugin '%s' does not export IW_RendererPlugin_Query.\n", path);
		Sys_CloseLibrary (lib);
		return false;
	}

	descriptor = query_fn ();
	if (!descriptor || descriptor->struct_size < IW_RENDERER_PLUGIN_DESCRIPTOR_MIN_SIZE)
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
	if (!R_Backend_ValidatePluginHostApi (&host_api, true))
	{
		Sys_CloseLibrary (lib);
		return false;
	}

	{
		static iw_renderer_host_bridge_t s_bridge;
		R_Backend_FillHostBridge (&s_bridge);
		host_api.bridge = &s_bridge;
		host_api.register_entry_points = R_Backend_RegisterEntryPoints;
	}

	if (!descriptor->register_plugin (&host_api))
	{
		Con_Warning ("Renderer plugin '%s' registration callback failed.\n", plugin_name);
		Sys_CloseLibrary (lib);
		return false;
	}

	Con_Printf ("Renderer plugin loaded: %s (%s)\n", plugin_name, path);
	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("R_Backend_LoadPluginFromPath: registered plugin '%s'\n", plugin_name);
	R_Backend_RecordPluginLibrary (lib);
	return true;
}

static void R_Backend_LoadRendererPlugins (void)
{
	findfile_t *find;
	const char *search_dirs[3];
	int search_dir_count = 0;
	const char *ext_find;
	int i;
	int plugins_loaded = 0;
	int plugin_candidates = 0;
	int plugin_failed = 0;
	const qboolean plugin_debug = (r_refgl_debug.value != 0.0f || COM_CheckParm ("-refgl_debug") > 0);

	s_ref_gl_plugin_candidate_found = false;
	s_ref_gl_plugin_loaded = false;
	s_ref_gl_plugin_load_failed = false;
	s_renderer_plugins_scanned = true;
	s_renderer_plugin_search_dir_count = 0;
	memset (s_renderer_plugin_search_dirs, 0, sizeof (s_renderer_plugin_search_dirs));

#ifdef _WIN32
	ext_find = "dll";
#elif defined(__APPLE__)
	ext_find = "dylib";
#else
	ext_find = "so";
#endif

	search_dirs[search_dir_count++] = (host_parms && host_parms->exedir && host_parms->exedir[0]) ? host_parms->exedir : NULL;
	search_dirs[search_dir_count++] = (host_parms && host_parms->basedir && host_parms->basedir[0]) ? host_parms->basedir : NULL;
	if ((!search_dirs[0] || !search_dirs[0][0])
		&& (!search_dirs[1] || !search_dirs[1][0]))
	{
		search_dirs[search_dir_count++] = ".";
	}

	Con_DPrintf ("R_Backend_LoadRendererPlugins: scanning %d search directories\n", search_dir_count);

	for (i = 0; i < search_dir_count; ++i)
	{
		const char *dir = search_dirs[i];
		if (!dir || !dir[0])
			continue;
		if (i > 0 && search_dirs[0] && !q_strcasecmp (dir, search_dirs[0]))
			continue;
		if (s_renderer_plugin_search_dir_count < (int)Q_COUNTOF (s_renderer_plugin_search_dirs))
		{
			q_strlcpy (
				s_renderer_plugin_search_dirs[s_renderer_plugin_search_dir_count],
				dir,
				sizeof (s_renderer_plugin_search_dirs[s_renderer_plugin_search_dir_count]));
			++s_renderer_plugin_search_dir_count;
		}

		Con_DPrintf ("R_Backend_LoadRendererPlugins: scanning '%s' for *.%s\n", dir, ext_find);

		for (find = Sys_FindFirst (dir, ext_find); find; find = Sys_FindNext (find))
		{
			char plugin_path[MAX_OSPATH];

			if (find->attribs & FA_DIRECTORY)
				continue;
			if (q_strncasecmp (find->name, s_renderer_plugin_prefix, strlen (s_renderer_plugin_prefix)) != 0
				&& q_strcasecmp (find->name, s_ref_gl_plugin_filename) != 0
				&& q_strcasecmp (find->name, s_ref_vk_plugin_filename) != 0
				&& q_strcasecmp (find->name, s_ref_dx12_plugin_filename) != 0)
				continue;
			if ((size_t) q_snprintf (plugin_path, sizeof (plugin_path), "%s/%s", dir, find->name) >= sizeof (plugin_path))
				continue;

			++plugin_candidates;
			if (!q_strcasecmp (find->name, s_ref_gl_plugin_filename))
				s_ref_gl_plugin_candidate_found = true;
			if (R_Backend_LoadPluginFromPath (plugin_path))
			{
				++plugins_loaded;
				if (!q_strcasecmp (find->name, s_ref_gl_plugin_filename))
					s_ref_gl_plugin_loaded = true;
			}
			else
			{
				++plugin_failed;
				if (!q_strcasecmp (find->name, s_ref_gl_plugin_filename))
					s_ref_gl_plugin_load_failed = true;
			}
		}
	}

	if (plugins_loaded == 0)
	{
		if (plugin_candidates == 0)
		{
			Con_Warning ("R_Backend_LoadRendererPlugins: no renderer plugin files found.\n");
		}
		else
		{
			Con_Warning ("R_Backend_LoadRendererPlugins: found %d plugin candidate(s), but none loaded successfully.\n",
				plugin_candidates);
		}
	}
	else if (plugin_debug)
	{
		Con_Printf ("R_Backend_LoadRendererPlugins: loaded=%d failed=%d candidates=%d\n",
			plugins_loaded,
			plugin_failed,
			plugin_candidates);
	}

	/* OpenGL fallback registration is handled by R_Backend_Init() policy. */
}

static void R_Backend_ApplySelectionToCvar (void)
{
	const char *api_name;

	if (!s_active_backend || !s_active_backend->name)
		return;

	if (!(R_Backend_IsExternalRendererRequest (r_backend.string)
		&& !q_strcasecmp (R_Backend_ApiToCanonicalName (r_backend.string), s_active_backend->name)))
	{
		s_applying_backend_cvar = true;
		Cvar_SetQuick (&r_backend, s_active_backend->name);
		s_applying_backend_cvar = false;
	}

	api_name = R_Backend_CanonicalNameToApi (s_active_backend->name);
	if (!(R_Backend_IsExternalRendererRequest (r_backend_api.string)
		&& !q_strcasecmp (R_Backend_ApiToCanonicalName (r_backend_api.string), s_active_backend->name)))
	{
		s_applying_backend_api_cvar = true;
		Cvar_SetQuick (&r_backend_api, api_name);
		s_applying_backend_api_cvar = false;
	}
}

static void R_Backend_Changed_f (cvar_t *var)
{
	if (!var || s_applying_backend_cvar)
		return;

	if (!s_renderer_plugins_scanned && R_Backend_IsExternalRendererRequest (var->string))
		R_Backend_LoadRendererPlugins ();

	if (!R_Backend_Select (var->string))
	{
		Con_Warning ("Renderer backend change to '%s' rejected; keeping '%s'\n",
			var->string,
			(s_active_backend && s_active_backend->name) ? s_active_backend->name : "<none>");
		R_Backend_ApplySelectionToCvar ();
	}
}

static void R_Backend_ApiChanged_f (cvar_t *var)
{
	const char *canonical_name;
	render_backend_runtime_status_t status;

	if (!var || s_applying_backend_api_cvar)
		return;

	if (!s_renderer_plugins_scanned && R_Backend_IsExternalRendererRequest (var->string))
		R_Backend_LoadRendererPlugins ();

	canonical_name = R_Backend_ApiToCanonicalName (var->string);
	status = R_Backend_GetRuntimeStatusForName (canonical_name);
	Con_Printf ("r_backend_api '%s' -> backend '%s' (%s)\n",
		var->string,
		canonical_name ? canonical_name : "<unknown>",
		R_Backend_GetRuntimeStatusLabel (status));
	if (!R_Backend_Select (canonical_name))
	{
		Con_Warning ("Renderer backend API change to '%s' rejected; keeping '%s'\n",
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

static qboolean R_VulkanStub_ContextInit (void *window_handle) { (void)window_handle; return false; }
static void R_VulkanStub_ContextShutdown (void) {}
static void R_VulkanStub_SwapBuffers (void) {}

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
static qboolean R_VulkanStub_HasRequiredPassCallbacks (void) { return true; }
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
static qboolean R_VulkanStub_QuerySurfaceMetrics (RenderBackendSurfaceMetrics *out_metrics) { (void)out_metrics; return false; }
static qboolean R_VulkanStub_NeedsSceneEffects (void) { return false; }
static qboolean R_VulkanStub_NeedsPostprocess (void) { return false; }
static void R_VulkanStub_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles) { (void)out_handles; }
static int R_VulkanStub_GetSceneSampleCount (void) { return 1; }

static const IRenderBackend s_vulkan_stub_backend = {
	"Vulkan",
	R_VulkanStub_ContextInit,
	R_VulkanStub_ContextShutdown,
	R_VulkanStub_SwapBuffers,
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
	R_VulkanStub_HasRequiredPassCallbacks,
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
	R_VulkanStub_QuerySurfaceMetrics,
	R_VulkanStub_NeedsSceneEffects,
	R_VulkanStub_NeedsPostprocess,
	R_VulkanStub_PopulateFrameGraphResources,
	R_VulkanStub_GetSceneSampleCount
};

static void R_Backend_VulkanStatus_f (void)
{
	const IRenderBackend *backend = R_Backend_FindByName ("Vulkan");
	qboolean is_stub = (backend == &s_vulkan_stub_backend);
	qboolean can_start = false;
	qboolean can_runtime = false;

	if (backend && backend->can_activate)
	{
		can_start = backend->can_activate (false);
		can_runtime = backend->can_activate (true);
	}

	Con_Printf ("Vulkan backend status:\n");
	if (!backend)
	{
		Con_Printf ("  registration: missing (no backend named 'Vulkan' registered).\n");
		return;
	}

	Con_Printf ("  registration: %s\n", is_stub ? "stub backend registered" : "plugin/backend override registered");
	Con_Printf ("  activation gate: startup=%s runtime=%s\n", can_start ? "allowed" : "blocked", can_runtime ? "allowed" : "blocked");
	Con_Printf ("  active backend: %s\n", (s_active_backend && s_active_backend->name) ? s_active_backend->name : "<none>");
	{
		RenderBackendMilestones milestones;
		if (R_Backend_GetMilestonesForName ("Vulkan", &milestones))
		{
			Con_Printf ("  milestones: init=%s pass_callbacks=%s present=%s resource_translation=%s\n",
				milestones.init_ready ? "yes" : "no",
				milestones.pass_callbacks_ready ? "yes" : "no",
				milestones.present_ready ? "yes" : "no",
				milestones.resource_translation_ready ? "yes" : "no");
		}
	}
	if (is_stub)
	{
		Con_Printf ("  implemented callbacks: contract no-op stubs only.\n");
		Con_Printf ("  remaining work: swapchain + command buffers + pass graph execution + resource lifetime + descriptor/pipeline cache.\n");
	}
	else
	{
		Con_Printf ("  source: non-stub backend is currently registered for 'Vulkan' (likely ref_vk plugin).\n");
		Con_Printf ("  note: current ref_vk backend is an independent placeholder with activation blocked until native Vulkan implementation lands.\n");
	}
}

static const IRenderBackend s_dx12_stub_backend = {
	"DX12",
	R_VulkanStub_ContextInit,
	R_VulkanStub_ContextShutdown,
	R_VulkanStub_SwapBuffers,
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
	R_VulkanStub_HasRequiredPassCallbacks,
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
	R_VulkanStub_QuerySurfaceMetrics,
	R_VulkanStub_NeedsSceneEffects,
	R_VulkanStub_NeedsPostprocess,
	R_VulkanStub_PopulateFrameGraphResources,
	R_VulkanStub_GetSceneSampleCount
};

static void R_Backend_DX12Status_f (void)
{
	const IRenderBackend *backend = R_Backend_FindByName ("DX12");
	qboolean is_stub = (backend == &s_dx12_stub_backend);
	qboolean can_start = false;
	qboolean can_runtime = false;

	if (backend && backend->can_activate)
	{
		can_start = backend->can_activate (false);
		can_runtime = backend->can_activate (true);
	}

	Con_Printf ("DX12 backend status:\n");
	if (!backend)
	{
		Con_Printf ("  registration: missing (no backend named 'DX12' registered).\n");
		return;
	}

	Con_Printf ("  registration: %s\n", is_stub ? "stub backend registered" : "plugin/backend override registered");
	Con_Printf ("  activation gate: startup=%s runtime=%s\n", can_start ? "allowed" : "blocked", can_runtime ? "allowed" : "blocked");
	Con_Printf ("  active backend: %s\n", (s_active_backend && s_active_backend->name) ? s_active_backend->name : "<none>");
	{
		RenderBackendMilestones milestones;
		if (R_Backend_GetMilestonesForName ("DX12", &milestones))
		{
			Con_Printf ("  milestones: init=%s pass_callbacks=%s present=%s resource_translation=%s\n",
				milestones.init_ready ? "yes" : "no",
				milestones.pass_callbacks_ready ? "yes" : "no",
				milestones.present_ready ? "yes" : "no",
				milestones.resource_translation_ready ? "yes" : "no");
		}
	}
	if (is_stub)
	{
		Con_Printf ("  implemented callbacks: contract no-op stubs only.\n");
		Con_Printf ("  remaining work: device init + swapchain + command lists + descriptor heaps + resource barriers.\n");
	}
	else
	{
		Con_Printf ("  source: non-stub backend is currently registered for 'DX12' (likely ref_dx12 plugin).\n");
		Con_Printf ("  note: current ref_dx12 backend is an independent placeholder with activation blocked until native DX12 implementation lands.\n");
	}
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
		if (s_registered_backends[i] == backend)
			return true;

		if (!q_strcasecmp (s_registered_backends[i]->name, backend->name))
		{
			if (s_registered_backends[i] != backend)
			{
				Con_DPrintf ("Renderer backend '%s' registration overridden (%p -> %p).\n",
					backend->name ? backend->name : "<unnamed>",
					(const void *)s_registered_backends[i],
					(const void *)backend);
			}
			s_registered_backends[i] = backend;
			if (s_active_backend
				&& !s_backend_active
				&& !q_strcasecmp (s_active_backend->name, backend->name))
				s_active_backend = backend;
			if (!q_strcasecmp (backend->name, "OpenGL"))
				s_gl_backend = backend;
			return true;
		}
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
	if (!q_strcasecmp (backend->name, "OpenGL"))
		s_gl_backend = backend;
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
	const render_backend_runtime_status_t status = R_Backend_GetRuntimeStatusForBackend (backend);

	if (!backend)
		return false;
	if (!R_Backend_ValidateContract (backend, true))
		return false;
	if (status == R_BACKEND_RUNTIME_STUB)
	{
		Con_Warning ("Renderer backend '%s' is a stub backend and cannot be selected.\n",
			backend->name ? backend->name : "<unnamed>");
		Con_Warning ("Fallback to OpenGL requested.\n");
		if (s_gl_backend && backend != s_gl_backend)
			return R_Backend_Select (s_gl_backend->name);
		return false;
	}

	if (runtime_switch && (!backend->can_activate || !backend->can_activate (true)))
	{
		Con_Warning (
			"Renderer backend '%s' cannot be activated at runtime; set r_backend and restart the engine.\n",
			backend->name ? backend->name : "<unnamed>");
		return false;
	}

	if (backend->can_activate && !backend->can_activate (false))
	{
		Con_Warning ("Renderer backend '%s' is %s and not ready for activation.\n",
			backend->name ? backend->name : "<unnamed>",
			R_Backend_GetRuntimeStatusLabel (status));
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
	RenderDispatch_Init ();
	memset (&s_last_populated_resources, 0, sizeof (s_last_populated_resources));
	s_last_populated_resources_valid = false;
	s_last_populated_resources_frame = -1;
	R_Backend_ClearExternalResourceRegistry ();
	R_Backend_ClearPipelineMetadataRegistry ();
	s_warned_missing_descriptor_binding_support_frame = -1;
	memset (s_missing_resource_warn_frame, 0xff, sizeof (s_missing_resource_warn_frame));
	Cvar_RegisterVariable (&r_backend);
	Cvar_RegisterVariable (&r_backend_api);
	Cvar_RegisterVariable (&r_refgl_debug);
	Cvar_RegisterVariable (&r_refgl_log_init);
	Cvar_RegisterVariable (&r_refgl_log_passes);
	Cvar_RegisterVariable (&r_refgl_log_resources);
	Cvar_RegisterVariable (&r_refgl_log_state);
	Cvar_RegisterVariable (&r_refgl_validate_state);
	Cvar_RegisterVariable (&r_refgl_validate_fbo);
	Cvar_RegisterVariable (&r_refgl_validate_lifetime);
	Cvar_RegisterVariable (&r_ref_enable_postfx);
	Cvar_RegisterVariable (&r_ref_enable_shadows);
	Cvar_RegisterVariable (&r_ref_enable_fog);
	Cvar_RegisterVariable (&r_ref_enable_lighting);
	Cvar_SetCallback (&r_backend, R_Backend_Changed_f);
	Cvar_SetCallback (&r_backend_api, R_Backend_ApiChanged_f);
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
	/* OBSOLETE (Phase 6): internal builtin OpenGL renderer registration path.
	 * External renderer plugins are now authoritative; ref_gl is the default. */
	R_Backend_LoadRendererPlugins ();

	if (!R_Backend_HasRegisteredName ("OpenGL"))
	{
		if (!s_ref_gl_plugin_candidate_found)
		{
			if (s_renderer_plugin_search_dir_count > 0)
			{
				Con_Warning (
					"No OpenGL renderer backend is available: required plugin '%s' was not found.\n"
					"Searched directories: '%s'%s%s%s%s\n",
					s_ref_gl_plugin_filename,
					s_renderer_plugin_search_dirs[0][0] ? s_renderer_plugin_search_dirs[0] : "<none>",
					s_renderer_plugin_search_dir_count > 1 ? ", '" : "",
					s_renderer_plugin_search_dir_count > 1 ? s_renderer_plugin_search_dirs[1] : "",
					s_renderer_plugin_search_dir_count > 1 ? "'" : "",
					s_renderer_plugin_search_dir_count > 2 ? ", ..." : "");
				Sys_Error (
					"No OpenGL renderer backend is available: required plugin '%s' was not found.\n"
					"Searched directories: '%s'%s%s%s%s\n",
					s_ref_gl_plugin_filename,
					s_renderer_plugin_search_dirs[0][0] ? s_renderer_plugin_search_dirs[0] : "<none>",
					s_renderer_plugin_search_dir_count > 1 ? ", '" : "",
					s_renderer_plugin_search_dir_count > 1 ? s_renderer_plugin_search_dirs[1] : "",
					s_renderer_plugin_search_dir_count > 1 ? "'" : "",
					s_renderer_plugin_search_dir_count > 2 ? ", ..." : "");
			}
			else
			{
				Con_Warning ("No OpenGL renderer backend is available: required plugin '%s' was not found.\n",
					s_ref_gl_plugin_filename);
				Sys_Error ("No OpenGL renderer backend is available: required plugin '%s' was not found.\n",
					s_ref_gl_plugin_filename);
			}
		}
		else if (s_ref_gl_plugin_load_failed || !s_ref_gl_plugin_loaded)
		{
			Con_Warning (
				"No OpenGL renderer backend is available: required plugin '%s' was discovered but failed to load/register.\n"
				"Check earlier renderer plugin warnings for ABI/export/registration details.\n",
				s_ref_gl_plugin_filename);
			Sys_Error (
				"No OpenGL renderer backend is available: required plugin '%s' was discovered but failed to load/register.\n"
				"Check earlier renderer plugin warnings for ABI/export/registration details.\n",
				s_ref_gl_plugin_filename);
		}
		else
		{
			Con_Warning (
				"No OpenGL renderer backend is available: plugin '%s' loaded but did not register backend name 'OpenGL'.\n",
				s_ref_gl_plugin_filename);
			Sys_Error (
				"No OpenGL renderer backend is available: plugin '%s' loaded but did not register backend name 'OpenGL'.\n",
				s_ref_gl_plugin_filename);
		}
	}

	if (!s_active_backend && s_registered_backend_count > 0)
		s_active_backend = s_registered_backends[0];

	if (!R_Backend_Select (R_Backend_ApiToCanonicalName (r_backend_api.string)))
	{
		if (!R_Backend_Select (r_backend.string))
			R_Backend_ApplySelectionToCvar ();
	}

	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
	{
		Con_DPrintf ("R_Backend_Init: active backend='%s' api='%s' registered=%d plugins_loaded=%d\n",
			s_active_backend && s_active_backend->name ? s_active_backend->name : "<none>",
			r_backend_api.string ? r_backend_api.string : "<null>",
			s_registered_backend_count,
			s_plugin_lib_count);
	}

	R_Backend_PrintApiHelp ();
}

qboolean R_Backend_ContextInit (void *window_handle)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (!backend || !backend->context_init)
		return false;
	return backend->context_init (window_handle);
}

void R_Backend_ContextShutdown (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->context_shutdown)
		backend->context_shutdown ();
}

void R_Backend_SwapBuffers (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->swap_buffers)
		backend->swap_buffers ();
}

void R_Backend_Shutdown (void)
{
	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
	{
		Con_DPrintf ("R_Backend_Shutdown: active backend='%s' plugins=%d\n",
			s_active_backend && s_active_backend->name ? s_active_backend->name : "<none>",
			s_plugin_lib_count);
	}
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
	s_gl_backend = NULL;
	s_ref_gl_plugin_candidate_found = false;
	s_ref_gl_plugin_loaded = false;
	s_ref_gl_plugin_load_failed = false;
	s_renderer_plugins_scanned = false;
	s_renderer_plugin_search_dir_count = 0;
	memset (s_renderer_plugin_search_dirs, 0, sizeof (s_renderer_plugin_search_dirs));
	R_Backend_ClearExternalResourceRegistry ();
	R_Backend_ClearPipelineMetadataRegistry ();
	s_backend_initialized = false;
	s_command_encoder_recording = false;
	s_command_encoder_frame = -1;
	memset (&s_last_populated_resources, 0, sizeof (s_last_populated_resources));
	s_last_populated_resources_valid = false;
	s_last_populated_resources_frame = -1;
	s_warned_missing_descriptor_binding_support_frame = -1;
	s_warned_missing_bind_pipeline_support_frame = -1;
	s_warned_missing_pipeline_state_support_frame = -1;
	s_warned_missing_dynamic_state_support_frame = -1;
}

void R_Backend_OnResize (int width, int height)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("R_Backend_OnResize: %dx%d backend='%s'\n",
			width, height,
			backend && backend->name ? backend->name : "<none>");

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
	/* External renderer plugins maintain their own render globals; keep host-side
	 * frame-indexed backend state monotonic by syncing to host_framecount. */
	if (host_framecount > r_framecount)
		r_framecount = host_framecount;
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("R_Backend_BeginFrame: frame=%d backend='%s'\n",
			r_framecount,
			backend && backend->name ? backend->name : "<none>");
	if (s_upload_last_begin_frame != r_framecount)
	{
		s_upload_last_begin_frame = r_framecount;
		s_upload_transient_epoch++;
	}
	R_Backend_ReapTransientExternalResources ();
	if (backend && backend->begin_frame)
		backend->begin_frame ();
}

void R_Backend_EndFrame (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->end_frame)
		backend->end_frame ();
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("R_Backend_EndFrame: frame=%d backend='%s'\n",
			r_framecount,
			backend && backend->name ? backend->name : "<none>");
	if (s_upload_transient_epoch > 0u)
		s_upload_completed_epoch = s_upload_transient_epoch - 1u;
	else
		s_upload_completed_epoch = 0u;
}

void R_Backend_Present (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("R_Backend_Present: frame=%d backend='%s'\n",
			r_framecount,
			backend && backend->name ? backend->name : "<none>");
	if (backend && backend->present)
		backend->present ();
}

void R_Backend_BeginPassEx (const RenderBackendPassDesc *pass_desc)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	static int warned_missing_begin_pass_ex_frame = -1;

	if (!backend)
		return;

	if (backend->begin_pass_ex)
		backend->begin_pass_ex (pass_desc);
	else if (warned_missing_begin_pass_ex_frame != r_framecount)
	{
		Con_DWarning ("Renderer backend '%s' missing required begin_pass_ex callback; pass '%s' setup skipped.\n",
			backend->name ? backend->name : "<unnamed>",
			(pass_desc && pass_desc->name) ? pass_desc->name : "<unnamed>");
		warned_missing_begin_pass_ex_frame = r_framecount;
	}
}

void R_Backend_EndPassEx (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	static int warned_missing_end_pass_ex_frame = -1;

	if (!backend)
		return;

	if (backend->end_pass_ex)
		backend->end_pass_ex ();
	else if (warned_missing_end_pass_ex_frame != r_framecount)
	{
		Con_DWarning ("Renderer backend '%s' missing required end_pass_ex callback; pass teardown skipped.\n",
			backend->name ? backend->name : "<unnamed>");
		warned_missing_end_pass_ex_frame = r_framecount;
	}
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
	{
		if (s_warned_missing_bind_pipeline_support_frame != r_framecount)
		{
			Con_DWarning ("Renderer backend '%s' missing bind_pipeline callback; falling back to set_pipeline_state.\n",
				backend->name ? backend->name : "<unnamed>");
			s_warned_missing_bind_pipeline_support_frame = r_framecount;
		}
		backend->set_pipeline_state (pipeline->state_bits);
	}
	else if (pipeline)
	{
		if (s_warned_missing_pipeline_state_support_frame != r_framecount)
		{
			Con_DWarning ("Renderer backend '%s' missing bind_pipeline and set_pipeline_state callbacks; pipeline state dropped.\n",
				backend->name ? backend->name : "<unnamed>");
			s_warned_missing_pipeline_state_support_frame = r_framecount;
		}
		SDL_assert (!"Renderer backend missing pipeline binding callbacks");
	}

	R_Backend_RecordPipelineMetadata (pipeline);
}

void R_Backend_SetDynamicState (const RenderBackendDynamicState *dynamic_state)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (backend && backend->set_dynamic_state)
		backend->set_dynamic_state (dynamic_state);
	else if (backend && dynamic_state)
	{
		if (s_warned_missing_dynamic_state_support_frame != r_framecount)
		{
			Con_DWarning ("Renderer backend '%s' missing set_dynamic_state callback; dynamic state update dropped.\n",
				backend->name ? backend->name : "<unnamed>");
			s_warned_missing_dynamic_state_support_frame = r_framecount;
		}
		SDL_assert (!"Renderer backend missing set_dynamic_state callback");
	}
}

void R_Backend_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (!backend || !bindings || count == 0u)
		return;

	if (!backend->bind_descriptors)
	{
		if (s_warned_missing_descriptor_binding_support_frame != r_framecount)
		{
			Con_DWarning ("Renderer backend '%s' missing bind_descriptors callback; descriptor bindings were dropped for this pass.\n",
				backend->name ? backend->name : "<unnamed>");
			s_warned_missing_descriptor_binding_support_frame = r_framecount;
		}
		SDL_assert (!"Renderer backend missing bind_descriptors callback");
		return;
	}

	R_Backend_ValidateDescriptorBindings (bindings, count);
	backend->bind_descriptors (bindings, count);
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

void R_Backend_ApplyLegacyPipelineState (unsigned state_bits)
{
	RenderBackendPipelineDesc pipeline_desc;
	RenderBackendDynamicState dynamic_state;

	memset (&pipeline_desc, 0, sizeof (pipeline_desc));
	memset (&dynamic_state, 0, sizeof (dynamic_state));
	pipeline_desc.pipeline_id = 0u;
	pipeline_desc.state_bits = state_bits;
	dynamic_state.blend_state = state_bits;
	dynamic_state.depth_state = state_bits;
	dynamic_state.raster_state = state_bits;
	R_Backend_BindPipeline (&pipeline_desc);
	R_Backend_SetDynamicState (&dynamic_state);
}

void R_Backend_SetPipelineState (unsigned state_bits)
{
	R_Backend_ApplyLegacyPipelineState (state_bits);
}

void R_Backend_ApplyFrameGraphBaseline (unsigned baseline_bits)
{
	unsigned state_bits = glstate;
	qboolean apply_pipeline_state = false;

	if ((baseline_bits & FG_PASS_BASELINE_RESET_BLEND) != 0u)
	{
		state_bits = (state_bits & ~GLS_MASK_BLEND) | GLS_BLEND_OPAQUE;
		apply_pipeline_state = true;
	}

	if ((baseline_bits & FG_PASS_BASELINE_RESET_DEPTH) != 0u)
	{
		state_bits &= ~(GLS_NO_ZTEST | GLS_NO_ZWRITE);
		apply_pipeline_state = true;
	}

	if ((baseline_bits & FG_PASS_BASELINE_RESET_CULL) != 0u)
	{
		state_bits = (state_bits & ~GLS_MASK_CULL) | GLS_CULL_BACK;
		apply_pipeline_state = true;
	}

	if ((baseline_bits & FG_PASS_BASELINE_RESET_PROGRAM_BINDINGS) != 0u)
	{
		state_bits &= ~(GLS_MASK_ATTRIBS | GLS_MASK_INSTANCED_ATTRIBS);
		apply_pipeline_state = true;
	}

	if (apply_pipeline_state)
		R_Backend_SetPipelineState (state_bits);

	if ((baseline_bits & FG_PASS_BASELINE_RESET_SCISSOR) != 0u)
		R_Backend_SetScissor (false, 0, 0, 0, 0);
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
	R_Backend_ValidateFrameGraphResourceRegistry (out_handles);
	s_last_populated_resources = *out_handles;
	s_last_populated_resources_valid = true;
	s_last_populated_resources_frame = r_framecount;
}

int R_Backend_GetSceneSampleCount (void)
{
	const IRenderBackend *backend = R_GetRenderBackend ();

	if (backend && backend->get_scene_sample_count)
		return backend->get_scene_sample_count ();
	return 1;
}
