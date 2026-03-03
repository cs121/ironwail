#include "r_framegraph.h"

#include "r_fogvol.h"

/*
 * Framegraph pass order and data dependencies
 * ------------------------------------------
 * 1) Setup:
 *    - R_SetupView prepares camera/frustum globals used by every render pass.
 *    - R_UpdateDecals updates decal state consumed in opaque rendering.
 * 2) Opaque:
 *    - Fog_EnableGFog + R_RenderScene render solid geometry and scene buffers.
 *    - R_WarpScaleView resolves/warps the scene into composite targets.
 * 3) Transparency/volumetrics:
 *    - R_FogVol_BuildList gathers fog-volume primitives from the current view.
 *    - R_FogVol_Render composites volumetric contribution over scene color.
 * 4) PostFX handoff:
 *    - Capture fog parameters for SSAO (postprocess runs after fog disable).
 *    - Fog_DisableGFog keeps 2D overlays fog-free.
 * 5) Overlay:
 *    - Overlay/UI rendering happens later in SCR_UpdateScreen.
 */
void R_FrameGraph_RenderView (const r_framegraph_state_t *state)
{
	R_SetupView ();
	R_UpdateDecals ();
	Fog_EnableGFog ();
	R_RenderScene ();
	R_WarpScaleView ();

	if (state && state->fogvol_update_called)
		(*state->fogvol_update_called)++;
	R_FogVol_BuildList ();
	if (state && state->fogvol_draw_called)
		(*state->fogvol_draw_called)++;
	R_FogVol_Render ();

	if (state && state->ssao_fog_state)
		R_SSAO_CaptureFogState (&r_framedata, state->ssao_fog_state);

	Fog_DisableGFog ();
	if (state && state->frame_rendered_this_update)
		*state->frame_rendered_this_update = true;
}
