#include "quakedef.h"

#include "r_framegraph.h"

extern int r_framecount;
extern cvar_t r_refgl_log_passes;
extern cvar_t r_refgl_debug;

static int s_shadow_skip_log_frame = -1;
static int s_postfx_skip_log_frame = -1;
static int s_viewmodel_skip_log_frame = -1;
static int s_polyblend_skip_log_frame = -1;
static int s_storeprev_skip_log_frame = -1;

#define FG_PASS_BASELINE_DETERMINISTIC_STATE ( \
	FG_PASS_BASELINE_RESET_SCISSOR | \
	FG_PASS_BASELINE_RESET_BLEND | \
	FG_PASS_BASELINE_RESET_DEPTH | \
	FG_PASS_BASELINE_RESET_CULL | \
	FG_PASS_BASELINE_RESET_PROGRAM_BINDINGS)
/* TODO_PASS_BOUNDARY:
 * r_passes declares WHAT each pass needs and produces.
 * Pass execution details (GL state/FBOs/bindings/clears) stay backend-owned. */

static qboolean R_FG_PassWhenShadowEnabled (const RenderPassContext *ctx)
{
	const render_backend_resource_ref_t *shadow_depth;
	const qboolean log_skips = (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f);

	if (!ctx || !ctx->frame_plan || !ctx->frame_plan->run_shadowmaps)
	{
		if (log_skips && s_shadow_skip_log_frame != r_framecount)
		{
			Con_DPrintf ("ref_gl: skip FG pass Shadow maps (run_shadowmaps=%d)\n",
				(ctx && ctx->frame_plan && ctx->frame_plan->run_shadowmaps) ? 1 : 0);
			s_shadow_skip_log_frame = r_framecount;
		}
		return false;
	}

	shadow_depth = R_FrameGraph_GetResourceRef (ctx->resources, R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH);
	if (!shadow_depth)
	{
		if (log_skips && s_shadow_skip_log_frame != r_framecount)
		{
			Con_DPrintf ("ref_gl: skip FG pass Shadow maps (missing shadow depth resource ref)\n");
			s_shadow_skip_log_frame = r_framecount;
		}
		return false;
	}

	return !ctx->backend || !ctx->backend->is_resource_valid
		|| ctx->backend->is_resource_valid (ctx->resources, shadow_depth);
}

static qboolean R_FG_PassWhenPostprocessEnabled (const RenderPassContext *ctx)
{
	const qboolean enabled = ctx && ctx->frame_plan
		&& ctx->frame_plan->run_postprocess;

	if (!enabled && (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		&& s_postfx_skip_log_frame != r_framecount)
	{
		Con_DPrintf ("ref_gl: skip FG pass Postprocess (run_postprocess=0)\n");
		s_postfx_skip_log_frame = r_framecount;
	}

	return enabled;
}

static qboolean R_FG_PassWhenViewmodelEnabled (const RenderPassContext *ctx)
{
	const qboolean enabled = ctx && ctx->frame_plan && ctx->frame_plan->run_viewmodel;

	if (!enabled && (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		&& s_viewmodel_skip_log_frame != r_framecount)
	{
		Con_DPrintf ("ref_gl: skip FG pass Overlay viewmodel (run_viewmodel=0)\n");
		s_viewmodel_skip_log_frame = r_framecount;
	}

	return enabled;
}

static qboolean R_FG_PassWhenPolyblendEnabled (const RenderPassContext *ctx)
{
	const qboolean enabled = ctx && ctx->frame_plan && ctx->frame_plan->run_polyblend;

	if (!enabled && (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		&& s_polyblend_skip_log_frame != r_framecount)
	{
		Con_DPrintf ("ref_gl: skip FG pass Overlay polyblend (run_polyblend=0)\n");
		s_polyblend_skip_log_frame = r_framecount;
	}

	return enabled;
}

static qboolean R_FG_PassWhenStorePrevEnabled (const RenderPassContext *ctx)
{
	const qboolean enabled = ctx && ctx->frame_plan && ctx->frame_plan->run_store_prev;

	if (!enabled && (r_refgl_log_passes.value != 0.f || r_refgl_debug.value != 0.f)
		&& s_storeprev_skip_log_frame != r_framecount)
	{
		Con_DPrintf ("ref_gl: skip FG pass Store previous state (run_store_prev=0)\n");
		s_storeprev_skip_log_frame = r_framecount;
	}

	return enabled;
}

static void R_Pass_RunBackendCallback (RenderPassContext *ctx, const char *pass_name, void (*callback)(RenderPassContext *))
{
	if (!ctx || !ctx->backend)
		Sys_Error ("R_Pass_RunBackendCallback: missing render backend context for pass '%s'.\n",
			pass_name ? pass_name : "<unnamed>");

	if (!ctx->backend->has_required_pass_callbacks
		|| !ctx->backend->has_required_pass_callbacks ())
	{
		Sys_Error ("R_Pass_RunBackendCallback: backend '%s' did not advertise required pass callbacks for '%s'.\n",
			ctx->backend->name ? ctx->backend->name : "<unnamed>",
			pass_name ? pass_name : "<unnamed>");
	}

	if (!callback)
	{
		Sys_Error ("R_Pass_RunBackendCallback: backend '%s' is missing required callback for pass '%s'.\n",
			ctx->backend->name ? ctx->backend->name : "<unnamed>",
			pass_name ? pass_name : "<unnamed>");
	}

	callback (ctx);
}

void R_Pass_SetupView (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Setup view",
		ctx && ctx->backend ? ctx->backend->pass_setup_view : NULL);
}

void R_Pass_RenderShadowMaps (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Shadow maps",
		ctx && ctx->backend ? ctx->backend->pass_shadowmaps : NULL);
}

void R_Pass_RenderScene (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Render scene",
		ctx && ctx->backend ? ctx->backend->pass_render_scene : NULL);
}

