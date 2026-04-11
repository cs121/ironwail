#include "quakedef.h"
#include "glquake.h"
#include "gl_backend.h"

#include "r_framegraph.h"
#include "renderer_plugin.h"

#ifndef GL_CLIP_DEPTH_MODE
#define GL_CLIP_DEPTH_MODE 0x935D
#endif
#ifndef GL_NEGATIVE_ONE_TO_ONE
#define GL_NEGATIVE_ONE_TO_ONE 0x935E
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif

enum
{
	GL_BACKEND_TIMER_QUERY_RING = 3,
	GL_BACKEND_MAX_RESOURCES = 128
};

typedef struct gl_backend_timer_slot_s
{
	GLuint start_query;
	GLuint end_query;
	int frame_index;
	qboolean issued;
} gl_backend_timer_slot_t;

typedef struct gl_backend_timer_pass_s
{
	gl_backend_timer_slot_t timer_slots[GL_BACKEND_TIMER_QUERY_RING];
	int timer_write_index;
	int timer_active_slot_plus_one;
	qboolean pending_sample;
	double pending_gpu_ms;
} gl_backend_timer_pass_t;

static gl_backend_timer_pass_t s_timer_passes[R_BACKEND_MAX_PROFILE_SLOTS];
static RenderBackendCaps s_gl_backend_caps;
static int s_legacy_pass_resource_fallback_frame = -1;
static gl_proc_address_loader_t s_gl_proc_loader = NULL;
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

void GL_Backend_SetProcAddressLoader (gl_proc_address_loader_t loader)
{
	s_gl_proc_loader = loader;
}

void *GL_Backend_GetProcAddress (const char *name)
{
	if (!s_gl_proc_loader || !name || !name[0])
		return NULL;
	return s_gl_proc_loader (name);
}

static unsigned GLBackend_ResolveResourceOpaqueId (const RenderGraphResourceHandle *resources, unsigned short opaque_id);

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

static unsigned GLBackend_ResolveResourceOpaqueId (const RenderGraphResourceHandle *resources, unsigned short opaque_id)
{
	(void)resources;
	return GL_Backend_ResolveOpaqueResource (opaque_id);
}

static void GLBackend_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles)
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

static int GLBackend_GetSceneSampleCount (void)
{
	return framebufs.scene.samples;
}

static qboolean GLBackend_QuerySurfaceMetrics (RenderBackendSurfaceMetrics *out_metrics)
{
	if (!out_metrics)
		return false;

	memset (out_metrics, 0, sizeof (*out_metrics));
	out_metrics->surface_x = glx;
	out_metrics->surface_y = gly;
	out_metrics->surface_width = (glwidth > 0) ? glwidth : q_max (1, vid.width);
	out_metrics->surface_height = (glheight > 0) ? glheight : q_max (1, vid.height);
	out_metrics->view_x = out_metrics->surface_x + r_refdef.vrect.x;
	out_metrics->view_y = out_metrics->surface_y + out_metrics->surface_height - r_refdef.vrect.y - r_refdef.vrect.height;
	out_metrics->view_width = q_max (1, r_refdef.vrect.width);
	out_metrics->view_height = q_max (1, r_refdef.vrect.height);
	out_metrics->scene_width = q_max (1, R_GetSceneRenderWidth ());
	out_metrics->scene_height = q_max (1, R_GetSceneRenderHeight ());
	return true;
}

static qboolean GLBackend_NeedsSceneEffects (void)
{
	return GL_NeedsSceneEffects ();
}

static qboolean GLBackend_NeedsPostprocess (void)
{
	return GL_NeedsPostprocess ();
}

static qboolean GLBackend_HasTimestampQueries (void)
{
	return (GL_GenQueriesFunc && GL_DeleteQueriesFunc
		&& GL_QueryCounterFunc
		&& GL_GetQueryObjectuivFunc
		&& GL_GetQueryObjectui64vFunc);
}

