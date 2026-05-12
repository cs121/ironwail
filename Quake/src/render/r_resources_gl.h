#ifndef R_RESOURCES_GL_H
#define R_RESOURCES_GL_H

#include "render_api.h"
#include "texture_handles.h"

typedef enum gl_resource_size_class_e
{
	GL_RESOURCE_SIZE_NONE = 0,
	GL_RESOURCE_SIZE_SCENE,
	GL_RESOURCE_SIZE_NATIVE,
	GL_RESOURCE_SIZE_SHADOW_SUN
} gl_resource_size_class_t;

typedef struct gl_resource_registry_entry_s
{
	const char *name;
	const char *creator;
	render_backend_resource_type_t type;
	render_backend_resource_slot_t slot;
	render_backend_resource_lifetime_t lifetime;
	gl_resource_size_class_t size_class;
	float width_scale;
	float height_scale;
} gl_resource_registry_entry_t;

const gl_resource_registry_entry_t *GL_ResourceRegistry_FindEntry (render_backend_resource_slot_t slot);
unsigned GL_ResourceRegistry_GetEntryCount (void);
const gl_resource_registry_entry_t *GL_ResourceRegistry_GetEntryByIndex (unsigned index);

void GL_ResourceRegistry_RegisterSlot (render_backend_resource_slot_t slot);
void GL_ResourceRegistry_UnregisterSlot (render_backend_resource_slot_t slot);
void GL_ResourceRegistry_RegisterFrameGraphSlots (void);
void GL_ResourceRegistry_UnregisterFrameGraphSlots (void);

render_texture_handle_t GL_Backend_TextureHandleFromNativeTexture (unsigned target, unsigned native_id);
qboolean GL_Backend_ResolveTextureHandleNative (render_texture_handle_t handle, unsigned *out_target, unsigned *out_native_id);
qboolean GL_Backend_BindTextureHandle (unsigned texunit, render_texture_handle_t handle, unsigned expected_target);

#endif
