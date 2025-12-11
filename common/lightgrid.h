#pragma once

#include "quakedef.h"

typedef struct sh9_color_s {
    vec3_t c[9];
} sh9_color_t;

typedef struct lightcell_s {
    vec3_t rgb;
    vec3_t dir;
    float intensity;
    float ao;
    float emissive;
    qboolean sh_valid;
    sh9_color_t *sh9; // NULL unless Lightgrid V3 allocates it
} lightcell_t;
typedef lightcell_t lightgrid_probe_t;

#define LIGHTGRID_STANDARD_CELLSIZE 32.f
#define LIGHTGRID_MAX_NX 128
#define LIGHTGRID_MAX_NY 128
#define LIGHTGRID_MAX_NZ 256
#define LIGHTGRID_MAX_CELLS (4 * 1024 * 1024)

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

typedef enum lightgrid_component_e {
    LIGHTGRID_COMPONENT_RGB = (1u << 0),
    LIGHTGRID_COMPONENT_DIR = (1u << 1),
    LIGHTGRID_COMPONENT_INTENSITY = (1u << 2),
    LIGHTGRID_COMPONENT_AO = (1u << 3),
    LIGHTGRID_COMPONENT_SH9_FLAG = (1u << 8) // Future V3 SH9 data
} lightgrid_component_t;

typedef enum lightgrid_source_e {
    LIGHTGRID_SRC_NONE = 0,
    LIGHTGRID_SRC_V2,
    LIGHTGRID_SRC_OCTREE,
    LIGHTGRID_SRC_RAW,
    LIGHTGRID_SRC_KTX2
} lightgrid_source_t;

lightgrid_t *Lightgrid_Alloc(int nx, int ny, int nz, float cellsize, const vec3_t mins, const vec3_t maxs);
lightgrid_t *Lightgrid_LoadV2(const char *path);
lightgrid_t *Lightgrid_LoadKTX2(const char *path);
lightgrid_t *Lightgrid_LoadV3(const char *path);
lightgrid_t *Lightgrid_LoadExternal(const char *path);
void Lightgrid_Free(lightgrid_t *lg);