static void GLBackend_DetectCaps (void)
{
	GLint max_textures = 0;
	GLint max_samplers = 0;
	GLint max_ubos = 0;
	GLint max_ssbos = 0;
	unsigned sample;

	memset (&s_gl_backend_caps, 0, sizeof (s_gl_backend_caps));
	s_gl_backend_caps.supports_timestamps = GLBackend_HasTimestampQueries ();
	s_gl_backend_caps.supports_compute = (GL_DispatchComputeFunc != NULL);
	s_gl_backend_caps.supports_draw_instanced = (GL_DrawArraysInstancedFunc != NULL && GL_DrawElementsInstancedFunc != NULL);
	s_gl_backend_caps.supports_draw_indirect = (GL_DrawElementsIndirectFunc != NULL);
	s_gl_backend_caps.supports_multi_draw_indirect = (GL_MultiDrawElementsIndirectFunc != NULL);
	s_gl_backend_caps.supports_memory_barrier = (GL_MemoryBarrierFunc != NULL);
	s_gl_backend_caps.supports_bindless = gl_bindless_able;
	s_gl_backend_caps.shader_model = 50u;
	s_gl_backend_caps.max_msaa_samples = (framebufs.max_samples > 0) ? (unsigned)framebufs.max_samples : 1u;
	s_gl_backend_caps.msaa_mode_mask = 1u;
	for (sample = 2u; sample <= s_gl_backend_caps.max_msaa_samples && sample <= 32u; sample <<= 1)
		s_gl_backend_caps.msaa_mode_mask |= sample;

	glGetIntegerv (GL_MAX_TEXTURE_IMAGE_UNITS, &max_textures);
	glGetIntegerv (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_samplers);
	glGetIntegerv (GL_MAX_UNIFORM_BUFFER_BINDINGS, &max_ubos);
	glGetIntegerv (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &max_ssbos);

	s_gl_backend_caps.max_textures = (max_textures > 0) ? (unsigned)max_textures : 0u;
	s_gl_backend_caps.max_samplers = (max_samplers > 0) ? (unsigned)max_samplers : 0u;
	s_gl_backend_caps.max_ubos = (max_ubos > 0) ? (unsigned)max_ubos : 0u;
	s_gl_backend_caps.max_ssbos = (max_ssbos > 0) ? (unsigned)max_ssbos : 0u;
}

static const RenderBackendCaps *GLBackend_GetCaps (void)
{
	return &s_gl_backend_caps;
}

static GLenum GLBackend_MapPrimitive (render_backend_primitive_t primitive)
{
	switch (primitive)
	{
	case R_BACKEND_PRIMITIVE_TRIANGLE_FAN:
		return GL_TRIANGLE_FAN;
	case R_BACKEND_PRIMITIVE_TRIANGLE_STRIP:
		return GL_TRIANGLE_STRIP;
	case R_BACKEND_PRIMITIVE_LINES:
		return GL_LINES;
	case R_BACKEND_PRIMITIVE_POINTS:
		return GL_POINTS;
	case R_BACKEND_PRIMITIVE_TRIANGLES:
	default:
		return GL_TRIANGLES;
	}
}

static GLenum GLBackend_MapIndexType (render_backend_index_type_t index_type)
{
	switch (index_type)
	{
	case R_BACKEND_INDEX_TYPE_UINT32:
		return GL_UNSIGNED_INT;
	case R_BACKEND_INDEX_TYPE_UINT16:
	default:
		return GL_UNSIGNED_SHORT;
	}
}

static GLbitfield GLBackend_MapBarrierBits (unsigned barrier_bits)
{
	GLbitfield gl_bits = 0u;

	if (barrier_bits & R_BACKEND_BARRIER_TEXTURE_FETCH)
		gl_bits |= GL_TEXTURE_FETCH_BARRIER_BIT;
	if (barrier_bits & R_BACKEND_BARRIER_SHADER_IMAGE_ACCESS)
		gl_bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
	if (barrier_bits & R_BACKEND_BARRIER_SHADER_STORAGE)
		gl_bits |= GL_SHADER_STORAGE_BARRIER_BIT;
	if (barrier_bits & R_BACKEND_BARRIER_COMMAND)
		gl_bits |= GL_COMMAND_BARRIER_BIT;
	if (barrier_bits & R_BACKEND_BARRIER_ELEMENT_ARRAY)
		gl_bits |= GL_ELEMENT_ARRAY_BARRIER_BIT;
	if (barrier_bits & R_BACKEND_BARRIER_FRAMEBUFFER)
		gl_bits |= GL_FRAMEBUFFER_BARRIER_BIT;

	return gl_bits;
}