void R_Pass_WarpResolve (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Warp/resolve",
		ctx && ctx->backend ? ctx->backend->pass_warp_resolve : NULL);
}

void R_Pass_PostProcess (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Postprocess",
		ctx && ctx->backend ? ctx->backend->pass_postprocess : NULL);
}

void R_Pass_DrawViewmodel (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Overlay viewmodel",
		ctx && ctx->backend ? ctx->backend->pass_overlay_viewmodel : NULL);
}

void R_Pass_DrawPolyblend (RenderPassContext *ctx)
{
	R_Pass_RunBackendCallback (ctx, "Overlay polyblend",
		ctx && ctx->backend ? ctx->backend->pass_overlay_polyblend : NULL);
}

static void R_Pass_CaptureSSAOFogHandoff (RenderPassContext *ctx)
{
	(void)ctx;
	/* Capture global fog parameters for deterministic postprocess SSAO suppression. */
	R_CaptureSSAOFogHandoffState ();
}

static void R_Pass_StorePrevFrame (RenderPassContext *ctx)
{
	(void)ctx;
	R_MarkFrameRenderedThisUpdate ();
	R_StorePrevFrameState ();
}

static const FGPassAttachmentConfig s_scene_color_attachments[] = {
	{ RENDER_RES_SCENE_COLOR, R_BACKEND_LOAD_OP_CLEAR, R_BACKEND_STORE_OP_STORE },
	{ RENDER_RES_VELOCITY, R_BACKEND_LOAD_OP_CLEAR, R_BACKEND_STORE_OP_STORE }
};

static const FGPassAttachmentConfig s_scene_depth_attachment = {
	RENDER_RES_SCENE_DEPTH,
	R_BACKEND_LOAD_OP_CLEAR,
	R_BACKEND_STORE_OP_STORE
};

static const FGPassAttachmentConfig s_shadow_depth_attachment = {
	RENDER_RES_SHADOW_SUN_DEPTH,
	R_BACKEND_LOAD_OP_CLEAR,
	R_BACKEND_STORE_OP_STORE
};

static const FGPassAttachmentConfig s_warp_color_attachments[] = {
	{ RENDER_RES_COMPOSITE_COLOR, R_BACKEND_LOAD_OP_CLEAR, R_BACKEND_STORE_OP_STORE }
};

static const FGPassAttachmentConfig s_warp_depth_attachment = {
	RENDER_RES_COMPOSITE_DEPTH,
	R_BACKEND_LOAD_OP_CLEAR,
	R_BACKEND_STORE_OP_STORE
};

static const FGPassAttachmentConfig s_postprocess_color_attachments[] = {
	{ RENDER_RES_COMPOSITE_COLOR, R_BACKEND_LOAD_OP_LOAD, R_BACKEND_STORE_OP_STORE }
};

static const RenderPassDesc s_setup_view_framegraph_pass = {
	.name = "Setup view",
	.reads = RENDER_RES_NONE,
	.writes = RENDER_RES_NONE,
	.side_effects = FG_SIDEFX_GLOBAL_STATE,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE,
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = NULL,
	.execute = R_Pass_SetupView,
	.stage = FG_PASS_STAGE_SETUP,
	.stats_channel = FG_PASS_STATS_SETUP
};

static const RenderPassDesc s_shadowmaps_framegraph_pass = {
	.name = "Shadow maps",
	.reads = RENDER_RES_NONE,
	.writes = RENDER_RES_SHADOW_SUN_DEPTH,
	.side_effects = 0,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE,
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = R_FG_PassWhenShadowEnabled,
	.execute = R_Pass_RenderShadowMaps,
	.depth_attachment = &s_shadow_depth_attachment,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_SHADOW
};

