#ifndef RENDERER_PLUGIN_H
#define RENDERER_PLUGIN_H

#include "render_api.h"

#define IW_RENDERER_PLUGIN_ABI_MAJOR 2u
#define IW_RENDERER_PLUGIN_ABI_MINOR 0u

#if defined(_WIN32)
#define IW_RENDERER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define IW_RENDERER_PLUGIN_EXPORT
#endif

typedef struct iw_renderer_plugin_host_api_s
{
	unsigned int struct_size;
	unsigned int abi_major;
	unsigned int abi_minor;
	/* ABI v2: register a backend implementation directly via vtable. */
	qboolean (*register_backend)(const IRenderBackend *backend);
	/* Built-in OpenGL backend interface exposed by host for v2 plugin wiring. */
	const IRenderBackend *builtin_opengl_backend;
	/* Deprecated compatibility path, retained for transition-only plugins. */
	qboolean (*register_builtin_backend)(const char *backend_name);
} iw_renderer_plugin_host_api_t;

typedef struct iw_renderer_plugin_descriptor_s
{
	unsigned int struct_size;
	unsigned int abi_major;
	unsigned int abi_minor;
	const char *plugin_name;
	/* Register one or more backends using host_api entrypoints. */
	qboolean (*register_plugin)(const iw_renderer_plugin_host_api_t *host_api);
} iw_renderer_plugin_descriptor_t;

typedef const iw_renderer_plugin_descriptor_t *(*iw_renderer_plugin_query_fn)(void);

/*
 * Host-provided accessor for the built-in OpenGL backend interface.
 * Plugins use this to register OpenGL without depending on gl_backend.h.
 */
const IRenderBackend *IW_RendererPlugin_GetBuiltinOpenGLBackend (void);

#endif