static GLenum GLBackend_MapBlendFactor (render_blend_factor_t factor)
{
	switch (factor)
	{
	case R_BLEND_FACTOR_ZERO:
		return GL_ZERO;
	case R_BLEND_FACTOR_ONE:
		return GL_ONE;
	case R_BLEND_FACTOR_SRC_ALPHA:
		return GL_SRC_ALPHA;
	case R_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return GL_ONE_MINUS_SRC_ALPHA;
	case R_BLEND_FACTOR_DST_ALPHA:
		return GL_DST_ALPHA;
	case R_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return GL_ONE_MINUS_DST_ALPHA;
	case R_BLEND_FACTOR_SRC_COLOR:
		return GL_SRC_COLOR;
	case R_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return GL_ONE_MINUS_SRC_COLOR;
	case R_BLEND_FACTOR_DST_COLOR:
		return GL_DST_COLOR;
	case R_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return GL_ONE_MINUS_DST_COLOR;
	case R_BLEND_FACTOR_INVALID:
	default:
		return GL_ONE;
	}
}

static GLenum GLBackend_MapDepthFunc (render_backend_depth_func_t depth_func)
{
	switch (depth_func)
	{
	case R_BACKEND_DEPTH_FUNC_LESS:
		return GL_LESS;
	case R_BACKEND_DEPTH_FUNC_EQUAL:
		return GL_EQUAL;
	case R_BACKEND_DEPTH_FUNC_GREATER:
		return GL_GREATER;
	case R_BACKEND_DEPTH_FUNC_GEQUAL:
		return GL_GEQUAL;
	case R_BACKEND_DEPTH_FUNC_ALWAYS:
		return GL_ALWAYS;
	case R_BACKEND_DEPTH_FUNC_NEVER:
		return GL_NEVER;
	case R_BACKEND_DEPTH_FUNC_LEQUAL:
	default:
		return GL_LEQUAL;
	}
}

static void GLBackend_BeginPass (const char *name)
{
	GL_BeginGroup (name);
}

static void GLBackend_EndPass (void)
{
	GL_EndGroup ();
}

static void GLBackend_BeginPassEx (const RenderBackendPassDesc *pass_desc)
{
	GLBackend_BeginPass ((pass_desc && pass_desc->name) ? pass_desc->name : "<unnamed>");
}

static void GLBackend_EndPassEx (void)
{
	GLBackend_EndPass ();
}

static void GLBackend_ResourceBarrier (const RenderGraphResourceHandle *resources, const RenderBackendResourceBarrier *barriers, unsigned count)
{
	unsigned i;
	qboolean needs_memory_barrier = false;
	(void)resources;

	if (!barriers || count == 0u)
		return;

	for (i = 0; i < count; ++i)
	{
		if (!barriers[i].resource)
			continue;
		if (barriers[i].before == R_BACKEND_RESOURCE_STATE_STORAGE_WRITE
			|| barriers[i].after == R_BACKEND_RESOURCE_STATE_SAMPLED
			|| barriers[i].after == R_BACKEND_RESOURCE_STATE_STORAGE_READ
			|| barriers[i].after == R_BACKEND_RESOURCE_STATE_STORAGE_WRITE)
		{
			needs_memory_barrier = true;
			break;
		}
	}

	if (needs_memory_barrier)
		R_Backend_MemoryBarrier (R_BACKEND_BARRIER_FRAMEBUFFER | R_BACKEND_BARRIER_TEXTURE_FETCH | R_BACKEND_BARRIER_SHADER_IMAGE_ACCESS);
}

