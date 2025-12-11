#pragma once

#include "quakedef.h"

typedef struct lightcell_s {
    vec3_t rgb;
    vec3_t dir;
    float intensity;
    float ao;
    // placeholder for future SH9
} lightcell_t;
typedef lightcell_t lightgrid_probe_t;

#define LIGHTGRID_STANDARD_CELLSIZE 32.f
#define LIGHTGRID_MAX_NX 64
#define LIGHTGRID_MAX_NY 64
#define LIGHTGRID_MAX_NZ 128
#define LIGHTGRID_MAX_CELLS (512 * 1024)

typedef struct lightgrid_s {
    int nx, ny, nz;
    float cellsize;
    vec3_t mins, maxs;

    lightcell_t *probes; // size = nx*ny*nz

    int source; // enum lightgrid_source_e
    GLuint tex_color3d;
    GLuint tex_dir3d;
    GLuint tex_intensity;
    GLuint tex_ao;
} lightgrid_t;

typedef enum lightgrid_source_e {
    LIGHTGRID_SRC_NONE = 0,
    LIGHTGRID_SRC_OCTREE,
    LIGHTGRID_SRC_RAW,
    LIGHTGRID_SRC_KTX2
} lightgrid_source_t;

lightgrid_t *Lightgrid_Alloc(int nx, int ny, int nz, float cellsize, const vec3_t mins, const vec3_t maxs);
lightgrid_t *Lightgrid_LoadV2(const char *path);
lightgrid_t *Lightgrid_LoadExternal(const char *path);
void Lightgrid_Free(lightgrid_t *lg);
