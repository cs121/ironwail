/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "glquake.h"

float r_prev_matviewproj[16] = {
	1.f, 0.f, 0.f, 0.f,
	0.f, 1.f, 0.f, 0.f,
	0.f, 0.f, 1.f, 0.f,
	0.f, 0.f, 0.f, 1.f
};
vec3_t r_prev_vieworg = { 0.f, 0.f, 0.f };
double r_prev_frame_time = 0.0;
qboolean r_prev_frame_valid = false;
float r_motionblur_shutter_scale_filtered = 1.f;
qboolean r_motionblur_shutter_scale_valid = false;
qboolean r_frame_rendered_this_update;
viewmedium_t r_view_medium = VIEWMEDIUM_NONE;

void R_StorePrevFrameState (void)
{
	if (!r_frame_rendered_this_update)
	{
		r_prev_frame_valid = false;
		return;
	}

	double prev_time = r_prev_frame_time;

	memcpy (r_prev_matviewproj, r_matviewproj, sizeof (r_prev_matviewproj));
	VectorCopy (r_refdef.vieworg, r_prev_vieworg);

	r_prev_frame_time = cl.time;
	r_prev_frame_valid = (cl.time > prev_time);
	r_frame_rendered_this_update = false;
}

void R_MarkFrameRenderedThisUpdate (void)
{
	r_frame_rendered_this_update = true;
}

qboolean R_PrevFrameValid (void)
{
	return r_prev_frame_valid;
}

viewmedium_t R_GetViewMedium (void)
{
	return r_view_medium;
}
