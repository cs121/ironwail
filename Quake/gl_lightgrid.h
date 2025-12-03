//
// Copyright (C) 2024 Ironwail developers
//
// Lightgrid public interface
//

#pragma once

#include "quakedef.h"

typedef struct lightgrid_cell_s
{
    vec3_t  color;
    vec3_t  dir;
    float   intensity;
} lightgrid_cell_t;

typedef struct lightgrid_s
{
    int             nx, ny, nz;
    float           cellsize;
    vec3_t          mins, maxs;
    lightgrid_cell_t *cells;
    qboolean        valid;
} lightgrid_t;

extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_debug;

void Lightgrid_Init (void);
void Lightgrid_Shutdown (void);
void Lightgrid_Clear (void);
qboolean Lightgrid_LoadFromBSPX (void *bspx_data, int bspx_len);
void Lightgrid_BuildFallback (void);
void Lightgrid_Sample (const vec3_t pos, vec3_t out_color, vec3_t out_dir);
const lightgrid_t *Lightgrid_Get (void);

