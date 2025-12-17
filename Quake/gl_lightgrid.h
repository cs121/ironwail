//
// Copyright (C) 2024 Ironwail developers
//
// Lightgrid public interface
//

#pragma once

#include "quakedef.h"
#include "../common/lightgrid.h"

struct qmodel_s;
typedef struct sh9_color_s sh9_color_t;
typedef struct lightgrid_raw_s lightgrid_raw_t;

extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_backend;
extern cvar_t r_lightgrid_debug;
extern cvar_t r_lightgrid_octree_debug;
extern cvar_t r_lightgrid_force;
extern cvar_t r_generate_lightgrid_test;
extern cvar_t r_lightgrid_weight_direct;
extern cvar_t r_lightgrid_weight_lightmap;
extern cvar_t r_lightgrid_surface_weight;
extern cvar_t r_lightgrid_bounce_factor;
extern cvar_t r_lightgrid_bounce_rays;
extern cvar_t r_lightgrid_sh9;
extern cvar_t r_lightgrid_ktx_enable;
extern cvar_t r_lightgrid_ktx_export;
extern cvar_t r_lightgrid_ktx_prefer;

void Lightgrid_Init (void);
void Lightgrid_Shutdown (void);
void Lightgrid_Clear (void);
void Lightgrid_Set (lightgrid_t *lg);
void Lightgrid_UploadToGPU (lightgrid_t *lg);
lightgrid_t *Lightgrid_FromRaw (const lightgrid_raw_t *raw);
lightgrid_raw_t *Lightgrid_GenerateRaw (const struct qmodel_s *model);
void Lightgrid_BuildFallback (void);
qboolean Lightgrid_ValidateOctree (const lightgrid_octree_t *oct, qboolean verbose);
void Lightgrid_Sample (const vec3_t pos, vec3_t out_color, float *out_ao);
qboolean Lightgrid_SampleProbe (const lightgrid_t *lg, const vec3_t pos, lightgrid_probe_t *out_probe);
const lightgrid_t *Lightgrid_Get (void);
void Lightgrid_SetSource (const char *name);
const char *Lightgrid_GetSource (void);
void SH9_EncodeDirectional(vec3_t dir, vec3_t rgb, sh9_color_t *out);

#if USE_KTX2_LIGHTGRID
qboolean Lightgrid_LoadFromKTX2(const char *mapname);
qboolean Lightgrid_ExportToKTX2(const lightgrid_t *grid, const char *mapname);
#endif

