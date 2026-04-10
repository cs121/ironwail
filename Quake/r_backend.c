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

cvar_t r_backend = { "r_backend", "OpenGL", CVAR_ARCHIVE };

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
	Cvar_RegisterVariable (&r_backend);
	Cvar_SetCallback (&r_backend, R_Backend_Changed_f);

	GL_Backend_Register ();

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

void R_Backend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z)
{
	const IRenderBackend *backend = R_GetRenderBackend ();
	if (backend && backend->dispatch)
		backend->dispatch (group_x, group_y, group_z);
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
