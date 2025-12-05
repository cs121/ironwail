//
// Copyright (C) 2024 Ironwail developers
//
// Lightgrid public interface
//

#pragma once

#include "quakedef.h"
#include "../common/lightgrid.h"

extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_debug;

void Lightgrid_Init (void);
void Lightgrid_Shutdown (void);
void Lightgrid_Clear (void);
qboolean Lightgrid_LoadFromBSPX (void *bspx_data, int bspx_len);
void Lightgrid_BuildFallback (void);
void Lightgrid_Sample (const vec3_t pos, vec3_t out_color, vec3_t out_dir);
const lightgrid_t *Lightgrid_Get (void);

