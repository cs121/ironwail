#include "quakedef.h"

#include "r_framegraph.h"

#include "r_fogvol.h"

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

void R_SetupView (void);
void R_RenderShadowMaps (void);
void R_RenderScene (const RenderGraphResourceHandle *resources);
void R_WarpScaleView (const RenderGraphResourceHandle *resources);
void GL_PostProcess (const RenderGraphResourceHandle *resources);
void R_DrawViewModel (void);
void R_StorePrevFrameState (void);
void V_PolyBlend (void);

typedef enum framegraph_pass_id_e
{
	FG_PASS_SETUP_VIEW = 0,
	FG_PASS_SHADOW_MAPS,
	FG_PASS_DECALS,
	FG_PASS_SCENE,
	FG_PASS_WARP_RESOLVE,
	FG_PASS_FOGVOL_PREPARE,
	FG_PASS_FOGVOL,
	FG_PASS_SSAO_HANDOFF,
	FG_PASS_POSTPROCESS,
	FG_PASS_VIEWMODEL,
	FG_PASS_POLYBLEND,
	FG_PASS_STORE_PREV,
	FG_PASS_COUNT
} framegraph_pass_id_t;

enum
{
	FG_TIMER_QUERY_RING = 3
};

typedef struct framegraph_timer_slot_s
{
	GLuint start_query;
	GLuint end_query;
	int frame_index;
	qboolean issued;
} framegraph_timer_slot_t;

typedef struct framegraph_pass_stats_s
{
	double cpu_ms;
	double cpu_avg_ms;
	double gpu_ms;
	double gpu_avg_ms;
	unsigned cpu_samples;
	unsigned gpu_samples;
	framegraph_timer_slot_t timer_slots[FG_TIMER_QUERY_RING];
	int timer_write_index;
	int timer_active_slot_plus_one;
} framegraph_pass_stats_t;

static framegraph_pass_stats_t s_pass_stats[FG_PASS_COUNT];
static int s_last_stats_print = -120;
static RenderFramePlan s_cached_plan;
static int s_cached_plan_frame = -1;

static void FG_Backend_BeginPass (const char *name);
static void FG_Backend_EndPass (void);
static void FG_Backend_ValidatePassState (const char *pass_name, qboolean before_pass);
static void FG_Backend_BeginTimer (int pass_id);
static void FG_Backend_EndTimer (int pass_id);
static void FG_Backend_ResolveTimers (void);

static const IRenderBackend s_gl_backend = {
	"OpenGL",
	FG_Backend_BeginPass,
	FG_Backend_EndPass,
	FG_Backend_ValidatePassState,
	FG_Backend_BeginTimer,
	FG_Backend_EndTimer,
	FG_Backend_ResolveTimers
};

static qboolean FG_PassAlways (const RenderPassContext *ctx)
{
	(void)ctx;
	return true;
}

static qboolean FG_HasResources (const RenderPassContext *ctx, unsigned required)
{
	unsigned available = RENDER_RES_NONE;

	if (!ctx || !ctx->resources)
		return false;

	if (ctx->resources->scene_fbo)
		available |= RENDER_RES_SCENE_COLOR;
	if (ctx->resources->scene_depth_tex)
		available |= RENDER_RES_SCENE_DEPTH;
	if (ctx->resources->composite_fbo)
		available |= RENDER_RES_COMPOSITE_COLOR;
	if (ctx->resources->shadow_sun_depth_tex)
		available |= RENDER_RES_SHADOW_SUN_DEPTH;
	if (ctx->resources->fogvol_history_tex)
		available |= RENDER_RES_FOGVOL_HISTORY;
	if (ctx->resources->velocity_tex)
		available |= RENDER_RES_VELOCITY;

	return (available & required) == required;
}

static qboolean FG_PassWhenShadowEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_shadowmaps
		&& FG_HasResources (ctx, RENDER_RES_SHADOW_SUN_DEPTH);
}

static qboolean FG_PassWhenFogVolEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_fogvol
		&& FG_HasResources (ctx, RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_FOGVOL_HISTORY);
}

static qboolean FG_PassWhenPostprocessEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_postprocess
		&& FG_HasResources (ctx, RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH);
}

static qboolean FG_PassWhenViewmodelEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_viewmodel;
}

static qboolean FG_PassWhenPolyBlendEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_polyblend;
}

