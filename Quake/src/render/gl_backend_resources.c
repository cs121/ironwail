#include "quakedef.h"
#include "glquake.h"
#include "gl_backend.h"
#include "r_framegraph.h"
#include "r_resources_gl.h"
#include "gl_texmgr.h"

enum
{
	GL_BACKEND_MAX_RESOURCES = 128,
	GL_BACKEND_MAX_TEXTURE_HANDLES = 256
};


typedef enum gl_backend_texture_handle_kind_e
{
	GL_BACKEND_TEXTURE_HANDLE_NONE = 0,
	GL_BACKEND_TEXTURE_HANDLE_LEGACY_GLTEXTURE,
	GL_BACKEND_TEXTURE_HANDLE_NATIVE
} gl_backend_texture_handle_kind_t;

static struct gl_backend_texture_handle_table_s
{
	struct gl_backend_texture_handle_entry_s
	{
		render_texture_handle_t handle;
		gl_backend_texture_handle_kind_t kind;
		gltexture_t *legacy_texture;
		unsigned native_id;
		unsigned target;
	} entries[GL_BACKEND_MAX_TEXTURE_HANDLES];
	render_texture_handle_t next_handle;
	unsigned short count;
} s_gl_texture_handles = { { { 0 } }, 1u, 0u };

#ifndef RENDERER_PLUGIN_BUILD
extern cvar_t r_backend_debug;
extern cvar_t r_renderer_migration_debug;
#endif

static qboolean GLBackend_TextureHandleDebugEnabled (void)
{
#ifdef RENDERER_PLUGIN_BUILD
	return false;
#else
	return r_renderer_migration_debug.value != 0.f || r_backend_debug.value != 0.f;
#endif
}

static render_texture_handle_t GLBackend_AllocTextureHandle (void)
{
	render_texture_handle_t handle = s_gl_texture_handles.next_handle++;
	if (handle == RENDER_TEXTURE_HANDLE_INVALID)
		handle = s_gl_texture_handles.next_handle++;
	return handle;
}

static int GLBackend_FindTextureHandleIndex (render_texture_handle_t handle)
{
	unsigned i;

	if (!R_TextureHandle_IsValid (handle))
		return -1;

	for (i = 0; i < s_gl_texture_handles.count; ++i)
	{
		if (s_gl_texture_handles.entries[i].handle == handle)
			return (int)i;
	}

	return -1;
}

static int GLBackend_FindLegacyTextureHandleIndex (const gltexture_t *texture)
{
	unsigned i;

	if (!texture)
		return -1;

	for (i = 0; i < s_gl_texture_handles.count; ++i)
	{
		const struct gl_backend_texture_handle_entry_s *entry = &s_gl_texture_handles.entries[i];
		if (entry->kind == GL_BACKEND_TEXTURE_HANDLE_LEGACY_GLTEXTURE && entry->legacy_texture == texture)
			return (int)i;
	}

	return -1;
}

static int GLBackend_FindNativeTextureHandleIndex (unsigned target, unsigned native_id)
{
	unsigned i;

	if (target == 0u || native_id == 0u)
		return -1;

	for (i = 0; i < s_gl_texture_handles.count; ++i)
	{
		const struct gl_backend_texture_handle_entry_s *entry = &s_gl_texture_handles.entries[i];
		if (entry->kind == GL_BACKEND_TEXTURE_HANDLE_NATIVE && entry->target == target && entry->native_id == native_id)
			return (int)i;
	}

	return -1;
}

static struct gl_backend_texture_handle_entry_s *GLBackend_NewTextureHandleEntry (void)
{
	struct gl_backend_texture_handle_entry_s *entry;

	if (s_gl_texture_handles.count >= GL_BACKEND_MAX_TEXTURE_HANDLES)
		return NULL;

	entry = &s_gl_texture_handles.entries[s_gl_texture_handles.count++];
	memset (entry, 0, sizeof (*entry));
	entry->handle = GLBackend_AllocTextureHandle ();
	return entry;
}

render_texture_handle_t R_TextureHandle_FromLegacyGLTexture (gltexture_t *tex)
{
	struct gl_backend_texture_handle_entry_s *entry;
	int index;

	if (!tex)
		return R_TextureHandle_Invalid ();

	index = GLBackend_FindLegacyTextureHandleIndex (tex);
	if (index >= 0)
		return s_gl_texture_handles.entries[index].handle;

	entry = GLBackend_NewTextureHandleEntry ();
	if (!entry)
	{
		if (GLBackend_TextureHandleDebugEnabled ())
			Con_DPrintf ("R_TextureHandle: legacy gltexture_t handle table full\n");
		return R_TextureHandle_Invalid ();
	}

	entry->kind = GL_BACKEND_TEXTURE_HANDLE_LEGACY_GLTEXTURE;
	entry->legacy_texture = tex;
	entry->target = (unsigned)TexMgr_GetTarget (tex);
	entry->native_id = (unsigned)TexMgr_GetNativeHandle (tex);
	return entry->handle;
}

