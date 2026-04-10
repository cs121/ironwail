#include "renderer_plugin.h"

static qboolean IW_RendererBuiltinGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api
		|| host_api->struct_size < sizeof (*host_api)
		|| !host_api->register_builtin_backend)
	{
		return false;
	}

	return host_api->register_builtin_backend ("OpenGL");
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