static void GLBackend_ValidatePassState (const char *pass_name, qboolean before_pass)
{
	GLenum err;

	if (r_gl_state_validate.value <= 0.f)
		return;

	err = glGetError ();
	if (err != GL_NO_ERROR)
		Con_Warning ("FrameGraph %s(%s): GL error 0x%x\n", before_pass ? "before" : "after", pass_name, (unsigned)err);

	{
		GLint draw_fbo = 0;
		GLint viewport[4] = {0};
		glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
		glGetIntegerv (GL_VIEWPORT, viewport);
		if (viewport[2] <= 0 || viewport[3] <= 0)
			Con_Warning ("FrameGraph %s(%s): invalid viewport %d %d %d %d\n",
				before_pass ? "before" : "after",
				pass_name,
				viewport[0], viewport[1], viewport[2], viewport[3]);
		if (draw_fbo && GL_CheckFramebufferStatusFunc)
		{
			GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE)
				Con_Warning ("FrameGraph %s(%s): incomplete FBO %d status=0x%x\n",
					before_pass ? "before" : "after",
					pass_name, draw_fbo, (unsigned)status);
		}
	}

	if (gl_clipcontrol_able)
	{
		GLint clip_depth_mode = GL_ZERO_TO_ONE;
		glGetIntegerv (GL_CLIP_DEPTH_MODE, &clip_depth_mode);
		if (clip_depth_mode != GL_ZERO_TO_ONE && clip_depth_mode != GL_NEGATIVE_ONE_TO_ONE)
			Con_Warning ("FrameGraph %s(%s): invalid clip depth mode 0x%x\n",
				before_pass ? "before" : "after",
				pass_name, (unsigned)clip_depth_mode);
	}
}

static void GLBackend_BeginTimer (int pass_id)
{
	gl_backend_timer_slot_t *slot;
	gl_backend_timer_pass_t *stats;
	int attempts;
	int slot_index;

	if (!s_gl_backend_caps.supports_timestamps)
		return;
	if (pass_id < 0 || pass_id >= R_BACKEND_MAX_PROFILE_SLOTS)
		return;

	stats = &s_timer_passes[pass_id];
	if (stats->timer_active_slot_plus_one != 0)
		return;

	for (attempts = 0; attempts < GL_BACKEND_TIMER_QUERY_RING; ++attempts)
	{
		slot_index = (stats->timer_write_index + attempts) % GL_BACKEND_TIMER_QUERY_RING;
		slot = &stats->timer_slots[slot_index];
		if (slot->issued)
			continue;

		if (!slot->start_query || !slot->end_query)
		{
			GL_GenQueriesFunc (1, &slot->start_query);
			GL_GenQueriesFunc (1, &slot->end_query);
			if (!slot->start_query || !slot->end_query)
			{
				if (slot->start_query)
					GL_DeleteQueriesFunc (1, &slot->start_query);
				if (slot->end_query)
					GL_DeleteQueriesFunc (1, &slot->end_query);
				slot->start_query = 0;
				slot->end_query = 0;
				return;
			}
		}

		stats->timer_active_slot_plus_one = slot_index + 1;
		GL_QueryCounterFunc (slot->start_query, GL_TIMESTAMP);
		return;
	}
}

static void GLBackend_EndTimer (int pass_id)
{
	gl_backend_timer_slot_t *slot;
	gl_backend_timer_pass_t *stats;
	int slot_index;

	if (!s_gl_backend_caps.supports_timestamps)
		return;
	if (pass_id < 0 || pass_id >= R_BACKEND_MAX_PROFILE_SLOTS)
		return;

	stats = &s_timer_passes[pass_id];
	if (stats->timer_active_slot_plus_one == 0)
		return;

	slot_index = stats->timer_active_slot_plus_one - 1;
	slot = &stats->timer_slots[slot_index];

	if (!slot->start_query || !slot->end_query)
	{
		stats->timer_active_slot_plus_one = 0;
		return;
	}

	GL_QueryCounterFunc (slot->end_query, GL_TIMESTAMP);
	slot->issued = true;
	slot->frame_index = r_framecount;
	stats->timer_active_slot_plus_one = 0;
	stats->timer_write_index = (slot_index + 1) % GL_BACKEND_TIMER_QUERY_RING;
}

static void GLBackend_ResolveTimers (void)
{
	int pass_id;
	int slot_index;

	if (!s_gl_backend_caps.supports_timestamps)
		return;

	for (pass_id = 0; pass_id < R_BACKEND_MAX_PROFILE_SLOTS; ++pass_id)
	{
		gl_backend_timer_pass_t *stats = &s_timer_passes[pass_id];

		for (slot_index = 0; slot_index < GL_BACKEND_TIMER_QUERY_RING; ++slot_index)
		{
			gl_backend_timer_slot_t *slot = &stats->timer_slots[slot_index];
			GLuint available = 0;
			GLuint64 start_ns = 0;
			GLuint64 end_ns = 0;
			double gpu_ms;

			if (!slot->issued || !slot->start_query || !slot->end_query)
				continue;
			if (slot->frame_index > r_framecount - GL_BACKEND_TIMER_QUERY_RING)
				continue;

			GL_GetQueryObjectuivFunc (slot->end_query, GL_QUERY_RESULT_AVAILABLE, &available);
			if (!available)
				continue;

			GL_GetQueryObjectui64vFunc (slot->start_query, GL_QUERY_RESULT, &start_ns);
			GL_GetQueryObjectui64vFunc (slot->end_query, GL_QUERY_RESULT, &end_ns);

			if (end_ns >= start_ns)
				gpu_ms = (double)(end_ns - start_ns) / 1000000.0;
			else
				gpu_ms = 0.0;

			stats->pending_sample = true;
			stats->pending_gpu_ms = gpu_ms;
			slot->issued = false;
		}
	}
}

