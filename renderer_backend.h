#ifndef RENDERER_BACKEND_H
#define RENDERER_BACKEND_H

#include <stdint.h>

typedef uint32_t texture_handle_t;
typedef uint32_t mesh_handle_t;
typedef uint32_t shader_handle_t;
typedef uint32_t framebuffer_handle_t;

struct texture_desc_t {
    int width;
    int height;
    int format;
    const void *pixels;
};

struct mesh_desc_t {
    const float *positions;
    const float *normals;
    const float *uvs;
    const uint32_t *indices;
    int vertex_count;
    int index_count;
};

struct shader_desc_t {
    const char *vs;
    const char *fs;
};

struct pass_info_t {
    uint32_t clear_flags;
    float clear_color[4];
    framebuffer_handle_t fb;
};

struct draw_surf_t {
    shader_handle_t shader;
    mesh_handle_t mesh;
    float model_matrix[16];
    int first_index;
    int index_count;
};

struct render_backend_s {
    void (*Init)(void);
    void (*Shutdown)(void);
    void (*BeginFrame)(void);
    void (*EndFrame)(void);
    void (*BeginPass)(const struct pass_info_t *);
    void (*EndPass)(void);
    texture_handle_t (*CreateTexture)(const struct texture_desc_t *);
    void (*DestroyTexture)(texture_handle_t);
    mesh_handle_t (*CreateMesh)(const struct mesh_desc_t *);
    void (*DestroyMesh)(mesh_handle_t);
    shader_handle_t (*CreateShader)(const struct shader_desc_t *);
    void (*DestroyShader)(shader_handle_t);
    void (*SetMaterial)(shader_handle_t);
    void (*DrawSurface)(const struct draw_surf_t *);
};

extern struct render_backend_s *rb;

#endif /* RENDERER_BACKEND_H */