gltexture_t *R_TextureHandle_ResolveLegacyGLTexture (render_texture_handle_t h)
{
	int index = GLBackend_FindTextureHandleIndex (h);

	if (index < 0 || s_gl_texture_handles.entries[index].kind != GL_BACKEND_TEXTURE_HANDLE_LEGACY_GLTEXTURE)
		return NULL;

	return s_gl_texture_handles.entries[index].legacy_texture;
}

render_texture_handle_t GL_Backend_TextureHandleFromNativeTexture (unsigned target, unsigned native_id)
{
	struct gl_backend_texture_handle_entry_s *entry;
	int index;

	if (target == 0u || native_id == 0u)
		return R_TextureHandle_Invalid ();

	index = GLBackend_FindNativeTextureHandleIndex (target, native_id);
	if (index >= 0)
		return s_gl_texture_handles.entries[index].handle;

	entry = GLBackend_NewTextureHandleEntry ();
	if (!entry)
	{
		if (GLBackend_TextureHandleDebugEnabled ())
			Con_DPrintf ("R_TextureHandle: native texture handle table full\n");
		return R_TextureHandle_Invalid ();
	}

	entry->kind = GL_BACKEND_TEXTURE_HANDLE_NATIVE;
	entry->target = target;
	entry->native_id = native_id;
	return entry->handle;
}

qboolean GL_Backend_ResolveTextureHandleNative (render_texture_handle_t handle, unsigned *out_target, unsigned *out_native_id)
{
	const struct gl_backend_texture_handle_entry_s *entry;
	int index = GLBackend_FindTextureHandleIndex (handle);

	if (out_target)
		*out_target = 0u;
	if (out_native_id)
		*out_native_id = 0u;
	if (index < 0)
		return false;

	entry = &s_gl_texture_handles.entries[index];
	if (entry->kind == GL_BACKEND_TEXTURE_HANDLE_LEGACY_GLTEXTURE)
	{
		if (!entry->legacy_texture)
			return false;
		if (out_target)
			*out_target = (unsigned)TexMgr_GetTarget (entry->legacy_texture);
		if (out_native_id)
			*out_native_id = (unsigned)TexMgr_GetNativeHandle (entry->legacy_texture);
		return out_native_id ? (*out_native_id != 0u) : true;
	}
	if (entry->kind == GL_BACKEND_TEXTURE_HANDLE_NATIVE)
	{
		if (out_target)
			*out_target = entry->target;
		if (out_native_id)
			*out_native_id = entry->native_id;
		return entry->target != 0u && entry->native_id != 0u;
	}

	return false;
}

qboolean GL_Backend_BindTextureHandle (unsigned texunit, render_texture_handle_t handle, unsigned expected_target)
{
	unsigned target = 0u;
	unsigned native_id = 0u;

	if (!GL_Backend_ResolveTextureHandleNative (handle, &target, &native_id))
	{
		if (GLBackend_TextureHandleDebugEnabled ())
			Con_DPrintf ("R_TextureHandle: failed to resolve texture handle %u\n", (unsigned)handle);
		return false;
	}
	if (expected_target != 0u && target != expected_target)
	{
		if (GLBackend_TextureHandleDebugEnabled ())
			Con_DPrintf ("R_TextureHandle: target mismatch for handle %u: got %u expected %u\n", (unsigned)handle, target, expected_target);
		return false;
	}

	return GL_BindNative ((GLenum)texunit, (GLenum)target, (GLuint)native_id);
}

static struct gl_backend_resource_table_s
{
	struct gl_backend_resource_entry_s
	{
		unsigned short opaque_id;
		unsigned native_id;
		unsigned char type;
		unsigned char lifetime;
		unsigned short slot;
		unsigned short key;
	} entries[GL_BACKEND_MAX_RESOURCES];
	unsigned short next_opaque_id;
	unsigned short count;
} s_gl_resources;

static int GLBackend_FindResourceIndexBySlot (render_backend_resource_slot_t slot)
{
	unsigned i;

	if (slot <= R_BACKEND_RESOURCE_SLOT_NONE || slot >= R_BACKEND_RESOURCE_SLOT_COUNT)
		return -1;

	for (i = 0; i < s_gl_resources.count; ++i)
	{
		if (s_gl_resources.entries[i].slot == (unsigned short)slot)
			return (int)i;
	}

	return -1;
}

static int GLBackend_FindResourceIndexByKey (gl_backend_resource_key_t key)
{
	unsigned index;

	if (key <= GL_BACKEND_RESOURCE_KEY_NONE)
		return -1;

	for (index = 0; index < s_gl_resources.count; ++index)
	{
		if (s_gl_resources.entries[index].key == (unsigned short)key)
			return (int)index;
	}

	return -1;
}

void GL_Backend_ResetResources (void)
{
	memset (&s_gl_resources, 0, sizeof (s_gl_resources));
	memset (&s_gl_texture_handles, 0, sizeof (s_gl_texture_handles));
	s_gl_resources.next_opaque_id = 1u;
	s_gl_texture_handles.next_handle = 1u;
	GL_Backend_ResetStateCache ();
}