static qboolean FG_PassWhenStorePrevEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_store_prev;
}

static void FG_ExecSetupView (RenderPassContext *ctx)
{
	(void)ctx;
	R_SetupView ();
}

static void FG_ExecShadowMaps (RenderPassContext *ctx)
{
	(void)ctx;
	R_RenderShadowMaps ();
}

static void FG_ExecDecals (RenderPassContext *ctx)
{
	(void)ctx;
	R_UpdateDecals ();
}

static void FG_ExecScene (RenderPassContext *ctx)
{
	R_RenderScene (ctx ? ctx->resources : NULL);
}

static void FG_ExecWarpResolve (RenderPassContext *ctx)
{
	R_WarpScaleView (ctx ? ctx->resources : NULL);
}

static void FG_ExecFogVolPrepare (RenderPassContext *ctx)
{
	if (!ctx || !ctx->legacy_state || !ctx->legacy_state->prepare_fogvol_inputs)
		return;
	ctx->legacy_state->prepare_fogvol_inputs ();
}

static void FG_ExecFogVol (RenderPassContext *ctx)
{
	if (ctx && ctx->legacy_state && ctx->legacy_state->fogvol_update_called)
		(*ctx->legacy_state->fogvol_update_called)++;
	R_FogVol_BuildList ();
	if (ctx && ctx->legacy_state && ctx->legacy_state->fogvol_draw_called)
		(*ctx->legacy_state->fogvol_draw_called)++;
	R_FogVol_Render ();
}

static void FG_ExecSSAOHandoff (RenderPassContext *ctx)
{
	if (!ctx || !ctx->legacy_state || !ctx->legacy_state->ssao_fog_state)
		return;
	R_SSAO_CaptureFogState (&r_framedata, ctx->legacy_state->ssao_fog_state);
}

static void FG_ExecPostprocess (RenderPassContext *ctx)
{
	GL_PostProcess (ctx ? ctx->resources : NULL);
}

static void FG_ExecViewmodel (RenderPassContext *ctx)
{
	(void)ctx;
	R_DrawViewModel ();
}

static void FG_ExecPolyBlend (RenderPassContext *ctx)
{
	(void)ctx;
	V_PolyBlend ();
}

static void FG_ExecStorePrev (RenderPassContext *ctx)
{
	if (ctx && ctx->legacy_state && ctx->legacy_state->frame_rendered_this_update)
		*ctx->legacy_state->frame_rendered_this_update = true;
	R_StorePrevFrameState ();
}

static const RenderPassDesc s_render_passes[FG_PASS_COUNT] = {
	{
		"Setup view",
		RENDER_RES_NONE,
		RENDER_RES_NONE,
		1u << 0,
		FG_PassAlways,
		FG_ExecSetupView
	},
	{
		"Shadow maps",
		RENDER_RES_NONE,
		RENDER_RES_SHADOW_SUN_DEPTH,
		0,
		FG_PassWhenShadowEnabled,
		FG_ExecShadowMaps
	},
	{
		"Update decals",
		RENDER_RES_NONE,
		RENDER_RES_DECALS,
		0,
		FG_PassAlways,
		FG_ExecDecals
	},
	{
		"Render scene",
		RENDER_RES_DECALS,
		RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_VELOCITY,
		0,
		FG_PassAlways,
		FG_ExecScene
	},
	{
		"Warp/resolve",
		RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH,
		RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
		1u << 0,
		FG_PassAlways,
		FG_ExecWarpResolve
	},
	{
		"Prepare fogvol inputs",
		RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
		RENDER_RES_FOGVOL_INPUTS,
		0,
		FG_PassAlways,
		FG_ExecFogVolPrepare
	},
	{
		"Render fog volumes",
		RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_SHADOW_SUN_DEPTH | RENDER_RES_FOGVOL_HISTORY | RENDER_RES_VELOCITY | RENDER_RES_FOGVOL_INPUTS,
		RENDER_RES_COMPOSITE_COLOR | RENDER_RES_FOGVOL_HISTORY,
		1u << 0,
		FG_PassWhenFogVolEnabled,
		FG_ExecFogVol
	},
	{
		"Capture fog handoff",
		RENDER_RES_COMPOSITE_COLOR,
		RENDER_RES_SSAO_FOG_STATE,
		0,
		FG_PassAlways,
		FG_ExecSSAOHandoff
	},
	{
		"Postprocess",
		RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_SSAO_FOG_STATE,
		RENDER_RES_COMPOSITE_COLOR,
		1u << 0,
		FG_PassWhenPostprocessEnabled,
		FG_ExecPostprocess
	},
	{
		"Draw viewmodel",
		RENDER_RES_COMPOSITE_COLOR,
		RENDER_RES_COMPOSITE_COLOR,
		1u << 0,
		FG_PassWhenViewmodelEnabled,
		FG_ExecViewmodel
	},
	{
		"Polyblend",
		RENDER_RES_COMPOSITE_COLOR,
		RENDER_RES_COMPOSITE_COLOR,
		1u << 0,
		FG_PassWhenPolyBlendEnabled,
		FG_ExecPolyBlend
	},
	{
		"Store previous frame",
		RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
		RENDER_RES_NONE,
		1u << 0,
		FG_PassWhenStorePrevEnabled,
		FG_ExecStorePrev
	}
};

