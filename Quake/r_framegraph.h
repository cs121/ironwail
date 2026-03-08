#ifndef R_FRAMEGRAPH_H
#define R_FRAMEGRAPH_H

#include "quakedef.h"
#include "r_ssao.h"

typedef struct r_framegraph_state_s
{
	int *fogvol_update_called;
	int *fogvol_draw_called;
	void (*prepare_fogvol_inputs)(void);
	qboolean *frame_rendered_this_update;
	r_ssao_fog_state_t *ssao_fog_state;
} r_framegraph_state_t;

void R_FrameGraph_RenderView (const r_framegraph_state_t *state);

#endif