static qboolean GLBackend_ConsumeTimerSample (int pass_id, double *out_gpu_ms)
{
	gl_backend_timer_pass_t *stats;

	if (pass_id < 0 || pass_id >= R_BACKEND_MAX_PROFILE_SLOTS)
		return false;

	stats = &s_timer_passes[pass_id];
	if (!stats->pending_sample)
		return false;

	if (out_gpu_ms)
		*out_gpu_ms = stats->pending_gpu_ms;
	stats->pending_sample = false;
	return true;
}

static unsigned GLBackend_ResolveResourceId (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource)
{
	if (!resource || resource->type == R_BACKEND_RESOURCE_NONE)
		return 0u;

	if (resource->opaque_id != 0u)
		return GLBackend_ResolveResourceOpaqueId (resources, resource->opaque_id);
	return 0u;
}

static qboolean GLBackend_IsResourceValid (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource)
{
	return GLBackend_ResolveResourceId (resources, resource) != 0u;
}

static void GLBackend_BindRenderTarget (const RenderGraphResourceHandle *resources, const render_backend_resource_ref_t *resource, qboolean backbuffer)
{
	GLuint target_fbo = 0;

	if (!backbuffer)
		target_fbo = (GLuint)GLBackend_ResolveResourceId (resources, resource);

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, target_fbo);
	if (target_fbo == 0)
	{
		glDrawBuffer (GL_BACK);
		glReadBuffer (GL_BACK);
	}
	else
	{
		glDrawBuffer (GL_COLOR_ATTACHMENT0);
		glReadBuffer (GL_COLOR_ATTACHMENT0);
	}
}

static void GLBackend_SetViewport (int x, int y, int width, int height)
{
	glViewport (x, y, width, height);
}

static void GLBackend_SetScissor (qboolean enabled, int x, int y, int width, int height)
{
	if (enabled)
	{
		glEnable (GL_SCISSOR_TEST);
		glScissor (x, y, width, height);
	}
	else
	{
		glDisable (GL_SCISSOR_TEST);
	}
}

static void GLBackend_SetPipelineState (unsigned state_bits)
{
	GL_SetState (state_bits);
}

static void GLBackend_BindPipeline (const RenderBackendPipelineDesc *pipeline)
{
	if (!pipeline)
		return;
	if (pipeline->pipeline_id != 0u)
		GL_UseProgram ((GLuint)pipeline->pipeline_id);
	GLBackend_SetPipelineState (pipeline->state_bits);
}

static void GLBackend_SetDynamicState (const RenderBackendDynamicState *dynamic_state)
{
	unsigned state_bits;

	if (!dynamic_state)
		return;

	state_bits = 0u;
	state_bits |= dynamic_state->blend_state & GLS_MASK_BLEND;
	state_bits |= dynamic_state->depth_state & (GLS_NO_ZTEST | GLS_NO_ZWRITE);
	state_bits |= dynamic_state->raster_state & (GLS_MASK_CULL | GLS_MASK_ATTRIBS | GLS_MASK_INSTANCED_ATTRIBS);
	GLBackend_SetPipelineState (state_bits);
}

