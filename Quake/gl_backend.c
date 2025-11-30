#include <stdio.h>

#include "renderer_backend.h"

typedef struct render_backend_s render_backend_t;

qboolean RB_Init(void)
{
    printf("GL backend init\n");
    return true;
}

void RB_Shutdown(void)
{
    printf("GL backend shutdown\n");
}

void RB_BeginFrame(void)
{
    printf("begin frame placeholder\n");
}

void RB_EndFrame(void)
{
    printf("end frame placeholder\n");
}

texture_handle_t RB_CreateTexture(int width, int height, const void *pixels)
{
    (void)width;
    (void)height;
    (void)pixels;

    printf("create texture placeholder\n");
    return 0;
}

void RB_DestroyTexture(texture_handle_t handle)
{
    (void)handle;

    printf("destroy texture placeholder\n");
}

shader_handle_t RB_CreateShader(const char *vertex_source, const char *fragment_source)
{
    (void)vertex_source;
    (void)fragment_source;

    printf("create shader placeholder\n");
    return 0;
}

void RB_DestroyShader(shader_handle_t handle)
{
    (void)handle;

    printf("destroy shader placeholder\n");
}

mesh_handle_t RB_CreateMesh(const void *vertex_data, size_t vertex_count, const void *index_data, size_t index_count)
{
    (void)vertex_data;
    (void)vertex_count;
    (void)index_data;
    (void)index_count;

    printf("create mesh placeholder\n");
    return 0;
}

void RB_DestroyMesh(mesh_handle_t handle)
{
    (void)handle;

    printf("destroy mesh placeholder\n");
}

void RB_BeginPass(struct render_pass_s *pass)
{
    (void)pass;

    printf("begin pass placeholder\n");
}

void RB_EndPass(struct render_pass_s *pass)
{
    (void)pass;

    printf("end pass placeholder\n");
}

void RB_SetMaterial(struct material_s *material)
{
    (void)material;

    printf("set material placeholder\n");
}

void RB_SetViewport(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;

    printf("set viewport placeholder\n");
}

void RB_DrawSurface(const draw_surf_t *surface)
{
    (void)surface;

    printf("draw surface placeholder\n");
}

render_backend_t gl_backend = {
    .Init = RB_Init,
    .Shutdown = RB_Shutdown,
    .BeginFrame = RB_BeginFrame,
    .EndFrame = RB_EndFrame,
    .CreateTexture = RB_CreateTexture,
    .DestroyTexture = RB_DestroyTexture,
    .CreateShader = RB_CreateShader,
    .DestroyShader = RB_DestroyShader,
    .CreateMesh = RB_CreateMesh,
    .DestroyMesh = RB_DestroyMesh,
    .BeginPass = RB_BeginPass,
    .EndPass = RB_EndPass,
    .SetMaterial = RB_SetMaterial,
    .SetViewport = RB_SetViewport,
    .DrawSurface = RB_DrawSurface,
};

render_backend_t *GL_GetBackend(void)
{
    return &gl_backend;
}

