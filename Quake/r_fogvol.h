/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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

#ifndef __R_FOGVOL_H__
#define __R_FOGVOL_H__

#include "q_stdinc.h"

typedef struct fog_volume_s
{
	float mins[3];
	float maxs[3];
	float color[3];
	float density;
	float noise_scale;
	float noise_strength;
	float velocity[3];
	float falloff;
	int flags;
} fog_volume_t;

typedef struct froxel_grid_s froxel_grid_t;

void R_FogVol_Init (void);
void R_FogVol_Clear (void);
void R_FogVol_ParseEntities (void);
void R_FogVol_BuildList (void);
void R_FogVol_AddTestVolumes (void);
void R_FogVol_Render (void);
void R_FogVol_DrawDebug2D (void);
void R_FogVol_LogEndFrameState (void);
void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num);
int R_FogVol_BindForFroxelBuild (void);
void R_FogVol_InjectBuiltIntoFroxel (void);
qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres);
void R_FogVol_NotifyFramebuffersRecreated (void);

#endif /* __R_FOGVOL_H__ */
