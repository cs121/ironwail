#include "quakedef.h"
#include "glquake.h"
#include "gl_backend.h"
#include "r_framegraph.h"

enum
{
	GL_BACKEND_MAX_RESOURCES = 128
};

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
	s_gl_resources.next_opaque_id = 1u;
	GL_Backend_ResetStateCache ();
}

static unsigned short GLBackend_RegisterResourceInternal (render_backend_resource_type_t type, render_backend_resource_slot_t slot, gl_backend_resource_key_t key, render_backend_resource_lifetime_t lifetime, unsigned native_id)
{
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