static void GLBackend_BindDescriptors (const RenderBackendDescriptorBinding *bindings, unsigned count)
{
	unsigned i;
	unsigned max_textures = s_gl_backend_caps.max_textures;
	unsigned max_ubos = s_gl_backend_caps.max_ubos;
	unsigned max_ssbos = s_gl_backend_caps.max_ssbos;

	if (!bindings || count == 0u)
		return;

	for (i = 0; i < count; ++i)
	{
		const RenderBackendDescriptorBinding *binding = &bindings[i];
		switch (binding->type)
		{
		case R_BACKEND_DESCRIPTOR_TEXTURE:
		case R_BACKEND_DESCRIPTOR_SAMPLER:
			if (max_textures > 0u && binding->slot >= max_textures)
			{
				Con_DWarning ("GL descriptor bind out of range: type=%u slot=%u max_textures=%u\n",
					(unsigned)binding->type, binding->slot, max_textures);
				SDL_assert (!"GL descriptor texture/sampler slot out of range");
				break;
			}
			if (binding->type == R_BACKEND_DESCRIPTOR_TEXTURE)
				GL_BindNative (GL_TEXTURE0 + binding->slot, GL_TEXTURE_2D, (GLuint)binding->resource_id);
			else if (GL_BindSamplerFunc)
				GL_BindSamplerFunc (binding->slot, (GLuint)binding->resource_id);
			break;
		case R_BACKEND_DESCRIPTOR_UNIFORM_BUFFER:
			if (max_ubos > 0u && binding->slot >= max_ubos)
			{
				Con_DWarning ("GL descriptor bind out of range: UBO slot=%u max_ubos=%u\n",
					binding->slot, max_ubos);
				SDL_assert (!"GL descriptor UBO slot out of range");
			}
			if (binding->range == 0u)
			{
				Con_DWarning ("GL descriptor UBO binding requires range > 0 (slot=%u resource=%u)\n",
					binding->slot, binding->resource_id);
				SDL_assert (!"GL descriptor UBO binding missing range");
				break;
			}
			GL_BindBufferRange (GL_UNIFORM_BUFFER, binding->slot, (GLuint)binding->resource_id, (GLintptr)binding->offset, (GLsizeiptr)binding->range);
			break;
		case R_BACKEND_DESCRIPTOR_STORAGE_BUFFER:
			if (max_ssbos > 0u && binding->slot >= max_ssbos)
			{
				Con_DWarning ("GL descriptor bind out of range: SSBO slot=%u max_ssbos=%u\n",
					binding->slot, max_ssbos);
				SDL_assert (!"GL descriptor SSBO slot out of range");
			}
			if (binding->range == 0u)
			{
				Con_DWarning ("GL descriptor SSBO binding requires range > 0 (slot=%u resource=%u)\n",
					binding->slot, binding->resource_id);
				SDL_assert (!"GL descriptor SSBO binding missing range");
				break;
			}
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, binding->slot, (GLuint)binding->resource_id, (GLintptr)binding->offset, (GLsizeiptr)binding->range);
			break;
		default:
			Con_DWarning ("GL descriptor bind has unknown descriptor type=%u (slot=%u)\n",
				(unsigned)binding->type, binding->slot);
			SDL_assert (!"GL descriptor unknown descriptor type");
			break;
		}
	}
}

static void GLBackend_Draw (render_backend_primitive_t primitive, int first, int count)
{
	glDrawArrays (GLBackend_MapPrimitive (primitive), first, count);
}

static void GLBackend_DrawIndexed (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes)
{
	glDrawElements (GLBackend_MapPrimitive (primitive), count, GLBackend_MapIndexType (index_type), (const GLvoid *)index_offset_bytes);
}

static void GLBackend_DrawInstanced (render_backend_primitive_t primitive, int first, int count, int instance_count)
{
	if (!GL_DrawArraysInstancedFunc || instance_count <= 0)
		return;

	GL_DrawArraysInstancedFunc (GLBackend_MapPrimitive (primitive), first, count, instance_count);
}

static void GLBackend_DrawIndexedInstanced (render_backend_primitive_t primitive, render_backend_index_type_t index_type, int count, intptr_t index_offset_bytes, int instance_count)
{
	if (!GL_DrawElementsInstancedFunc || instance_count <= 0)
		return;

	GL_DrawElementsInstancedFunc (GLBackend_MapPrimitive (primitive), count, GLBackend_MapIndexType (index_type), (const GLvoid *)index_offset_bytes, instance_count);
}

static void GLBackend_DrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes)
{
	if (!GL_DrawElementsIndirectFunc)
		return;

	GL_DrawElementsIndirectFunc (GLBackend_MapPrimitive (primitive), GLBackend_MapIndexType (index_type), (const void *)indirect_offset_bytes);
}

