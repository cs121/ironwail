//
// Copyright (C) 2024 Ironwail developers
//
// Lightgrid public interface
//

#pragma once

#include "quakedef.h"
#include "../common/lightgrid.h"

struct qmodel_s;
typedef struct lightgrid_raw_s lightgrid_raw_t;

extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_debug;
extern cvar_t r_lightgrid_force;
extern cvar_t r_generate_lightgrid_test;

void Lightgrid_Init (void);
void Lightgrid_Shutdown (void);
void Lightgrid_Clear (void);
qboolean Lightgrid_LoadFromBSPX (void *bspx_data, int bspx_len);
lightgrid_t *Lightgrid_FromRaw (const lightgrid_raw_t *raw);
lightgrid_raw_t *Lightgrid_GenerateRaw (const struct qmodel_s *model);
void Lightgrid_BuildFallback (void);
void Lightgrid_Sample (const vec3_t pos, vec3_t out_color, vec3_t out_dir);
const lightgrid_t *Lightgrid_Get (void);
void Lightgrid_SetSource (const char *name);
const char *Lightgrid_GetSource (void);

