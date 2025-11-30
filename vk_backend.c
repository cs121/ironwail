#include "renderer_backend.h"
#include <stdio.h>

static void VK_Init(void) {
    printf("VK Init\n");
}

static void VK_Shutdown(void) {
    printf("VK Shutdown\n");
}

static void VK_BeginFrame(void) {
    printf("VK BeginFrame\n");
}

static void VK_EndFrame(void) {
    printf("VK EndFrame\n");
}

static void VK_BeginPass(const struct pass_info_t *info) {
    (void)info;
    printf("VK BeginPass\n");
}

static void VK_EndPass(void) {
    printf("VK EndPass\n");
}

static texture_handle_t VK_CreateTexture(const struct texture_desc_t *desc) {
    (void)desc;
    printf("VK CreateTexture\n");
    return 0;
}

static void VK_DestroyTexture(texture_handle_t handle) {
    (void)handle;
    printf("VK DestroyTexture\n");
}

static mesh_handle_t VK_CreateMesh(const struct mesh_desc_t *desc) {
    (void)desc;
    printf("VK CreateMesh\n");
    return 0;
}

static void VK_DestroyMesh(mesh_handle_t handle) {
    (void)handle;
    printf("VK DestroyMesh\n");
}

static shader_handle_t VK_CreateShader(const struct shader_desc_t *desc) {
    (void)desc;
    printf("VK CreateShader\n");
    return 0;
}

static void VK_DestroyShader(shader_handle_t handle) {
    (void)handle;
    printf("VK DestroyShader\n");
}

static void VK_SetMaterial(shader_handle_t shader) {
    (void)shader;
    printf("VK SetMaterial\n");
}

static void VK_DrawSurface(const struct draw_surf_t *surf) {
    (void)surf;
    printf("VK DrawSurface\n");
}

static struct render_backend_s vk_backend = {
    .Init = VK_Init,
    .Shutdown = VK_Shutdown,
    .BeginFrame = VK_BeginFrame,
    .EndFrame = VK_EndFrame,
    .BeginPass = VK_BeginPass,
    .EndPass = VK_EndPass,
    .CreateTexture = VK_CreateTexture,
    .DestroyTexture = VK_DestroyTexture,
    .CreateMesh = VK_CreateMesh,
    .DestroyMesh = VK_DestroyMesh,
    .CreateShader = VK_CreateShader,
    .DestroyShader = VK_DestroyShader,
    .SetMaterial = VK_SetMaterial,
    .DrawSurface = VK_DrawSurface,
};

struct render_backend_s *VK_GetBackend(void) {
    return &vk_backend;
}