static void GLBackend_MultiDrawIndexedIndirect (render_backend_primitive_t primitive, render_backend_index_type_t index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes)
{
	if (draw_count <= 0)
		return;

	if (GL_MultiDrawElementsIndirectFunc)
	{
		GL_MultiDrawElementsIndirectFunc (
			GLBackend_MapPrimitive (primitive),
			GLBackend_MapIndexType (index_type),
			(const void *)indirect_offset_bytes,
			draw_count,
			stride_bytes);
		return;
	}

	if (GL_DrawElementsIndirectFunc)
	{
		int i;
		const byte *base = (const byte *)indirect_offset_bytes;
		const int stride = (stride_bytes > 0) ? stride_bytes : (int)sizeof (unsigned) * 5;

		for (i = 0; i < draw_count; ++i)
		{
			GL_DrawElementsIndirectFunc (
				GLBackend_MapPrimitive (primitive),
				GLBackend_MapIndexType (index_type),
				base + (i * stride));
		}
	}
}

static void GLBackend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z)
{
	if (GL_DispatchComputeFunc)
		GL_DispatchComputeFunc (group_x, group_y, group_z);
}

static void GLBackend_MemoryBarrier (unsigned barrier_bits)
{
	const GLbitfield gl_bits = GLBackend_MapBarrierBits (barrier_bits);

	if (!GL_MemoryBarrierFunc || gl_bits == 0u)
		return;

	GL_MemoryBarrierFunc (gl_bits);
}

static void GLBackend_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst)
{
	GLenum gl_src = GLBackend_MapBlendFactor (src);
	GLenum gl_dst = GLBackend_MapBlendFactor (dst);
	glBlendFunc (gl_src, gl_dst);
}

static void GLBackend_SetDepthFunc (render_backend_depth_func_t depth_func)
{
	glDepthFunc (GLBackend_MapDepthFunc (depth_func));
}

static unsigned GLBackend_CreatePostFXLUTTexture (void)
{
	GLuint texture_id = 0;
	glGenTextures (1, &texture_id);
	return (unsigned)texture_id;
}

static void GLBackend_ConfigurePostFXLUTTexture (unsigned texture_id)
{
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D_ARRAY, (GLuint)texture_id);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void GLBackend_Finish (void)
{
	glFinish ();
}

static void GLBackend_PassSetupView (RenderPassContext *ctx)
{
	(void)ctx;
	R_SetupView ();
}

static void GLBackend_PassShadowMaps (RenderPassContext *ctx)
{
	(void)ctx;
	R_RenderShadowMaps ();
}

static void GLBackend_PassRenderScene (RenderPassContext *ctx)
{
	R_RenderScene (ctx ? ctx->resources : NULL);
}

static const RenderGraphResourceHandle *GLBackend_GetPassResourcesOrFallback (const RenderPassContext *ctx, RenderGraphResourceHandle *fallback_resources, const char *pass_name)
{
	if (ctx && ctx->resources)
		return ctx->resources;
	if (!fallback_resources)
		return NULL;

	memset (fallback_resources, 0, sizeof (*fallback_resources));
	GLBackend_PopulateFrameGraphResources (fallback_resources);
	if (r_framegraph_debug.value > 0.f && s_legacy_pass_resource_fallback_frame != r_framecount)
	{
		Con_DWarning ("FrameGraph seam: '%s' ran without pass resources; using backend fallback handles\n",
			pass_name ? pass_name : "<unnamed>");
		s_legacy_pass_resource_fallback_frame = r_framecount;
	}
	return fallback_resources;
}

static void GLBackend_PassWarpResolve (RenderPassContext *ctx)
{
	RenderGraphResourceHandle fallback_resources;
	const RenderGraphResourceHandle *resources = GLBackend_GetPassResourcesOrFallback (ctx, &fallback_resources, "Warp/resolve");
	R_WarpScaleView (resources);
}

static void GLBackend_PassPostProcess (RenderPassContext *ctx)
{
	RenderGraphResourceHandle fallback_resources;
	const RenderGraphResourceHandle *resources = GLBackend_GetPassResourcesOrFallback (ctx, &fallback_resources, "Postprocess");
	GL_PostProcess (resources);
}

static void GLBackend_PassOverlayViewmodel (RenderPassContext *ctx)
{
	(void)ctx;
	R_DrawViewModel ();
}

