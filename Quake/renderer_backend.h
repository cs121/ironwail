#ifndef RENDERER_BACKEND_H
#define RENDERER_BACKEND_H

#include "q_stdinc.h"

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL_opengl.h>
#else
#include "SDL_opengl.h"
#endif

typedef uint32_t texture_handle_t;
typedef uint32_t shader_handle_t;
typedef uint32_t mesh_handle_t;
typedef uint32_t framebuffer_handle_t;

typedef vec4_t vec4;

typedef struct pass_info_s {
    GLenum clear_flags;
    vec4 clear_color;
    framebuffer_handle_t target;
} pass_info_t;

typedef struct shader_uniform_def_s {
    const char *name;
    int type;
} shader_uniform_def_t;

typedef struct shader_sampler_binding_s {
    const char *name;
    int texture_unit;
} shader_sampler_binding_t;

typedef struct shader_desc_s {
    const char *vertex_source;
    const char *fragment_source;
    const shader_uniform_def_t *uniforms;
    int uniform_count;
    const shader_sampler_binding_t *samplers;
    int sampler_count;
} shader_desc_t;

typedef struct texture_desc_s {
    int width;
    int height;
    int format;
    const void *pixels;
} texture_desc_t;

typedef struct mesh_desc_s {
    float *positions;
    float *normals;
    float *uvs;
    int *indices;
    int vertex_count;
    int index_count;
} mesh_desc_t;

struct draw_surf_s;
typedef struct draw_surf_s draw_surf_t;

struct material_s;
typedef struct render_backend_s render_backend_t;

struct render_backend_s {
    qboolean (*Init)(void);
    void (*Shutdown)(void);
    void (*BeginFrame)(void);
    void (*EndFrame)(void);

    texture_handle_t (*CreateTexture)(const texture_desc_t *desc);
    void (*DestroyTexture)(texture_handle_t handle);

    shader_handle_t (*CreateShader)(const shader_desc_t *desc);
    void (*DestroyShader)(shader_handle_t handle);

    mesh_handle_t (*CreateMesh)(const mesh_desc_t *desc);
    void (*DestroyMesh)(mesh_handle_t handle);

    void (*BeginPass)(const pass_info_t *info);
    void (*EndPass)(void);

    void (*SetMaterial)(struct material_s *material);
    void (*SetViewport)(int x, int y, int width, int height);

    void (*DrawSurface)(const draw_surf_t *surface);
};

extern struct render_backend_s *rb;

render_backend_t *Null_GetBackend(void);

#endif // RENDERER_BACKEND_H
