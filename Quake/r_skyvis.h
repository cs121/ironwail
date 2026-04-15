#pragma once

#include "quakedef.h"

#ifndef RENDERER_PLUGIN_BUILD
extern cvar_t r_skyvis;
extern cvar_t r_skyvis_debug;
#endif
extern cvar_t r_skyvis_scale;
extern cvar_t r_skyvis_cap;
extern cvar_t r_skyvis_spacing_xy;
extern cvar_t r_skyvis_spacing_z;
extern cvar_t r_skyvis_rays;

void R_SkyVis_Init (void);
void R_SkyVis_Shutdown (void);
void R_SkyVis_Clear (void);
void R_SkyVis_NewMap (void);
qboolean R_SkyVis_Active (void);
float R_SkyVis_Sample (const vec3_t pos);
void R_SkyVis_GetTint (vec3_t out_tint);
float R_SkyVis_GetResolvedScale (void);
float R_SkyVis_GetResolvedCap (void);
