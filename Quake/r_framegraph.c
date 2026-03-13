#include "quakedef.h"

#include "r_framegraph.h"

#include "r_fogvol.h"

void R_SetupView (void);
void R_RenderShadowMaps (void);
void R_RenderScene (void);
void R_WarpScaleView (void);
void R_DrawViewModel (void);

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
 *    - R_FogVol_BuildList gathers local/entity fog volumes and prepares global fog state.
 *    - R_FogVol_Render composites a global baseline first, then local/entity fog volumes.
 * 4) PostFX handoff:
 *    - Capture fog parameters for SSAO (postprocess runs after fog disable).
 *    - Fog_DisableGFog keeps 2D overlays fog-free.
 * 5) Overlay:
 *    - Overlay/UI rendering happens later in SCR_UpdateScreen.
 */
void R_FrameGraph_RenderView (const r_framegraph_state_t *state)
{
	qboolean run_fogvol_pass;

	R_SetupView ();
	R_RenderShadowMaps ();
	R_UpdateDecals ();
	Fog_EnableGFog ();
	R_RenderScene ();
	R_WarpScaleView ();
	run_fogvol_pass = R_FogVol_IsEnabledForFrame () && R_FogVol_HasRenderableContent ();
	if (state && state->prepare_fogvol_inputs)
		state->prepare_fogvol_inputs ();

	if (run_fogvol_pass)
	{
		if (state && state->fogvol_update_called)
			(*state->fogvol_update_called)++;
		R_FogVol_BuildList ();
		if (state && state->fogvol_draw_called)
			(*state->fogvol_draw_called)++;
		R_FogVol_Render ();
	}

	/* Draw the weapon after volumetric fog so viewmodel depth cannot punch holes
	 * into fog/froxel reconstruction and temporal history. */
	R_DrawViewModel ();

	if (state && state->ssao_fog_state)
		R_SSAO_CaptureFogState (&r_framedata, state->ssao_fog_state);

	Fog_DisableGFog ();
	if (state && state->frame_rendered_this_update)
		*state->frame_rendered_this_update = true;
}
