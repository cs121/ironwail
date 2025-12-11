#pragma once

#include "quakedef.h"

// BSPX lightgrid probe decoded from octree lightgrid data
typedef struct lightgrid_probe_s {
    float rgb[3];
    float dir[3];
    float intensity;
} lightgrid_probe_t;

#define LIGHTGRID_STANDARD_CELLSIZE 32.f
#define LIGHTGRID_MAX_NX 64
#define LIGHTGRID_MAX_NY 64
#define LIGHTGRID_MAX_NZ 128
#define LIGHTGRID_MAX_CELLS (512 * 1024)

typedef struct lightgrid_s {
    int nx, ny, nz;
    float cellsize;
    vec3_t mins, maxs;

    lightgrid_probe_t *probes; // size = nx*ny*nz

    int source; // enum lightgrid_source_e
    unsigned int tex_color3d;
    unsigned int tex_dir3d;
} lightgrid_t;

typedef enum lightgrid_source_e {
    LIGHTGRID_SRC_NONE = 0,
    LIGHTGRID_SRC_OCTREE,
    LIGHTGRID_SRC_RAW,
    LIGHTGRID_SRC_KTX2
} lightgrid_source_t;

typedef struct bspx_lump_s {
    const void *data;
    size_t size;
} bspx_lump_t;

lightgrid_t *Lightgrid_Alloc(int nx, int ny, int nz, float cellsize, const vec3_t mins, const vec3_t maxs);
lightgrid_t *Lightgrid_LoadFromBSPX_Octree(const bspx_lump_t *l);
lightgrid_t *Lightgrid_LoadFromBSPX_FTE(const bspx_lump_t *l);
void Lightgrid_Free(lightgrid_t *lg);
