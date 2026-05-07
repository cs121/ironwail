#include "quakedef.h"
#include "glquake.h"
#include "gl_backend.h"
#include "r_ssao.h"
#include "r_postfx.h"
#include "r_shadow_debug.h"

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
static int s_missing_pass_resource_log_frame = -1;
static int s_refgl_feature_log_frame = -1;
static int s_backend_shadow_skip_log_frame = -1;
static int s_backend_postfx_skip_log_frame = -1;
static int s_backend_viewmodel_skip_log_frame = -1;
static int s_backend_polyblend_skip_log_frame = -1;
static int s_backend_missing_frameplan_log_frame = -1;

extern cvar_t r_refgl_debug;
extern cvar_t r_refgl_log_init;
extern cvar_t r_refgl_log_passes;
extern cvar_t r_refgl_log_resources;
extern cvar_t r_refgl_log_state;
extern cvar_t r_refgl_validate_state;
extern cvar_t r_refgl_validate_fbo;
extern cvar_t r_refgl_validate_lifetime;
extern cvar_t r_ref_enable_postfx;
extern cvar_t r_ref_enable_shadows;
extern cvar_t r_ref_enable_fog;
extern cvar_t r_ref_enable_lighting;

static qboolean GLBackend_ShouldLogPasses (void)
{
	return (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f || (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_BACKEND)));
}

static qboolean GLBackend_LogOncePerFrame (int *last_frame)
{
	if (!last_frame || !GLBackend_ShouldLogPasses ())
		return false;
	if (*last_frame == r_framecount)
		return false;
	*last_frame = r_framecount;
	return true;
}

static qboolean GLBackend_RequireFramePlan (const RenderPassContext *ctx, const char *pass_name)
{
	if (ctx && ctx->frame_plan)
		return true;

	if (GLBackend_LogOncePerFrame (&s_backend_missing_frameplan_log_frame))
	{
		Con_DWarning ("FrameGraph contract: backend pass '%s' missing frame plan; skipping callback\n",
			pass_name ? pass_name : "<unnamed>");
	}
	return false;
}

void GL_Backend_PopulateResourceRegistry (RenderGraphResourceHandle *out_handles);

ref_gl_stats_t ref_gl_stats;
static int s_stats_log_frame = -1;

void REFGL_StatsLogSummary (void)
{
	if (ref_gl_stats.textures_alive < 0) ref_gl_stats.textures_alive = 0;
	if (ref_gl_stats.fbos_alive < 0) ref_gl_stats.fbos_alive = 0;
	if (ref_gl_stats.buffers_alive < 0) ref_gl_stats.buffers_alive = 0;
	if (ref_gl_stats.programs_alive < 0) ref_gl_stats.programs_alive = 0;
	Con_Printf ("ref_gl resources: tex=%d(%d/%d) fbo=%d(%d/%d) buf=%d(%d/%d) prog=%d(%d/%d) gl_err=%d frames=%d\n",
		ref_gl_stats.textures_alive, ref_gl_stats.textures_created, ref_gl_stats.textures_destroyed,
		ref_gl_stats.fbos_alive, ref_gl_stats.fbos_created, ref_gl_stats.fbos_destroyed,
		ref_gl_stats.buffers_alive, ref_gl_stats.buffers_created, ref_gl_stats.buffers_destroyed,
		ref_gl_stats.programs_alive, ref_gl_stats.programs_created, ref_gl_stats.programs_destroyed,
		ref_gl_stats.gl_errors_detected, ref_gl_stats.frames_rendered);
	DBG_VERBOSE(DBG_CH_BACKEND, "ref_gl resources: tex=%d fbo=%d buf=%d prog=%d gl_err=%d frames=%d",
		ref_gl_stats.textures_alive, ref_gl_stats.fbos_alive, ref_gl_stats.buffers_alive,
		ref_gl_stats.programs_alive, ref_gl_stats.gl_errors_detected, ref_gl_stats.frames_rendered);
}

static void REFGL_StatsPeriodicLog (void)
{
	if (r_refgl_log_resources.value == 0.f && !(debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_BACKEND)))
		return;
	if (s_stats_log_frame < 0 || r_framecount - s_stats_log_frame >= 120)
	{
		s_stats_log_frame = r_framecount;
		REFGL_StatsLogSummary ();
	}
}