static qboolean FG_Backend_HasTimestampQueries (void)
{
	return (GL_GenQueriesFunc && GL_DeleteQueriesFunc
		&& GL_QueryCounterFunc
		&& GL_GetQueryObjectuivFunc
		&& GL_GetQueryObjectui64vFunc);
}

static void FG_Backend_BeginPass (const char *name)
{
	GL_BeginGroup (name);
}

static void FG_Backend_EndPass (void)
{
	GL_EndGroup ();
}

static void FG_Backend_ValidatePassState (const char *pass_name, qboolean before_pass)
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

static void FG_Backend_BeginTimer (int pass_id)
{
	framegraph_timer_slot_t *slot;
	framegraph_pass_stats_t *stats;
	int attempts;
	int slot_index;

	if (!FG_Backend_HasTimestampQueries ())
		return;
	if (pass_id < 0 || pass_id >= FG_PASS_COUNT)
		return;

	stats = &s_pass_stats[pass_id];
	if (stats->timer_active_slot_plus_one != 0)
		return;

	for (attempts = 0; attempts < FG_TIMER_QUERY_RING; ++attempts)
	{
		slot_index = (stats->timer_write_index + attempts) % FG_TIMER_QUERY_RING;
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

static void FG_Backend_EndTimer (int pass_id)
{
	framegraph_timer_slot_t *slot;
	framegraph_pass_stats_t *stats;
	int slot_index;

	if (!FG_Backend_HasTimestampQueries ())
		return;
	if (pass_id < 0 || pass_id >= FG_PASS_COUNT)
		return;

	stats = &s_pass_stats[pass_id];
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
	stats->timer_write_index = (slot_index + 1) % FG_TIMER_QUERY_RING;
}

static void FG_Backend_ResolveTimers (void)
{
	int i, slot_index;

	if (!FG_Backend_HasTimestampQueries ())
		return;

	for (i = 0; i < FG_PASS_COUNT; ++i)
	{
		framegraph_pass_stats_t *stats = &s_pass_stats[i];

		for (slot_index = 0; slot_index < FG_TIMER_QUERY_RING; ++slot_index)
		{
			framegraph_timer_slot_t *slot = &stats->timer_slots[slot_index];
			GLuint available = 0;
			GLuint64 start_ns = 0;
			GLuint64 end_ns = 0;
			double gpu_ms;

			if (!slot->issued || !slot->start_query || !slot->end_query)
				continue;
			if (slot->frame_index > r_framecount - FG_TIMER_QUERY_RING)
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

			stats->gpu_ms = gpu_ms;
			stats->gpu_avg_ms = (stats->gpu_samples == 0) ? gpu_ms : (stats->gpu_avg_ms * 0.8 + gpu_ms * 0.2);
			stats->gpu_samples++;
			slot->issued = false;
		}
	}
}

static void FG_BuildResourceHandles (RenderGraphResourceHandle *out_handles)
{
	if (!out_handles)
		return;

	memset (out_handles, 0, sizeof (*out_handles));
	out_handles->scene_fbo = framebufs.scene.fbo;
	out_handles->scene_color_tex = framebufs.scene.color_tex;
	out_handles->scene_velocity_tex = framebufs.scene.velocity_tex;
	out_handles->composite_fbo = framebufs.composite.fbo;
	out_handles->composite_color_tex = framebufs.composite.color_tex;
	out_handles->composite_depth_tex = framebufs.composite.depth_stencil_tex;
	out_handles->scene_depth_tex = framebufs.scene.depth_stencil_tex;
	out_handles->scene_samples = framebufs.scene.samples;
	out_handles->resolved_scene_fbo = framebufs.resolved_scene.fbo;
	out_handles->resolved_scene_color_tex = framebufs.resolved_scene.color_tex;
	out_handles->resolved_scene_velocity_tex = framebufs.resolved_scene.velocity_tex;
	out_handles->shadow_sun_depth_tex = framebufs.shadow.sun_depth_tex;
	out_handles->fogvol_history_tex = framebufs.fogvol.history_tex[0];
	out_handles->velocity_tex = (framebufs.scene.samples > 1) ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
}

static unsigned FG_BuildActivePassMask (const RenderPassContext *ctx, int first_pass)
{
	unsigned active_mask = 0;
	unsigned needed_resources = RENDER_RES_NONE;
	int i;

	if (first_pass < 0)
		first_pass = 0;
	if (first_pass >= FG_PASS_COUNT)
		return 0;

	for (i = FG_PASS_COUNT - 1; i >= first_pass; --i)
	{
		const RenderPassDesc *pass = &s_render_passes[i];
		unsigned pass_bit = 1u << i;
		qboolean enabled = (!pass->enabled || pass->enabled (ctx));

		if (!enabled)
			continue;

		if (pass->side_effects)
		{
			active_mask |= pass_bit;
			needed_resources = (needed_resources & ~pass->writes) | pass->reads;
			continue;
		}

		if (pass->writes & needed_resources)
		{
			active_mask |= pass_bit;
			needed_resources = (needed_resources & ~pass->writes) | pass->reads;
		}
	}

	return active_mask;
}

static void FG_DebugPrintPrunedPasses (unsigned active_mask, int first_pass)
{
	int i;

	if (r_gl_state_validate.value < 2.f)
		return;

	for (i = first_pass; i < FG_PASS_COUNT; ++i)
	{
		unsigned pass_bit = 1u << i;
		if ((active_mask & pass_bit) == 0)
			Con_DPrintf ("FrameGraph prune: skipped '%s'\n", s_render_passes[i].name);
	}
}

static void FG_AccumulateCPUStats (int pass_id, double cpu_ms)
{
	framegraph_pass_stats_t *stats;

	if (pass_id < 0 || pass_id >= FG_PASS_COUNT)
		return;

	stats = &s_pass_stats[pass_id];
	stats->cpu_ms = cpu_ms;
	stats->cpu_avg_ms = (stats->cpu_samples == 0) ? cpu_ms : (stats->cpu_avg_ms * 0.8 + cpu_ms * 0.2);
	stats->cpu_samples++;
}

static void FG_MaybePrintStats (void)
{
	if (r_speeds.value < 3.f)
		return;
	if (r_framecount < s_last_stats_print + 60)
		return;

	Con_Printf ("FrameGraph CPUms setup=%.2f shadow=%.2f scene=%.2f warp=%.2f fog=%.2f post=%.2f overlay=%.2f | GPUms shadow=%.2f scene=%.2f fog=%.2f post=%.2f\n",
		s_pass_stats[FG_PASS_SETUP_VIEW].cpu_avg_ms,
		s_pass_stats[FG_PASS_SHADOW_MAPS].cpu_avg_ms,
		s_pass_stats[FG_PASS_SCENE].cpu_avg_ms,
		s_pass_stats[FG_PASS_WARP_RESOLVE].cpu_avg_ms,
		s_pass_stats[FG_PASS_FOGVOL].cpu_avg_ms,
		s_pass_stats[FG_PASS_POSTPROCESS].cpu_avg_ms,
		s_pass_stats[FG_PASS_VIEWMODEL].cpu_avg_ms + s_pass_stats[FG_PASS_POLYBLEND].cpu_avg_ms + s_pass_stats[FG_PASS_STORE_PREV].cpu_avg_ms,
		s_pass_stats[FG_PASS_SHADOW_MAPS].gpu_avg_ms,
		s_pass_stats[FG_PASS_SCENE].gpu_avg_ms,
		s_pass_stats[FG_PASS_FOGVOL].gpu_avg_ms,
		s_pass_stats[FG_PASS_POSTPROCESS].gpu_avg_ms);

	s_last_stats_print = r_framecount;
}

static void FG_RunPass (int pass_id, const RenderPassDesc *pass, RenderPassContext *ctx)
{
	double cpu_start;

	if (!pass || !ctx)
		return;
	if (pass->enabled && !pass->enabled (ctx))
		return;

	if (ctx->backend && ctx->backend->validate_pass_state)
		ctx->backend->validate_pass_state (pass->name, true);
	if (ctx->backend && ctx->backend->begin_pass)
		ctx->backend->begin_pass (pass->name);
	if (ctx->backend && ctx->backend->begin_timer)
		ctx->backend->begin_timer (pass_id);

	cpu_start = Sys_DoubleTime ();
	pass->execute (ctx);
	FG_AccumulateCPUStats (pass_id, (Sys_DoubleTime () - cpu_start) * 1000.0);

	if (ctx->backend && ctx->backend->end_timer)
		ctx->backend->end_timer (pass_id);
	if (ctx->backend && ctx->backend->end_pass)
		ctx->backend->end_pass ();
	if (ctx->backend && ctx->backend->validate_pass_state)
		ctx->backend->validate_pass_state (pass->name, false);
}

void R_FrameGraph_BuildRenderFramePlan (RenderFramePlan *out_plan)
{
	if (!out_plan)
		return;

	memset (out_plan, 0, sizeof (*out_plan));
	out_plan->needs_scene_effects = GL_NeedsSceneEffects ();
	out_plan->needs_postprocess = GL_NeedsPostprocess ();
	out_plan->run_shadowmaps = true;
	out_plan->run_fogvol = R_FogVol_IsEnabledForFrame () && R_FogVol_HasRenderableContent ();
	out_plan->run_postprocess = out_plan->needs_postprocess;
	out_plan->run_viewmodel = true;
	out_plan->run_polyblend = true;
	out_plan->run_store_prev = true;
}

void R_FrameGraph_SetRenderFramePlan (const RenderFramePlan *plan)
{
	if (!plan)
	{
		s_cached_plan_frame = -1;
		memset (&s_cached_plan, 0, sizeof (s_cached_plan));
		return;
	}

	s_cached_plan = *plan;
	s_cached_plan_frame = r_framecount;
}

qboolean R_FrameGraph_GetRenderFramePlan (RenderFramePlan *out_plan)
{
	if (s_cached_plan_frame != r_framecount)
		return false;
	if (out_plan)
		*out_plan = s_cached_plan;
	return true;
}

/*
 * Framegraph pass order and data dependencies
 * ------------------------------------------
 * 1) Build a deterministic per-frame plan once and cache it.
 * 2) Execute explicit passes with declared contracts and timing instrumentation.
 * 3) Keep postprocess + overlays in the same scheduler so ordering remains stable.
 */
void R_FrameGraph_RenderView (const r_framegraph_state_t *state)
{
	RenderFramePlan frame_plan;
	RenderGraphResourceHandle resources;
	RenderPassContext pass_ctx;
	unsigned active_pass_mask;
	int i = 0;

	memset (&frame_plan, 0, sizeof (frame_plan));
	FG_BuildResourceHandles (&resources);

	pass_ctx.legacy_state = state;
	pass_ctx.frame_plan = &frame_plan;
	pass_ctx.resources = &resources;
	pass_ctx.backend = &s_gl_backend;

	if (pass_ctx.backend && pass_ctx.backend->resolve_timers)
		pass_ctx.backend->resolve_timers ();

	/* Setup first so frame counters/state are current before building the plan cache. */
	FG_RunPass (FG_PASS_SETUP_VIEW, &s_render_passes[FG_PASS_SETUP_VIEW], &pass_ctx);
	i = FG_PASS_SETUP_VIEW + 1;

	R_FrameGraph_BuildRenderFramePlan (&frame_plan);
	R_FrameGraph_SetRenderFramePlan (&frame_plan);
	active_pass_mask = FG_BuildActivePassMask (&pass_ctx, i);
	FG_DebugPrintPrunedPasses (active_pass_mask, i);

	for (; i < FG_PASS_COUNT; ++i)
	{
		unsigned pass_bit = 1u << i;
		if ((active_pass_mask & pass_bit) == 0)
			continue;
		FG_RunPass (i, &s_render_passes[i], &pass_ctx);
	}

	FG_MaybePrintStats ();
}
