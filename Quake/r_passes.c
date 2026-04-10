#include "quakedef.h"

#include "glquake.h"
#include "r_framegraph.h"

static qboolean R_FG_PassWhenShadowEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_shadowmaps;
}

static qboolean R_FG_PassWhenPostprocessEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan
		&& ctx->frame_plan->run_postprocess;
}

static qboolean R_FG_PassWhenViewmodelEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_viewmodel;
}

static qboolean R_FG_PassWhenPolyblendEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_polyblend;
}

static qboolean R_FG_PassWhenStorePrevEnabled (const RenderPassContext *ctx)
{
	return ctx && ctx->frame_plan && ctx->frame_plan->run_store_prev;
}

static qboolean R_Pass_AllowLegacyFallback (const RenderPassContext *ctx)
{
	const RenderBackendCaps *caps = R_Backend_GetCaps ();

	if (!ctx || !ctx->backend)
		return true;

	return caps && caps->supports_legacy_pass_fallbacks;
}

void R_Pass_SetupView (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_setup_view)
	{
		ctx->backend->pass_setup_view (ctx);
		return;
	}

	/* Compatibility fallback: backend opt-in via capability contract. */
	if (R_Pass_AllowLegacyFallback (ctx))
		R_SetupView ();
}

void R_Pass_RenderShadowMaps (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_shadowmaps)
	{
		ctx->backend->pass_shadowmaps (ctx);
		return;
	}

	if (R_Pass_AllowLegacyFallback (ctx))
		R_RenderShadowMaps ();
}

void R_Pass_RenderScene (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_render_scene)
	{
		ctx->backend->pass_render_scene (ctx);
		return;
	}

	if (R_Pass_AllowLegacyFallback (ctx))
		R_RenderScene (ctx ? ctx->resources : NULL);
}

void R_Pass_WarpResolve (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_warp_resolve)
	{
		ctx->backend->pass_warp_resolve (ctx);
		return;
	}

	if (R_Pass_AllowLegacyFallback (ctx))
		R_WarpScaleView (ctx ? ctx->resources : NULL);
}

void R_Pass_PostProcess (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_postprocess)
	{
		ctx->backend->pass_postprocess (ctx);
		return;
	}

	if (R_Pass_AllowLegacyFallback (ctx))
		GL_PostProcess (ctx ? ctx->resources : NULL);
}

void R_Pass_DrawViewmodel (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_overlay_viewmodel)
	{
		ctx->backend->pass_overlay_viewmodel (ctx);
		return;
	}

	if (R_Pass_AllowLegacyFallback (ctx))
		R_DrawViewModel ();
}

void R_Pass_DrawPolyblend (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_overlay_polyblend)
	{
		ctx->backend->pass_overlay_polyblend (ctx);
		return;
	}

	if (R_Pass_AllowLegacyFallback (ctx))
		V_PolyBlend ();
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
	{ RENDER_RES_SCENE_COLOR, R_BACKEND_LOAD_OP_CLEAR, R_BACKEND_STORE_OP_STORE }
};

static const FGPassAttachmentConfig s_scene_depth_attachment = {
	RENDER_RES_SCENE_DEPTH,
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
	.side_effects = 1u << 0,
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
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = R_FG_PassWhenShadowEnabled,
	.execute = R_Pass_RenderShadowMaps,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_SHADOW
};

static const RenderPassDesc s_scene_framegraph_pass = {
	.name = "Render scene",
	.reads = RENDER_RES_DECALS | RENDER_RES_SHADOW_SUN_DEPTH,
	.writes = RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_VELOCITY,
	.side_effects = 0,
	.output_target = FG_PASS_OUTPUT_AUTO_SCENE,
	.viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT_SCALED,
	.enabled = NULL,
	.execute = R_Pass_RenderScene,
	.color_attachments = s_scene_color_attachments,
	.num_color_attachments = 1,
	.depth_attachment = &s_scene_depth_attachment,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_SCENE
};

static const RenderPassDesc s_warp_resolve_framegraph_pass = {
	.name = "Warp/resolve",
	.reads = RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH,
	.writes = RENDER_RES_COMPOSITE_COLOR | RENDER_RES_COMPOSITE_DEPTH,
	.side_effects = 1u << 0,
	.output_target = FG_PASS_OUTPUT_AUTO_WARP,
	.viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT,
	.enabled = NULL,
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
	.output_target = FG_PASS_OUTPUT_KEEP,
	.viewport_mode = FG_PASS_VIEWPORT_KEEP,
	.enabled = NULL,
	.execute = R_Pass_CaptureSSAOFogHandoff
};

static const RenderPassDesc s_postprocess_framegraph_pass = {
	.name = "Postprocess",
	.reads = RENDER_RES_COMPOSITE_COLOR | RENDER_RES_COMPOSITE_DEPTH | RENDER_RES_SSAO_FOG_STATE,
	.writes = RENDER_RES_COMPOSITE_COLOR,
	.side_effects = 1u << 0,
	.output_target = FG_PASS_OUTPUT_BACKBUFFER,
	.viewport_mode = FG_PASS_VIEWPORT_FULL_WINDOW,
	.enabled = R_FG_PassWhenPostprocessEnabled,
	.execute = R_Pass_PostProcess,
	.color_attachments = s_postprocess_color_attachments,
	.num_color_attachments = 1,
	.stage = FG_PASS_STAGE_MAIN,
	.stats_channel = FG_PASS_STATS_POST
};

static const RenderPassDesc s_viewmodel_framegraph_pass = {
	.name = "Draw viewmodel",
	.reads = RENDER_RES_COMPOSITE_COLOR,
	.writes = RENDER_RES_COMPOSITE_COLOR,
	.side_effects = 1u << 0,
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
	.writes = RENDER_RES_COMPOSITE_COLOR,
	.side_effects = 1u << 0,
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
	.side_effects = 1u << 0,
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
