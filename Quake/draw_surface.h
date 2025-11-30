#ifndef DRAW_SURFACE_H
#define DRAW_SURFACE_H

#include "renderer_backend.h"

typedef struct material_s material_t;
typedef float matrix4x4_t[16];

typedef struct {
    material_t *material;
    mesh_handle_t mesh;
    matrix4x4_t model_matrix;
    int first_index;
    int index_count;
} draw_surf_t;

void R_SubmitDrawSurface(const draw_surf_t *surf);

#endif // DRAW_SURFACE_H
