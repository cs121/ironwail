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

static qboolean R_Pass_IsOpenGLBackend (const RenderPassContext *ctx)
{
	return ctx && ctx->backend && ctx->backend->name
		&& !q_strcasecmp (ctx->backend->name, "OpenGL");
}

void R_Pass_SetupView (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_setup_view)
	{
		ctx->backend->pass_setup_view (ctx);
		return;
	}

	/* Legacy fallback: keep OpenGL framegraph path working. */
	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		R_SetupView ();
}

void R_Pass_RenderShadowMaps (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_shadowmaps)
	{
		ctx->backend->pass_shadowmaps (ctx);
		return;
	}

	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		R_RenderShadowMaps ();
}

void R_Pass_RenderScene (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_render_scene)
	{
		ctx->backend->pass_render_scene (ctx);
		return;
	}

	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		R_RenderScene (ctx ? ctx->resources : NULL);
}

void R_Pass_WarpResolve (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_warp_resolve)
	{
		ctx->backend->pass_warp_resolve (ctx);
		return;
	}

	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		R_WarpScaleView (ctx ? ctx->resources : NULL);
}

void R_Pass_PostProcess (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_postprocess)
	{
		ctx->backend->pass_postprocess (ctx);
		return;
	}

	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		GL_PostProcess (ctx ? ctx->resources : NULL);
}

void R_Pass_DrawViewmodel (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_overlay_viewmodel)
	{
		ctx->backend->pass_overlay_viewmodel (ctx);
		return;
	}

	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		R_DrawViewModel ();
}

void R_Pass_DrawPolyblend (RenderPassContext *ctx)
{
	if (ctx && ctx->backend && ctx->backend->pass_overlay_polyblend)
	{
		ctx->backend->pass_overlay_polyblend (ctx);
		return;
	}

	if (!ctx || R_Pass_IsOpenGLBackend (ctx))
		V_PolyBlend ();
}

static void R_Pass_CaptureSSAOFogHandoff (RenderPassContext *ctx)
{
	(void)ctx;
	/* Capture global fog parameters for deterministic postprocess SSAO suppression. */
	R_SSAO_CaptureFogState (&r_framedata, &r_ssao_fog_state);
}

static void R_Pass_StorePrevFrame (RenderPassContext *ctx)
{
	(void)ctx;
	r_frame_rendered_this_update = true;
	R_StorePrevFrameState ();
}

static const RenderPassDesc s_setup_view_framegraph_pass = {
	"Setup view",
	RENDER_RES_NONE,
	RENDER_RES_NONE,
	1u << 0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	NULL,
	R_Pass_SetupView,
	FG_PASS_STAGE_SETUP,
	FG_PASS_STATS_SETUP
};

static const RenderPassDesc s_shadowmaps_framegraph_pass = {
	"Shadow maps",
	RENDER_RES_NONE,
	RENDER_RES_SHADOW_SUN_DEPTH,
	0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	R_FG_PassWhenShadowEnabled,
	R_Pass_RenderShadowMaps,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_SHADOW
};

static const RenderPassDesc s_scene_framegraph_pass = {
	"Render scene",
	RENDER_RES_DECALS | RENDER_RES_SHADOW_SUN_DEPTH,
	RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH | RENDER_RES_VELOCITY,
	0,
	FG_PASS_OUTPUT_AUTO_SCENE,
	FG_PASS_VIEWPORT_VIEW_RECT_SCALED,
	NULL,
	R_Pass_RenderScene,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_SCENE
};

static const RenderPassDesc s_warp_resolve_framegraph_pass = {
	"Warp/resolve",
	RENDER_RES_SCENE_COLOR | RENDER_RES_SCENE_DEPTH,
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_COMPOSITE_DEPTH,
	1u << 0,
	FG_PASS_OUTPUT_AUTO_WARP,
	FG_PASS_VIEWPORT_VIEW_RECT,
	NULL,
	R_Pass_WarpResolve,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_WARP
};

static const RenderPassDesc s_ssao_fog_handoff_framegraph_pass = {
	"Capture fog handoff",
	RENDER_RES_NONE,
	RENDER_RES_SSAO_FOG_STATE,
	0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	NULL,
	R_Pass_CaptureSSAOFogHandoff
};

static const RenderPassDesc s_postprocess_framegraph_pass = {
	"Postprocess",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_COMPOSITE_DEPTH | RENDER_RES_SSAO_FOG_STATE,
	RENDER_RES_COMPOSITE_COLOR,
	1u << 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_VIEWPORT_FULL_WINDOW,
	R_FG_PassWhenPostprocessEnabled,
	R_Pass_PostProcess,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_POST
};

static const RenderPassDesc s_viewmodel_framegraph_pass = {
	"Draw viewmodel",
	RENDER_RES_COMPOSITE_COLOR,
	RENDER_RES_COMPOSITE_COLOR,
	1u << 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_VIEWPORT_VIEW_RECT,
	R_FG_PassWhenViewmodelEnabled,
	R_Pass_DrawViewmodel,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_OVERLAY
};

static const RenderPassDesc s_polyblend_framegraph_pass = {
	"Polyblend",
	RENDER_RES_COMPOSITE_COLOR,
	RENDER_RES_COMPOSITE_COLOR,
	1u << 0,
	FG_PASS_OUTPUT_BACKBUFFER,
	FG_PASS_VIEWPORT_VIEW_RECT,
	R_FG_PassWhenPolyblendEnabled,
	R_Pass_DrawPolyblend,
	FG_PASS_STAGE_MAIN,
	FG_PASS_STATS_OVERLAY
};

static const RenderPassDesc s_storeprev_framegraph_pass = {
	"Store previous frame",
	RENDER_RES_COMPOSITE_COLOR | RENDER_RES_SCENE_DEPTH,
	RENDER_RES_NONE,
	1u << 0,
	FG_PASS_OUTPUT_KEEP,
	FG_PASS_VIEWPORT_KEEP,
	R_FG_PassWhenStorePrevEnabled,
	R_Pass_StorePrevFrame
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
