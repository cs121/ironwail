#include "quakedef.h"
#include "render_dispatch.h"

const iw_renderer_entry_points_t *g_rend = NULL;

static iw_renderer_entry_points_t s_entries;

void RenderDispatch_Init (void)
{
	memset (&s_entries, 0, sizeof (s_entries));
	g_rend = &s_entries;
}

void RenderDispatch_SetEntryPoints (const iw_renderer_entry_points_t *entry_points)
{
	if (entry_points)
		s_entries = *entry_points;
	else
		memset (&s_entries, 0, sizeof (s_entries));
}
