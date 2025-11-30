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

static texture_handle_t Null_CreateTexture(const texture_desc_t *desc)
{
    (void)desc;

    return 0;
}

static void Null_DestroyTexture(texture_handle_t handle)
{
    (void)handle;
}

static shader_handle_t Null_CreateShader(const shader_desc_t *desc)
{
    (void)desc;

    return 0;
}

static void Null_DestroyShader(shader_handle_t handle)
{
    (void)handle;
}

static mesh_handle_t Null_CreateMesh(const mesh_desc_t *desc)
{
    (void)desc;

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

static void Null_DrawSurface(const draw_surf_t *surface)
{
    (void)surface;
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
