#include "renderer_backend.h"

static qboolean Null_Init(void)
{
    return true;
}

static void Null_Shutdown(void)
{
}

static void Null_BeginFrame(void)
{
}

static void Null_EndFrame(void)
{
}

static texture_handle_t Null_CreateTexture(int width, int height, const void *pixels)
{
    (void)width;
    (void)height;
    (void)pixels;

    return 0;
}

static void Null_DestroyTexture(texture_handle_t handle)
{
    (void)handle;
}

static shader_handle_t Null_CreateShader(const char *vertex_source, const char *fragment_source)
{
    (void)vertex_source;
    (void)fragment_source;

    return 0;
}

static void Null_DestroyShader(shader_handle_t handle)
{
    (void)handle;
}

static mesh_handle_t Null_CreateMesh(const void *vertex_data, size_t vertex_count, const void *index_data, size_t index_count)
{
    (void)vertex_data;
    (void)vertex_count;
    (void)index_data;
    (void)index_count;

    return 0;
}

static void Null_DestroyMesh(mesh_handle_t handle)
{
    (void)handle;
}

static void Null_BeginPass(struct render_pass_s *pass)
{
    (void)pass;
}

static void Null_EndPass(struct render_pass_s *pass)
{
    (void)pass;
}

static void Null_SetMaterial(struct material_s *material)
{
    (void)material;
}

static void Null_SetViewport(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void Null_DrawSurface(mesh_handle_t mesh)
{
    (void)mesh;
}

static render_backend_t null_backend = {
    Null_Init,
    Null_Shutdown,
    Null_BeginFrame,
    Null_EndFrame,

    Null_CreateTexture,
    Null_DestroyTexture,

    Null_CreateShader,
    Null_DestroyShader,

    Null_CreateMesh,
    Null_DestroyMesh,

    Null_BeginPass,
    Null_EndPass,

    Null_SetMaterial,
    Null_SetViewport,

    Null_DrawSurface,
};

render_backend_t *Null_GetBackend(void)
{
    return &null_backend;
}
