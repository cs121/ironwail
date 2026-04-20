#include "quakedef.h"
#include "glquake.h"
#include "gl_backend.h"
#include "r_resources_gl.h"

static unsigned GL_ResourceRegistry_ResolveSceneVelocity (void)
{
	return (framebufs.scene.samples > 1) ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
}

static unsigned GL_ResourceRegistry_ResolveNativeResource (render_backend_resource_slot_t slot)
{
	switch (slot)
	{
	case R_BACKEND_RESOURCE_SLOT_SCENE_FBO:
		return framebufs.scene.fbo;
	case R_BACKEND_RESOURCE_SLOT_SCENE_COLOR:
		return framebufs.scene.color_tex;
	case R_BACKEND_RESOURCE_SLOT_SCENE_VELOCITY:
		return framebufs.scene.velocity_tex;
	case R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH:
		return framebufs.scene.depth_stencil_tex;
	case R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_FBO:
		return framebufs.resolved_scene.fbo;
	case R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_COLOR:
		return framebufs.resolved_scene.color_tex;
	case R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_VELOCITY:
		return framebufs.resolved_scene.velocity_tex;
	case R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO:
		return framebufs.composite.fbo;
	case R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR:
		return framebufs.composite.color_tex;
	case R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH:
		return framebufs.composite.depth_stencil_tex;
	case R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH:
		return framebufs.shadow.sun_depth_tex;
	case R_BACKEND_RESOURCE_SLOT_VELOCITY:
		return GL_ResourceRegistry_ResolveSceneVelocity ();
	default:
		break;
	}

	return 0;
}

static const gl_resource_registry_entry_t s_gl_resource_registry[] = {
	{ "scene_fbo", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_FRAMEBUFFER, R_BACKEND_RESOURCE_SLOT_SCENE_FBO, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "scene_color", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SCENE_COLOR, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "scene_velocity", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SCENE_VELOCITY, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "scene_depth", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "resolved_scene_fbo", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_FRAMEBUFFER, R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_FBO, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "resolved_scene_color", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_COLOR, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "resolved_scene_velocity", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_VELOCITY, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f },
	{ "composite_fbo", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_FRAMEBUFFER, R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_NATIVE, 1.f, 1.f },
	{ "composite_color", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_NATIVE, 1.f, 1.f },
	{ "composite_depth", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_NATIVE, 1.f, 1.f },
	{ "shadow_sun_depth", "R_Shadow_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH, R_BACKEND_RESOURCE_LIFETIME_LEVEL, GL_RESOURCE_SIZE_SHADOW_SUN, 1.f, 1.f },
	{ "velocity", "GL_CreateFrameBuffers", R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_VELOCITY, R_BACKEND_RESOURCE_LIFETIME_FRAME, GL_RESOURCE_SIZE_SCENE, 1.f, 1.f }
};

const gl_resource_registry_entry_t *GL_ResourceRegistry_FindEntry (render_backend_resource_slot_t slot)
{
	int i;

	for (i = 0; i < (int)Q_COUNTOF (s_gl_resource_registry); ++i)
	{
		if (s_gl_resource_registry[i].slot == slot)
			return &s_gl_resource_registry[i];
	}

	return NULL;
}

unsigned GL_ResourceRegistry_GetEntryCount (void)
{
	return (unsigned)Q_COUNTOF (s_gl_resource_registry);
}

const gl_resource_registry_entry_t *GL_ResourceRegistry_GetEntryByIndex (unsigned index)
{
	if (index >= (unsigned)Q_COUNTOF (s_gl_resource_registry))
		return NULL;

	return &s_gl_resource_registry[index];
}

void GL_ResourceRegistry_RegisterSlot (render_backend_resource_slot_t slot)
{
	const gl_resource_registry_entry_t *entry = GL_ResourceRegistry_FindEntry (slot);
	unsigned native_id;

	if (!entry)
		return;

	native_id = GL_ResourceRegistry_ResolveNativeResource (slot);
	if (native_id == 0)
		return;

	GL_Backend_RegisterResource (entry->type, entry->slot, entry->lifetime, native_id);
}

void GL_ResourceRegistry_UnregisterSlot (render_backend_resource_slot_t slot)
{
	if (GL_ResourceRegistry_FindEntry (slot))
		GL_Backend_UnregisterResourceBySlot (slot);
}

void GL_ResourceRegistry_RegisterFrameGraphSlots (void)
{
	unsigned i;

	for (i = 0; i < (unsigned)Q_COUNTOF (s_gl_resource_registry); ++i)
		GL_ResourceRegistry_RegisterSlot (s_gl_resource_registry[i].slot);
}

void GL_ResourceRegistry_UnregisterFrameGraphSlots (void)
{
	unsigned i;

	for (i = 0; i < (unsigned)Q_COUNTOF (s_gl_resource_registry); ++i)
		GL_ResourceRegistry_UnregisterSlot (s_gl_resource_registry[i].slot);
}
