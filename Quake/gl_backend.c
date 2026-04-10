#include "quakedef.h"

#include "r_framegraph.h"

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
	GL_BACKEND_TIMER_QUERY_RING = 3
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
	s_gl_backend_caps.supports_bindless = gl_bindless_able;
	s_gl_backend_caps.shader_model = 50u;
	s_gl_backend_caps.max_msaa_samples = (framebufs.max_samples > 0) ? (unsigned)framebufs.max_samples : 1u;
	s_gl_backend_caps.msaa_mode_mask = 1u;
	for (sample = 2u; sample <= s_gl_backend_caps.max_msaa_samples && sample <= 32u; sample <<= 1)
		s_gl_backend_caps.msaa_mode_mask |= sample;

	glGetIntegerv (GL_MAX_TEXTURE_IMAGE_UNITS, &max_textures);
	glGetIntegerv (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_samplers);
	if (GL_GetIntegervFunc)
	{
		GL_GetIntegervFunc (GL_MAX_UNIFORM_BUFFER_BINDINGS, &max_ubos);
		GL_GetIntegervFunc (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &max_ssbos);
	}

	s_gl_backend_caps.max_textures = (max_textures > 0) ? (unsigned)max_textures : 0u;
	s_gl_backend_caps.max_samplers = (max_samplers > 0) ? (unsigned)max_samplers : 0u;
	s_gl_backend_caps.max_ubos = (max_ubos > 0) ? (unsigned)max_ubos : 0u;
	s_gl_backend_caps.max_ssbos = (max_ssbos > 0) ? (unsigned)max_ssbos : 0u;
}

static const RenderBackendCaps *GLBackend_GetCaps (void)
{
	return &s_gl_backend_caps;
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

static void GLBackend_BeginPass (const char *name)
{
	GL_BeginGroup (name);
}

static void GLBackend_EndPass (void)
{
	GL_EndGroup ();
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
	render_backend_resource_slot_t slot;

	(void)resources;

	if (!resource || resource->type == R_BACKEND_RESOURCE_NONE)
		return 0u;

	slot = (render_backend_resource_slot_t)resource->slot;
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
		return (framebufs.scene.samples > 1) ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
	case R_BACKEND_RESOURCE_SLOT_NONE:
	case R_BACKEND_RESOURCE_SLOT_COUNT:
	default:
		return 0u;
	}
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

static void GLBackend_Draw (render_backend_primitive_t primitive, int first, int count)
{
	GLenum mode = GL_TRIANGLES;

	switch (primitive)
	{
	case R_BACKEND_PRIMITIVE_TRIANGLE_FAN:
		mode = GL_TRIANGLE_FAN;
		break;
	case R_BACKEND_PRIMITIVE_LINES:
		mode = GL_LINES;
		break;
	case R_BACKEND_PRIMITIVE_POINTS:
		mode = GL_POINTS;
		break;
	case R_BACKEND_PRIMITIVE_TRIANGLES:
	default:
		mode = GL_TRIANGLES;
		break;
	}

	glDrawArrays (mode, first, count);
}

static void GLBackend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z)
{
	if (s_gl_backend_caps.supports_compute)
		GL_DispatchComputeFunc (group_x, group_y, group_z);
}

static void GLBackend_SetBlendFactors (render_blend_factor_t src, render_blend_factor_t dst)
{
	GLenum gl_src = GLBackend_MapBlendFactor (src);
	GLenum gl_dst = GLBackend_MapBlendFactor (dst);
	glBlendFunc (gl_src, gl_dst);
}

static void GLBackend_Finish (void)
{
	glFinish ();
}

static qboolean GLBackend_Init (void)
{
	return true;
}

static void GLBackend_Shutdown (void)
{
}

static void GLBackend_OnResize (int width, int height)
{
	(void)width;
	(void)height;
}

static qboolean GLBackend_CanActivate (qboolean runtime_switch)
{
	return !runtime_switch;
}

void GL_Backend_Register (void)
{
	GLBackend_DetectCaps ();

	static const IRenderBackend gl_backend = {
		"OpenGL",
		GLBackend_Init,
		GLBackend_Shutdown,
		GLBackend_OnResize,
		GLBackend_CanActivate,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
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
		GLBackend_Dispatch,
		GLBackend_SetBlendFactors,
		GLBackend_Finish
	};

	R_Backend_Register (&gl_backend);
}