static unsigned short GLBackend_RegisterResourceInternal (render_backend_resource_type_t type, render_backend_resource_slot_t slot, gl_backend_resource_key_t key, render_backend_resource_lifetime_t lifetime, unsigned native_id)
{
	/* REF_GL_PRIVATE / TODO_RESOURCE_BOUNDARY:
	 * `native_id` is a backend-local native object handle (GL in ref_gl). */
	unsigned short opaque_id;
	int index;

	if (type == R_BACKEND_RESOURCE_NONE || native_id == 0u)
		return 0u;

	index = (slot > R_BACKEND_RESOURCE_SLOT_NONE) ? GLBackend_FindResourceIndexBySlot (slot) : GLBackend_FindResourceIndexByKey (key);
	if (index < 0)
	{
		if (s_gl_resources.count >= GL_BACKEND_MAX_RESOURCES)
			return 0u;
		index = (int)s_gl_resources.count++;
		memset (&s_gl_resources.entries[index], 0, sizeof (s_gl_resources.entries[index]));
		opaque_id = s_gl_resources.next_opaque_id++;
		if (opaque_id == 0u)
			opaque_id = s_gl_resources.next_opaque_id++;
		s_gl_resources.entries[index].opaque_id = opaque_id;
	}

	s_gl_resources.entries[index].native_id = native_id;
	s_gl_resources.entries[index].type = (unsigned char)type;
	s_gl_resources.entries[index].lifetime = (unsigned char)lifetime;
	s_gl_resources.entries[index].slot = (unsigned short)slot;
	s_gl_resources.entries[index].key = (unsigned short)key;
	return s_gl_resources.entries[index].opaque_id;
}

unsigned short GL_Backend_RegisterResource (render_backend_resource_type_t type, render_backend_resource_slot_t slot, render_backend_resource_lifetime_t lifetime, unsigned native_id)
{
	return GLBackend_RegisterResourceInternal (type, slot, GL_BACKEND_RESOURCE_KEY_NONE, lifetime, native_id);
}

unsigned short GL_Backend_RegisterNamedResource (render_backend_resource_type_t type, gl_backend_resource_key_t key, render_backend_resource_lifetime_t lifetime, unsigned native_id)
{
	return GLBackend_RegisterResourceInternal (type, R_BACKEND_RESOURCE_SLOT_NONE, key, lifetime, native_id);
}

void GL_Backend_UnregisterResourceBySlot (render_backend_resource_slot_t slot)
{
	int index = GLBackend_FindResourceIndexBySlot (slot);

	if (index < 0)
		return;

	s_gl_resources.entries[index] = s_gl_resources.entries[s_gl_resources.count - 1];
	s_gl_resources.count--;
}

void GL_Backend_UnregisterNamedResource (gl_backend_resource_key_t key)
{
	int index = GLBackend_FindResourceIndexByKey (key);

	if (index < 0)
		return;

	s_gl_resources.entries[index] = s_gl_resources.entries[s_gl_resources.count - 1];
	s_gl_resources.count--;
}

unsigned GL_Backend_ResolveOpaqueResource (unsigned short opaque_id)
{
	unsigned i;

	if (opaque_id == 0u)
		return 0u;

	for (i = 0; i < s_gl_resources.count; ++i)
	{
		if (s_gl_resources.entries[i].opaque_id == opaque_id)
			return s_gl_resources.entries[i].native_id;
	}

	return 0u;
}

void GL_Backend_PopulateResourceRegistry (RenderGraphResourceHandle *out_handles)
{
	unsigned i;

	if (!out_handles)
		return;

	/* Keep backend registry resilient against init-order differences between
	 * host and plugin paths by re-registering active GL slots on demand. */
	GL_ResourceRegistry_RegisterFrameGraphSlots ();

	for (i = 0; i < s_gl_resources.count; ++i)
	{
		const struct gl_backend_resource_entry_s *entry = &s_gl_resources.entries[i];
		unsigned registry_index;
		render_backend_resource_slot_t slot;

		if (entry->slot <= R_BACKEND_RESOURCE_SLOT_NONE || entry->slot >= R_BACKEND_RESOURCE_SLOT_COUNT)
			continue;
		if (entry->opaque_id == 0u || entry->native_id == 0u || entry->type == R_BACKEND_RESOURCE_NONE)
			continue;
		if (out_handles->registry_count >= (unsigned char)Q_COUNTOF (out_handles->registry))
			break;

		slot = (render_backend_resource_slot_t)entry->slot;
		registry_index = out_handles->registry_count++;
		out_handles->registry[registry_index].resource_id = entry->opaque_id;
		out_handles->registry[registry_index].native_id = entry->native_id;
		out_handles->registry[registry_index].type = entry->type;
		out_handles->registry[registry_index].lifetime = entry->lifetime;
		out_handles->registry[registry_index].slot = entry->slot;

		out_handles->slot_resource_ids[slot] = entry->opaque_id;
		out_handles->refs[slot].type = entry->type;
		out_handles->refs[slot].slot = entry->slot;
		out_handles->refs[slot].opaque_id = entry->opaque_id;
	}
}