static unsigned GLBackend_ResolveResourceOpaqueId (const RenderGraphResourceHandle *resources, unsigned short opaque_id)
{
	unsigned native_id = GL_Backend_ResolveOpaqueResource (opaque_id);
	unsigned i;

	if (native_id != 0u || !resources || opaque_id == 0u)
		return native_id;

	/* Cross-module safety: when host and renderer module keep separate opaque-id
	 * tables, resolve via the per-frame registry snapshot supplied by framegraph. */
	for (i = 0; i < (unsigned)resources->registry_count; ++i)
	{
		if (resources->registry[i].resource_id != (unsigned)opaque_id)
			continue;
		return resources->registry[i].native_id;
	}

	return 0u;
}

static void GLBackend_PopulateFrameGraphResources (RenderGraphResourceHandle *out_handles)
{
	GL_Backend_PopulateResourceRegistry (out_handles);
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
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: begin pass '%s'\n", name ? name : "<unnamed>");
	GL_BeginGroup (name);
}

static void GLBackend_EndPass (void)
{
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: end pass\n");
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
	typedef struct gl_backend_pass_contract_s
	{
		const char *name;
		qboolean expect_depth_test;
		qboolean expect_depth_write;
		qboolean expect_blend;
		qboolean expect_cull;
		qboolean expect_stencil_test;
		qboolean expect_scissor_test;
	} gl_backend_pass_contract_t;
	static const gl_backend_pass_contract_t contracts[] = {
		{"Setup view", true, true, false, true, false, false},
		{"Shadow maps", true, true, false, true, false, false},
		{"Render scene", true, true, false, true, false, false},
		{"Warp/resolve", false, false, false, false, false, false},
		{"Capture fog handoff", false, false, true, false, false, false},
		{"Postprocess", false, false, false, false, false, false},
		{"Draw viewmodel", true, true, false, true, false, false},
		{"Polyblend", false, false, true, false, false, false},
		{"Store previous frame", false, false, false, false, false, false},
		{"Update decals", true, true, true, true, false, false},
		{NULL, false, false, false, false, false, false}
	};
	const gl_backend_pass_contract_t *contract = NULL;
	unsigned i;
	GLenum err;

	if (r_gl_state_validate.value <= 0.f)
		return;

	if (r_refgl_validate_state.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: validate pass state %s(%s)\n",
			before_pass ? "before" : "after",
			pass_name ? pass_name : "<unnamed>");

	err = glGetError ();
	if (err != GL_NO_ERROR)
	{
		ref_gl_stats.gl_errors_detected++;
		Con_Warning ("FrameGraph %s(%s): GL error 0x%x\n", before_pass ? "before" : "after", pass_name, (unsigned)err);
	}

	{
		GLint draw_fbo = 0;
		GLint viewport[4] = {0};
		GLint scissor_box[4] = {0};
		GLboolean depth_test = GL_FALSE;
		GLboolean blend = GL_FALSE;
		GLboolean cull = GL_FALSE;
		GLboolean stencil_test = GL_FALSE;
		GLboolean scissor_test = GL_FALSE;
		GLboolean depth_write = GL_FALSE;
		GLboolean color_mask[4] = {GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
		glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
		glGetIntegerv (GL_VIEWPORT, viewport);
		glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
		glGetBooleanv (GL_DEPTH_TEST, &depth_test);
		glGetBooleanv (GL_BLEND, &blend);
		glGetBooleanv (GL_CULL_FACE, &cull);
		glGetBooleanv (GL_STENCIL_TEST, &stencil_test);
		glGetBooleanv (GL_SCISSOR_TEST, &scissor_test);
		glGetBooleanv (GL_DEPTH_WRITEMASK, &depth_write);
		glGetBooleanv (GL_COLOR_WRITEMASK, color_mask);

		for (i = 0; contracts[i].name; ++i)
		{
			if (!q_strcasecmp (pass_name ? pass_name : "", contracts[i].name))
			{
				contract = &contracts[i];
				break;
			}
		}

		if (viewport[2] <= 0 || viewport[3] <= 0)
			Con_Warning ("FrameGraph %s(%s): invalid viewport %d %d %d %d\n",
				before_pass ? "before" : "after",
				pass_name,
				viewport[0], viewport[1], viewport[2], viewport[3]);
		if (draw_fbo && GL_CheckFramebufferStatusFunc
			&& (r_refgl_validate_fbo.value != 0.f || r_refgl_validate_state.value != 0.f || r_refgl_debug.value != 0.f))
		{
			GLenum status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE)
				Con_Warning ("FrameGraph %s(%s): incomplete FBO %d status=0x%x\n",
					before_pass ? "before" : "after",
					pass_name, draw_fbo, (unsigned)status);
		}

		if (contract)
		{
			if ((depth_test != GL_FALSE) != contract->expect_depth_test
				|| (depth_write != GL_FALSE) != contract->expect_depth_write
				|| (blend != GL_FALSE) != contract->expect_blend
				|| (cull != GL_FALSE) != contract->expect_cull
				|| (stencil_test != GL_FALSE) != contract->expect_stencil_test
				|| (scissor_test != GL_FALSE) != contract->expect_scissor_test)
			{
				Con_Warning ("FrameGraph %s(%s): state contract mismatch exp[d=%d dw=%d b=%d c=%d s=%d sc=%d] got[d=%d dw=%d b=%d c=%d s=%d sc=%d] vp=[%d %d %d %d] scissor=[%d %d %d %d] colormask=[%d %d %d %d]\n",
					before_pass ? "before" : "after",
					pass_name,
					contract->expect_depth_test ? 1 : 0,
					contract->expect_depth_write ? 1 : 0,
					contract->expect_blend ? 1 : 0,
					contract->expect_cull ? 1 : 0,
					contract->expect_stencil_test ? 1 : 0,
					contract->expect_scissor_test ? 1 : 0,
					depth_test != GL_FALSE ? 1 : 0,
					depth_write != GL_FALSE ? 1 : 0,
					blend != GL_FALSE ? 1 : 0,
					cull != GL_FALSE ? 1 : 0,
					stencil_test != GL_FALSE ? 1 : 0,
					scissor_test != GL_FALSE ? 1 : 0,
					viewport[0], viewport[1], viewport[2], viewport[3],
					scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
					color_mask[0] != GL_FALSE ? 1 : 0,
					color_mask[1] != GL_FALSE ? 1 : 0,
					color_mask[2] != GL_FALSE ? 1 : 0,
					color_mask[3] != GL_FALSE ? 1 : 0);
			}
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
	GL_Backend_SetViewportCached (x, y, width, height);
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

void GL_Backend_UploadPostFXLUTData (unsigned texture_id, const void *data, int width, int height, int layer_count)
{
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D_ARRAY, (GLuint)texture_id);
	GL_TexImage3DFunc (GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width, height, layer_count, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	if (GL_ObjectLabelFunc)
		GL_ObjectLabelFunc (GL_TEXTURE, (GLuint)texture_id, -1, "postfx lut");
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
	static qboolean shadow_pass_diag_once = false;

	if (!shadow_pass_diag_once)
	{
		shadow_pass_diag_once = true;
		if (SHADOW_LOG_ENABLED())
			Con_Printf ("Shadow backend diag: entered callback run_shadowmaps=%d r_ref_enable_shadows=%.2f\n",
			(ctx && ctx->frame_plan && ctx->frame_plan->run_shadowmaps) ? 1 : 0,
			r_ref_enable_shadows.value);
	}

	if (!GLBackend_RequireFramePlan (ctx, "Shadow maps"))
		return;

	if (!ctx->frame_plan->run_shadowmaps)
	{
		if (GLBackend_LogOncePerFrame (&s_backend_shadow_skip_log_frame))
		{
			Con_DPrintf ("ref_gl: skipping backend Shadow maps callback (run_shadowmaps=0)\n");
		}
		return;
	}

	if (r_ref_enable_shadows.value == 0.f)
	{
		if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
			Con_DPrintf ("ref_gl: skipping pass Shadow maps (r_ref_enable_shadows=0)\n");
		return;
	}
	(void)ctx;
	R_RenderShadowMaps ();
}

static void GLBackend_PassRenderScene (RenderPassContext *ctx)
{
	r_render_scene_input_t input;
	scene_size_info_t scene_size;

	if (!GLBackend_RequireFramePlan (ctx, "Render scene"))
		return;

	memset (&input, 0, sizeof (input));
	R_GetSceneSizeInfo (&scene_size);
	input.resources = ctx ? ctx->resources : NULL;
	input.view_rect.x = r_refdef.vrect.x;
	input.view_rect.y = r_refdef.vrect.y;
	input.view_rect.width = r_refdef.vrect.width;
	input.view_rect.height = r_refdef.vrect.height;
	input.scene_size.width = scene_size.scene_width;
	input.scene_size.height = scene_size.scene_height;
	input.scene_size.scale = scene_size.scene_scale;
	input.scene_size.resolution_ratio = scene_size.resolution_ratio;
	input.has_worldmodel = (cl.worldmodel != NULL);
	if ((r_ref_enable_fog.value == 0.f || r_ref_enable_lighting.value == 0.f)
		&& (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f))
	{
		Con_DPrintf ("ref_gl: render scene flags fog=%d lighting=%d (informational; full scene-pass split pending)\n",
			r_ref_enable_fog.value != 0.f,
			r_ref_enable_lighting.value != 0.f);
	}

	R_RenderScene (&input);
}

static const RenderGraphResourceHandle *GLBackend_GetPassResourcesStrict (const RenderPassContext *ctx, const char *pass_name)
{
	if (ctx && ctx->resources)
		return ctx->resources;
	if (r_framegraph_debug.value > 0.f && s_missing_pass_resource_log_frame != r_framecount)
	{
		Con_DWarning ("FrameGraph contract: '%s' ran without pass resources; skipping pass callback\n",
			pass_name ? pass_name : "<unnamed>");
		s_missing_pass_resource_log_frame = r_framecount;
	}
	return NULL;
}

static void GLBackend_PassWarpResolve (RenderPassContext *ctx)
{
	const RenderGraphResourceHandle *resources;
	qboolean frameplan_needs_postprocess;
	r_warp_resolve_input_t input;
	scene_size_info_t scene_size;

	if (!GLBackend_RequireFramePlan (ctx, "Warp/resolve"))
		return;
	if (!ctx->frame_plan->needs_scene_effects)
	{
		if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
			Con_DPrintf ("ref_gl: skip FG pass Warp/resolve (run_scene_effects=0)\n");
		return;
	}
	frameplan_needs_postprocess = ctx->frame_plan->needs_postprocess;

	resources = GLBackend_GetPassResourcesStrict (ctx, "Warp/resolve");
	if (!resources)
		return;

	memset (&input, 0, sizeof (input));
	R_GetSceneSizeInfo (&scene_size);
	input.resources = resources;
	input.view_rect.x = r_refdef.vrect.x;
	input.view_rect.y = r_refdef.vrect.y;
	input.view_rect.width = r_refdef.vrect.width;
	input.view_rect.height = r_refdef.vrect.height;
	input.scene_size.width = scene_size.scene_width;
	input.scene_size.height = scene_size.scene_height;
	input.scene_size.scale = scene_size.scene_scale;
	input.scene_size.resolution_ratio = scene_size.resolution_ratio;
	input.needs_postprocess = frameplan_needs_postprocess;
	input.dof_enabled = frameplan_needs_postprocess && R_PostFX_DoFEnabledEffective ();
	input.ssao_enabled = frameplan_needs_postprocess && R_SSAO_EnabledEffective ();
	input.godrays_preview = R_PostFX_GodraysPreviewEnabledEffective ();

	R_WarpScaleView (&input);
}

static void GLBackend_PassPostProcess (RenderPassContext *ctx)
{
	static int postprocess_log_count = 0;
	if (!GLBackend_RequireFramePlan (ctx, "Postprocess"))
		return;

	if (!ctx->frame_plan->run_postprocess)
	{
		if (GLBackend_LogOncePerFrame (&s_backend_postfx_skip_log_frame))
		{
			Con_DPrintf ("ref_gl: skipping backend Postprocess callback (run_postprocess=0)\n");
		}
		return;
	}
	if (developer.value != 0.f && postprocess_log_count < 32)
	{
		Con_Printf ("ref_gl pass postprocess: run=1 scenefx=%d composite_written=%d\n",
			ctx->frame_plan->needs_scene_effects ? 1 : 0,
			ctx->composite_written_this_frame ? 1 : 0);
		postprocess_log_count++;
	}

	const RenderGraphResourceHandle *resources = GLBackend_GetPassResourcesStrict (ctx, "Postprocess");
	r_postprocess_input_t input;
	scene_size_info_t scene_size;

	if (!resources)
		return;

	memset (&input, 0, sizeof (input));
	R_GetSceneSizeInfo (&scene_size);
	input.resources = resources;
	input.view_rect.x = r_refdef.vrect.x;
	input.view_rect.y = r_refdef.vrect.y;
	input.view_rect.width = r_refdef.vrect.width;
	input.view_rect.height = r_refdef.vrect.height;
	input.scene_size.width = scene_size.scene_width;
	input.scene_size.height = scene_size.scene_height;
	input.scene_size.scale = scene_size.scene_scale;
	input.scene_size.resolution_ratio = scene_size.resolution_ratio;
	input.composite_written_this_frame = ctx ? ctx->composite_written_this_frame : false;
	if (ctx && ctx->frame_plan && !ctx->frame_plan->needs_scene_effects)
		input.composite_written_this_frame = true;
	if (!input.composite_written_this_frame && GLBackend_ShouldLogPasses ())
		Con_DPrintf ("ref_gl: postprocess running without composite_written flag (fallback path)\n");
	if (r_ref_enable_postfx.value == 0.f && (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f))
		Con_DPrintf ("ref_gl: postprocess disabled, presenting composite via blit fallback\n");

	GL_PostProcess (&input);
}

static void GLBackend_PassOverlayViewmodel (RenderPassContext *ctx)
{
	if (!GLBackend_RequireFramePlan (ctx, "Overlay viewmodel"))
		return;

	if (!ctx->frame_plan->run_viewmodel)
	{
		if (GLBackend_LogOncePerFrame (&s_backend_viewmodel_skip_log_frame))
		{
			Con_DPrintf ("ref_gl: skipping backend Overlay viewmodel callback (run_viewmodel=0)\n");
		}
		return;
	}
	(void)ctx;
	R_DrawViewModel ();
}

static void GLBackend_PassOverlayPolyblend (RenderPassContext *ctx)
{
	if (!GLBackend_RequireFramePlan (ctx, "Overlay polyblend"))
		return;

	if (!ctx->frame_plan->run_polyblend)
	{
		if (GLBackend_LogOncePerFrame (&s_backend_polyblend_skip_log_frame))
		{
			Con_DPrintf ("ref_gl: skipping backend Overlay polyblend callback (run_polyblend=0)\n");
		}
		return;
	}
	(void)ctx;
	V_PolyBlend ();
}

static qboolean GLBackend_HasRequiredPassCallbacks (void)
{
	return (GLBackend_PassSetupView != NULL
		&& GLBackend_PassShadowMaps != NULL
		&& GLBackend_PassRenderScene != NULL
		&& GLBackend_PassWarpResolve != NULL
		&& GLBackend_PassPostProcess != NULL
		&& GLBackend_PassOverlayViewmodel != NULL
		&& GLBackend_PassOverlayPolyblend != NULL);
}

static qboolean GLBackend_Init (void)
{
	memset (&ref_gl_stats, 0, sizeof (ref_gl_stats));
	GL_Backend_ResetResources ();
	GL_Backend_ResetStateCache ();
	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
		Con_Printf ("ref_gl: backend init complete\n");
	return true;
}

static void GLBackend_Shutdown (void)
{
	GL_Backend_ResetResources ();
	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
	{
		Con_Printf ("ref_gl: backend shutdown\n");
		REFGL_StatsLogSummary ();
	}
}

static void GLBackend_OnResize (int width, int height)
{
	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: backend resize %dx%d\n", width, height);
}

static qboolean GLBackend_CanActivate (qboolean runtime_switch)
{
	(void)runtime_switch;
	return true;
}

static void GLBackend_BeginFrame (void)
{
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: backend begin frame\n");
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

	GL_Backend_ResetStateCache ();
	GL_BackendBeginFrame ();
	if ((r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		&& s_refgl_feature_log_frame != r_framecount)
	{
		s_refgl_feature_log_frame = r_framecount;
		Con_DPrintf ("ref_gl flags: postfx=%d shadows=%d fog=%d lighting=%d\n",
			r_ref_enable_postfx.value != 0.f,
			r_ref_enable_shadows.value != 0.f,
			r_ref_enable_fog.value != 0.f,
			r_ref_enable_lighting.value != 0.f);
	}
	ref_gl_stats.frames_rendered++;
	REFGL_StatsPeriodicLog ();
}

static void GLBackend_EndFrame (void)
{
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: backend end frame\n");
	GL_BackendEndFrame ();
}

static void GLBackend_Present (void)
{
	if (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		Con_DPrintf ("ref_gl: backend present\n");
	GL_BackendPresent ();
}

static SDL_Window *s_backend_window = NULL;

static qboolean GLBackend_ContextInit (void *window_handle)
{
	SDL_Window *window = (SDL_Window *)window_handle;
	if (!window)
		return false;

	s_backend_window = window;

	if (r_refgl_log_init.value != 0.f || r_refgl_debug.value != 0.f)
		Con_Printf ("ref_gl: context init window=%p\n", (void *)window);

	return true;
}

static void GLBackend_ContextShutdown (void)
{
	s_backend_window = NULL;
}

static void GLBackend_SwapBuffers (void)
{
	if (s_backend_window)
		SDL_GL_SwapWindow (s_backend_window);
}

static unsigned GLBackend_GetActiveShaderId (void)
{
	return (unsigned)GL_GetCurrentProgram ();
}

static qboolean GLBackend_QueryShaderMetadata (unsigned shader_id, const char **out_debug_name, const char **out_entry_point, const char **out_stage, unsigned *out_permutation_key)
{
	return GL_QueryProgramMetadata ((GLuint)shader_id, out_debug_name, out_entry_point, out_stage, out_permutation_key);
}

static void GLBackend_ApplyFrameGraphBaseline (unsigned baseline_bits)
{
	unsigned state_bits = glstate;
	qboolean apply_pipeline_state = false;
	/* REF_GL_PASS_EXECUTION:
	 * Backend owns concrete state baselines for declarative framegraph passes. */

	if ((baseline_bits & FG_PASS_BASELINE_RESET_BLEND) != 0u)
	{
		state_bits = (state_bits & ~GLS_MASK_BLEND) | GLS_BLEND_OPAQUE;
		apply_pipeline_state = true;
	}

	if ((baseline_bits & FG_PASS_BASELINE_RESET_DEPTH) != 0u)
	{
		state_bits &= ~(GLS_NO_ZTEST | GLS_NO_ZWRITE);
		apply_pipeline_state = true;
	}

	if ((baseline_bits & FG_PASS_BASELINE_RESET_CULL) != 0u)
	{
		state_bits = (state_bits & ~GLS_MASK_CULL) | GLS_CULL_BACK;
		apply_pipeline_state = true;
	}

	if ((baseline_bits & FG_PASS_BASELINE_RESET_PROGRAM_BINDINGS) != 0u)
	{
		state_bits &= ~(GLS_MASK_ATTRIBS | GLS_MASK_INSTANCED_ATTRIBS);
		apply_pipeline_state = true;
	}

	if (apply_pipeline_state)
		GLBackend_SetPipelineState (state_bits);

	if ((baseline_bits & FG_PASS_BASELINE_RESET_SCISSOR) != 0u)
		GLBackend_SetScissor (false, 0, 0, 0, 0);
}

static const IRenderBackend s_gl_backend = {
	"OpenGL",
	GLBackend_ContextInit,
	GLBackend_ContextShutdown,
	GLBackend_SwapBuffers,
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
	GLBackend_GetSceneSampleCount,
	GLBackend_GetActiveShaderId,
	GLBackend_QueryShaderMetadata,
	GLBackend_ApplyFrameGraphBaseline
};

const IRenderBackend *GL_Backend_GetInterface (void)
{
	GLBackend_DetectCaps ();
	return &s_gl_backend;
}
