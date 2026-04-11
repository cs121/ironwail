#include "renderer_plugin.h"

static qboolean IW_RendererBuiltinGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	const IRenderBackend *backend;

	if (!host_api || host_api->struct_size < sizeof (*host_api))
	{
		return false;
	}

	backend = host_api->builtin_opengl_backend;
	if (backend && host_api->register_backend)
		return host_api->register_backend (backend);

	/* ABI v2 keeps this as a temporary bridge for transition-only plugins. */
	if (host_api->register_builtin_backend)
		return host_api->register_builtin_backend ("OpenGL");

	return false;
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"builtin-opengl",
		IW_RendererBuiltinGL_Register
	};

	return &descriptor;
}
