//
// Copyright (C) 2024 Ironwail developers
//
// Lightgrid public interface
//

#pragma once

#include "quakedef.h"
#include "lightgrid.h"

struct qmodel_s;
extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_debug;
extern cvar_t r_lightgrid_octree_debug;
extern cvar_t r_lightgrid_force;

void Lightgrid_Init (void);
void Lightgrid_Shutdown (void);
void Lightgrid_Clear (void);
void Lightgrid_Set (lightgrid_t *lg);
qboolean Lightgrid_ValidateOctree (const lightgrid_octree_t *oct, qboolean verbose);
void Lightgrid_Sample (const vec3_t pos, vec3_t out_color, float *out_ao);
qboolean Lightgrid_SampleProbe (const lightgrid_t *lg, const vec3_t pos, lightgrid_probe_t *out_probe);
const lightgrid_t *Lightgrid_Get (void);
const char *Lightgrid_GetSource (void);
