#ifndef RENDERER_PLUGIN_H
#define RENDERER_PLUGIN_H

#include "q_stdinc.h"

#define IW_RENDERER_PLUGIN_ABI_MAJOR 1u
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
	qboolean (*register_builtin_backend)(const char *backend_name);
} iw_renderer_plugin_host_api_t;

typedef struct iw_renderer_plugin_descriptor_s
{
	unsigned int struct_size;
	unsigned int abi_major;
	unsigned int abi_minor;
	const char *plugin_name;
	qboolean (*register_plugin)(const iw_renderer_plugin_host_api_t *host_api);
} iw_renderer_plugin_descriptor_t;

typedef const iw_renderer_plugin_descriptor_t *(*iw_renderer_plugin_query_fn)(void);

#endif