static void GLBackend_PassOverlayPolyblend (RenderPassContext *ctx)
{
	(void)ctx;
	V_PolyBlend ();
}

static qboolean GLBackend_HasRequiredPassCallbacks (void)
{
	return true;
}

static qboolean GLBackend_Init (void)
{
	GL_Backend_ResetResources ();
	return true;
}

static void GLBackend_Shutdown (void)
{
	GL_Backend_ResetResources ();
}

static void GLBackend_OnResize (int width, int height)
{
	(void)width;
	(void)height;
}

static qboolean GLBackend_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return true;
}

static void GLBackend_BeginFrame (void)
{
	if ((!s_gl_backend_caps.supports_compute && GL_DispatchComputeFunc)
		|| (!s_gl_backend_caps.supports_draw_instanced && GL_DrawArraysInstancedFunc && GL_DrawElementsInstancedFunc)
		|| (!s_gl_backend_caps.supports_draw_indirect && GL_DrawElementsIndirectFunc)
		|| (!s_gl_backend_caps.supports_multi_draw_indirect && GL_MultiDrawElementsIndirectFunc)
		|| (!s_gl_backend_caps.supports_memory_barrier && GL_MemoryBarrierFunc))
	{
		/* VID resize can initialize backend registration before GL extension
		 * entry points are loaded. Refresh caps lazily on first render frame. */
		GLBackend_DetectCaps ();
	}

	GL_BackendBeginFrame ();
}

static void GLBackend_EndFrame (void)
{
	GL_BackendEndFrame ();
}

static void GLBackend_Present (void)
{
	GL_BackendPresent ();
}

static const IRenderBackend s_gl_backend = {
	"OpenGL",
	GLBackend_Init,
	GLBackend_Shutdown,
	GLBackend_OnResize,
	GLBackend_CanActivate,
	GLBackend_BeginFrame,
	GLBackend_EndFrame,
	GLBackend_Present,
	GLBackend_BeginPassEx,
	GLBackend_EndPassEx,
	GLBackend_ResourceBarrier,
	GLBackend_BindPipeline,
	GLBackend_SetDynamicState,
	GLBackend_BindDescriptors,
	GLBackend_PassSetupView,
	GLBackend_PassShadowMaps,
	GLBackend_PassRenderScene,
	GLBackend_PassWarpResolve,
	GLBackend_PassPostProcess,
	GLBackend_PassOverlayViewmodel,
	GLBackend_PassOverlayPolyblend,
	GLBackend_HasRequiredPassCallbacks,
	GLBackend_BeginPass,
	GLBackend_EndPass,
	GLBackend_ValidatePassState,
	GLBackend_BeginTimer,
	GLBackend_EndTimer,
	GLBackend_ResolveTimers,
	GLBackend_ConsumeTimerSample,
	GLBackend_GetCaps,
	GLBackend_ResolveResourceId,
	GLBackend_IsResourceValid,
	GLBackend_BindRenderTarget,
	GLBackend_SetViewport,
	GLBackend_SetScissor,
	GLBackend_SetPipelineState,
	GLBackend_Draw,
	GLBackend_DrawIndexed,
	GLBackend_DrawInstanced,
	GLBackend_DrawIndexedInstanced,
	GLBackend_DrawIndexedIndirect,
	GLBackend_MultiDrawIndexedIndirect,
	GLBackend_Dispatch,
	GLBackend_MemoryBarrier,
	GLBackend_SetBlendFactors,
	GLBackend_SetDepthFunc,
	GLBackend_CreatePostFXLUTTexture,
	GLBackend_ConfigurePostFXLUTTexture,
	GLBackend_Finish,
	GLBackend_QuerySurfaceMetrics,
	GLBackend_NeedsSceneEffects,
	GLBackend_NeedsPostprocess,
	GLBackend_PopulateFrameGraphResources,
	GLBackend_GetSceneSampleCount
};

const IRenderBackend *GL_Backend_GetInterface (void)
{
	GLBackend_DetectCaps ();
	return &s_gl_backend;
}

const IRenderBackend *IW_RendererPlugin_GetBuiltinOpenGLBackend (void)
{
	return GL_Backend_GetInterface ();
}

void GL_Backend_Register (void)
{
	R_Backend_Register (GL_Backend_GetInterface ());
}
