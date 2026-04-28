#include "renderer_plugin.h"

static qboolean IW_RendererBuiltinGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	(void)host_api;
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
