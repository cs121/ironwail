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

cvar_t r_backend = { "r_backend", "OpenGL", CVAR_ARCHIVE };

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
		Con_Warning ("Renderer backend '%s' not found; keeping '%s'\n",
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
	const RenderBackendCaps *caps;

	if (!backend)
		return false;

	s_active_backend = backend;
	memset (&s_active_backend_caps, 0, sizeof (s_active_backend_caps));
	s_active_backend_caps.msaa_mode_mask = 1u;
	s_active_backend_caps.max_msaa_samples = 1u;

	caps = (backend->get_caps != NULL) ? backend->get_caps () : NULL;
	if (caps)
		s_active_backend_caps = *caps;
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
	return &s_active_backend_caps;
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
