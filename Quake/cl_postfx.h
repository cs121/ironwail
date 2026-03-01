/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2024 Ironwail contributors

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
#ifndef CL_POSTFX_H
#define CL_POSTFX_H

#include "quakedef.h"

typedef enum
{
	PFX_EVENT_PICKUP,
	PFX_EVENT_DAMAGE
} postfx_event_type_t;

typedef enum
{
	PFX_LUT_NONE = 0,
	PFX_LUT_WATER,
	PFX_LUT_SLIME,
	PFX_LUT_LAVA,
	PFX_LUT_PENT,
	PFX_LUT_RING,
	PFX_LUT_SUIT,
	PFX_LUT_QUAD,
	PFX_LUT_COUNT
} postfx_lut_id_t;

typedef struct postfx_state_s
{
	float	exposure_add_stops;
	float	bloom_boost;
	float	vignette;
	float	vignette_softness;
	float	desat;
	int		lut_id;
	float	lut_strength;
	float	underwater_grade_strength;
	float	underwater_fog_strength;
	float	underwater_fog_color[3];
	qboolean	underwater_postfx_active;
	float	emissive_boost;
	float	damage_trauma;
} postfx_state_t;

void CL_PostFX_Init (void);
void CL_PostFX_Reset (void);
void CL_PostFX_Frame (void);
void CL_PostFX_PushPickup (void);
void CL_PostFX_PushDamage (float damage_amount);
void CL_PostFX_SetContents (int contents, qboolean underwater_active, qboolean underwater_postfx_active);
void CL_PostFX_GetState (postfx_state_t *out_state);

#endif
