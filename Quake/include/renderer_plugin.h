#ifndef RENDERER_PLUGIN_H
#define RENDERER_PLUGIN_H

#include "render_api.h"
#include "renderer_host_bridge.h"
#include <stddef.h>

#define IW_RENDERER_PLUGIN_ABI_MAJOR 5u
#define IW_RENDERER_PLUGIN_ABI_MINOR 0u

#if defined(_WIN32)
#define IW_RENDERER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define IW_RENDERER_PLUGIN_EXPORT
#endif

/*
 * Renderer plugin ABI policy
 * --------------------------
 * - Major bumps are breaking and require both host+plugin rebuild.
 * - Minor bumps are additive only and must preserve existing fields.
 * - Plugins must gate optional host fields/services using struct_size checks.
 * - ABI v5 removes legacy built-in backend registration callbacks.
 */

typedef enum iw_renderer_surface_origin_e
{
	IW_RENDERER_SURFACE_ORIGIN_UPPER_LEFT = 0,
	IW_RENDERER_SURFACE_ORIGIN_LOWER_LEFT
} iw_renderer_surface_origin_t;

typedef struct iw_renderer_host_surface_info_s
{
	unsigned int struct_size;
	int surface_x;
	int surface_y;
	int surface_width;
	int surface_height;
	int view_x;
	int view_y;
	int view_width;
	int view_height;
	int scene_width;
	int scene_height;
	unsigned int scene_samples;
	unsigned int frame_index;
	unsigned int surface_origin;
	qboolean needs_scene_effects;
	qboolean needs_postprocess;
} iw_renderer_host_surface_info_t;

typedef struct iw_renderer_host_resource_handle_s
{
	unsigned int struct_size;
	unsigned int resource_id;
	unsigned int native_id;
	unsigned char type;
	unsigned char lifetime;
	unsigned short slot;
} iw_renderer_host_resource_handle_t;

typedef struct iw_renderer_host_upload_epoch_s
{
	unsigned int struct_size;
	unsigned int frame_index;
	unsigned int transient_epoch;
	unsigned int completed_epoch;
} iw_renderer_host_upload_epoch_t;

typedef struct iw_renderer_host_shader_metadata_s
{
	unsigned int struct_size;
	unsigned int shader_id;
	const char *debug_name;
	const char *entry_point;
	const char *stage;
	unsigned int permutation_key;
} iw_renderer_host_shader_metadata_t;

typedef struct iw_renderer_host_pipeline_metadata_s
{
	unsigned int struct_size;
	unsigned int pipeline_id;
	const char *debug_name;
	unsigned int state_bits;
	unsigned int shader_count;
	unsigned int shader_ids[8];
} iw_renderer_host_pipeline_metadata_t;

typedef struct iw_renderer_plugin_surface_services_s
{
	unsigned int struct_size;
	qboolean (*get_surface_info)(iw_renderer_host_surface_info_t *out_info);
} iw_renderer_plugin_surface_services_t;

typedef struct iw_renderer_plugin_resource_services_s
{
	unsigned int struct_size;
	qboolean (*resolve_resource_by_slot)(render_backend_resource_slot_t slot, iw_renderer_host_resource_handle_t *out_handle);
	qboolean (*resolve_resource_by_ref)(const render_backend_resource_ref_t *ref, iw_renderer_host_resource_handle_t *out_handle);
	qboolean (*register_external_resource)(const iw_renderer_host_resource_handle_t *resource, unsigned int *out_resource_id);
} iw_renderer_plugin_resource_services_t;

typedef struct iw_renderer_plugin_upload_services_s
{
	unsigned int struct_size;
	qboolean (*query_upload_epoch)(iw_renderer_host_upload_epoch_t *out_epoch);
	qboolean (*is_transient_resource_alive)(unsigned int resource_id, unsigned int producer_epoch);
} iw_renderer_plugin_upload_services_t;

typedef struct iw_renderer_plugin_pipeline_services_s
{
	unsigned int struct_size;
	qboolean (*get_shader_metadata)(unsigned int shader_id, iw_renderer_host_shader_metadata_t *out_metadata);
	qboolean (*get_pipeline_metadata)(unsigned int pipeline_id, iw_renderer_host_pipeline_metadata_t *out_metadata);
} iw_renderer_plugin_pipeline_services_t;

typedef struct iw_renderer_plugin_host_api_s
{
	unsigned int struct_size;
	unsigned int abi_major;
	unsigned int abi_minor;
	/* Register a backend implementation directly via vtable. */
	qboolean (*register_backend)(const IRenderBackend *backend);

	/* Host services (all backend-neutral, no GL/D3D/VK native types). */
	const iw_renderer_plugin_surface_services_t *surface_services;
	const iw_renderer_plugin_resource_services_t *resource_services;
	const iw_renderer_plugin_upload_services_t *upload_services;
	const iw_renderer_plugin_pipeline_services_t *pipeline_services;

	/* Host bridge for full engine services (renderer DLL extraction). */
	const iw_renderer_host_bridge_t *bridge;
	qboolean (*register_entry_points)(const iw_renderer_entry_points_t *entry_points);
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

#define IW_RENDERER_PLUGIN_HOST_API_V2_SIZE ((unsigned int)(offsetof(iw_renderer_plugin_host_api_t, register_backend) + sizeof (((iw_renderer_plugin_host_api_t *)0)->register_backend)))
#define IW_RENDERER_PLUGIN_HOST_API_V3_SIZE ((unsigned int)(offsetof(iw_renderer_plugin_host_api_t, pipeline_services) + sizeof (((iw_renderer_plugin_host_api_t *)0)->pipeline_services)))
#define IW_RENDERER_PLUGIN_HOST_API_V4_SIZE ((unsigned int)(offsetof(iw_renderer_plugin_host_api_t, register_entry_points) + sizeof (((iw_renderer_plugin_host_api_t *)0)->register_entry_points)))
#define IW_RENDERER_PLUGIN_DESCRIPTOR_MIN_SIZE ((unsigned int)(offsetof(iw_renderer_plugin_descriptor_t, register_plugin) + sizeof (((iw_renderer_plugin_descriptor_t *)0)->register_plugin)))

#define IW_RENDERER_PLUGIN_HOST_HAS_FIELD(host_api, field_name) \
	((host_api) != NULL && (host_api)->struct_size >= (unsigned int)(offsetof(iw_renderer_plugin_host_api_t, field_name) + sizeof ((host_api)->field_name)))

#endif
