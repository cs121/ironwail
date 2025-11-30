#ifndef RENDERER_BACKEND_H
#define RENDERER_BACKEND_H

#include "q_stdinc.h"

typedef uint32_t texture_handle_t;
typedef uint32_t shader_handle_t;
typedef uint32_t mesh_handle_t;

struct material_s;
struct render_pass_s;

struct render_backend_s {
    qboolean (*Init)(void);
    void (*Shutdown)(void);
    void (*BeginFrame)(void);
    void (*EndFrame)(void);

    texture_handle_t (*CreateTexture)(int width, int height, const void *pixels);
    void (*DestroyTexture)(texture_handle_t handle);

    shader_handle_t (*CreateShader)(const char *vertex_source, const char *fragment_source);
    void (*DestroyShader)(shader_handle_t handle);

    mesh_handle_t (*CreateMesh)(const void *vertex_data, size_t vertex_count, const void *index_data, size_t index_count);
    void (*DestroyMesh)(mesh_handle_t handle);

    void (*BeginPass)(struct render_pass_s *pass);
    void (*EndPass)(struct render_pass_s *pass);

    void (*SetMaterial)(struct material_s *material);
    void (*SetViewport)(int x, int y, int width, int height);

    void (*DrawSurface)(mesh_handle_t mesh);
};

extern struct render_backend_s *rb;

#endif // RENDERER_BACKEND_H