static const RenderPassDesc s_scene_framegraph_pass = {
	.name = "Render scene",
	/* Shadow depth is sampled opportunistically by runtime/backend shadow code;
	 * keep scene pass framegraph reads strict to always-bound resources only. */
	/* LEGACY_IMPLICIT_STATE:
	 * Transitional render units (r_world/r_alias/r_part) still rely on backend-
	 * established GL baselines and global renderer state. */
	.reads = RENDER_RES_DECALS,
	.writes = RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_VELOCITY,
	.side_effects = 0,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE | FG_PASS_BASELINE_REQUIRE_AUTOBIND,
	.output_target = FG_PASS_OUTPUT_AUTO_SCENE,
	.viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT_SCALED,
	.enabled = NULL,
	.execute = R_Pass_RenderScene,
	.color_attachments = s_scene_color_attachments,
	.num_color_attachments = 2,
	.depth_attachment = &s_scene_depth_attachment,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_SCENE
};

static const RenderPassDesc s_warp_resolve_framegraph_pass = {
	.name = "Warp/resolve",
	.reads = RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH,
	.writes = RENDER_RES_COMPOSITE_COLOR | RENDER_RES_COMPOSITE_DEPTH,
	.side_effects = FG_SIDEFX_GLOBAL_STATE,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE | FG_PASS_BASELINE_REQUIRE_AUTOBIND,
	.output_target = FG_PASS_OUTPUT_AUTO_WARP,
	.viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT,
	.enabled = NULL,
	/* REF_GL_PASS_EXECUTION: resolve/blit path implemented in backend callback. */
	.execute = R_Pass_WarpResolve,
	.color_attachments = s_warp_color_attachments,
	.num_color_attachments = 1,
	.depth_attachment = &s_warp_depth_attachment,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_WARP
};

static const RenderPassDesc s_ssao_fog_handoff_framegraph_pass = {
	.name = "Capture fog handoff",
	.reads = RENDER_RES_NONE,
	.writes = RENDER_RES_SSAO_FOG_STATE,
	.side_effects = 0,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE,
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = NULL,
	.execute = R_Pass_CaptureSSAOFogHandoff
};

static const RenderPassDesc s_postprocess_framegraph_pass = {
	.name = "Postprocess",
	.reads = RENDER_RES_COMPOSITE_COLOR | RENDER_RES_COMPOSITE_DEPTH | RENDER_RES_SSAO_FOG_STATE,
	.writes = RENDER_RES_COMPOSITE_COLOR,
	.side_effects = FG_SIDEFX_GLOBAL_STATE,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE | FG_PASS_BASELINE_REQUIRE_AUTOBIND,
	.output_target = FG_PASS_OUTPUT_BACKBUFFER,
	.viewport_mode = FG_PASS_VIEWPORT_FULL_WINDOW,
	.enabled = R_FG_PassWhenPostprocessEnabled,
	/* TODO_STATE_BASELINE: postprocess depends on deterministic baseline + output binding. */
	.execute = R_Pass_PostProcess,
	.color_attachments = s_postprocess_color_attachments,
	.num_color_attachments = 1,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_POST
};

static const RenderPassDesc s_viewmodel_framegraph_pass = {
	.name = "Draw viewmodel",
	.reads = RENDER_RES_COMPOSITE_COLOR,
	.writes = RENDER_RES_NONE,
	.side_effects = FG_SIDEFX_GLOBAL_STATE,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE | FG_PASS_BASELINE_REQUIRE_AUTOBIND,
	.output_target = FG_PASS_OUTPUT_BACKBUFFER,
	.viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT,
	.enabled = R_FG_PassWhenViewmodelEnabled,
	.execute = R_Pass_DrawViewmodel,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_OVERLAY
};

static const RenderPassDesc s_polyblend_framegraph_pass = {
	.name = "Polyblend",
	.reads = RENDER_RES_COMPOSITE_COLOR,
	.writes = RENDER_RES_NONE,
	.side_effects = FG_SIDEFX_GLOBAL_STATE,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE | FG_PASS_BASELINE_REQUIRE_AUTOBIND,
	.output_target = FG_PASS_OUTPUT_BACKBUFFER,
	.viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT,
	.enabled = R_FG_PassWhenPolyblendEnabled,
	.execute = R_Pass_DrawPolyblend,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_OVERLAY
};

static const RenderPassDesc s_storeprev_framegraph_pass = {
	.name = "Store previous frame",
	.reads = RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
	.writes = RENDER_RES_NONE,
	.side_effects = FG_SIDEFX_TEMPORAL_HISTORY | FG_SIDEFX_CPU_SIM_UPDATE,
	.baseline_bits = FG_PASS_BASELINE_DETERMINISTIC_STATE,
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = R_FG_PassWhenStorePrevEnabled,
	.execute = R_Pass_StorePrevFrame
};

void R_RegisterFrameGraphPasses (void)
{
	(void)R_FrameGraph_AddPass (&s_setup_view_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_shadowmaps_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_scene_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_warp_resolve_framegraph_pass);
	R_Decals_RegisterFrameGraphPasses ();
	(void)R_FrameGraph_AddPass (&s_ssao_fog_handoff_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_postprocess_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_viewmodel_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_polyblend_framegraph_pass);
	(void)R_FrameGraph_AddPass (&s_storeprev_framegraph_pass);
}
