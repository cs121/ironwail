#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL_opengl.h>
#else
#include "SDL_opengl.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "renderer_backend.h"

typedef struct render_backend_s render_backend_t;

typedef struct mesh_s {
    GLuint vao;
    GLuint vbo;
    GLuint ibo;
    int vertex_count;
    qboolean used;
} mesh_t;

static mesh_t *mesh_pool;
static size_t mesh_pool_count;
static size_t mesh_pool_capacity;

typedef struct shader_s {
    GLuint program;
    qboolean used;
} shader_t;

static shader_t *shader_pool;
static size_t shader_pool_count;
static size_t shader_pool_capacity;

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

texture_handle_t RB_CreateTexture(const texture_desc_t *desc)
{
    GLuint texture = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, desc->format, desc->width, desc->height, 0, desc->format, GL_UNSIGNED_BYTE, desc->pixels);

    return (texture_handle_t)texture;
}

void RB_DestroyTexture(texture_handle_t handle)
{
    GLuint texture = (GLuint)handle;

    glDeleteTextures(1, &texture);
}

static void RB_LogShaderError(GLuint object, qboolean is_program)
{
    GLint log_length = 0;

    if (is_program)
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &log_length);
    else
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &log_length);

    if (log_length > 1)
    {
        char *log = malloc((size_t)log_length);

        if (log)
        {
            if (is_program)
                glGetProgramInfoLog(object, log_length, NULL, log);
            else
                glGetShaderInfoLog(object, log_length, NULL, log);

            printf("GL shader error: %s\n", log);
            free(log);
        }
    }
}

static GLuint RB_CompileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint status = 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

    if (!status)
    {
        RB_LogShaderError(shader, false);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint RB_LinkProgram(GLuint vertex_shader, GLuint fragment_shader)
{
    GLuint program = glCreateProgram();
    GLint status = 0;

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &status);

    if (!status)
    {
        RB_LogShaderError(program, true);
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

static shader_t *RB_AllocateShaderSlot(size_t *out_index)
{
    size_t index;

    for (index = 0; index < shader_pool_count; ++index)
    {
        if (!shader_pool[index].used)
        {
            shader_pool[index].used = true;
            *out_index = index;
            return &shader_pool[index];
        }
    }

    if (shader_pool_count == shader_pool_capacity)
    {
        size_t new_capacity = shader_pool_capacity ? shader_pool_capacity * 2 : 16;
        shader_t *new_pool = realloc(shader_pool, new_capacity * sizeof(shader_t));

        if (!new_pool)
            return NULL;

        shader_pool = new_pool;
        shader_pool_capacity = new_capacity;
    }

    index = shader_pool_count++;
    shader_pool[index].used = true;
    *out_index = index;

    return &shader_pool[index];
}

shader_handle_t RB_CreateShader(const shader_desc_t *desc)
{
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;
    shader_t *shader;
    size_t shader_index;
    int i;

    vertex_shader = RB_CompileShader(GL_VERTEX_SHADER, desc->vertex_source);
    if (!vertex_shader)
        return 0;

    fragment_shader = RB_CompileShader(GL_FRAGMENT_SHADER, desc->fragment_source);
    if (!fragment_shader)
    {
        glDeleteShader(vertex_shader);
        return 0;
    }

    program = RB_LinkProgram(vertex_shader, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    if (!program)
        return 0;

    shader = RB_AllocateShaderSlot(&shader_index);
    if (!shader)
    {
        glDeleteProgram(program);
        return 0;
    }

    shader->program = program;

    glUseProgram(program);

    for (i = 0; i < desc->uniform_count; ++i)
    {
        GLint location = glGetUniformLocation(program, desc->uniforms[i].name);

        if (location == -1)
            printf("Warning: uniform '%s' not found in shader\n", desc->uniforms[i].name);
    }

    for (i = 0; i < desc->sampler_count; ++i)
    {
        GLint location = glGetUniformLocation(program, desc->samplers[i].name);

        if (location == -1)
        {
            printf("Warning: sampler '%s' not found in shader\n", desc->samplers[i].name);
            continue;
        }

        glUniform1i(location, desc->samplers[i].texture_unit);
    }

    return (shader_handle_t)(shader_index + 1);
}

void RB_DestroyShader(shader_handle_t handle)
{
    size_t index;
    shader_t *shader;

    if (handle == 0)
        return;

    index = handle - 1;

    if (index >= shader_pool_count || !shader_pool[index].used)
        return;

    shader = &shader_pool[index];

    glDeleteProgram(shader->program);
    shader->used = false;
}

static mesh_t *RB_AllocateMeshSlot(size_t *out_index)
{
    size_t index;

    for (index = 0; index < mesh_pool_count; ++index)
    {
        if (!mesh_pool[index].used)
        {
            mesh_pool[index].used = true;
            *out_index = index;
            return &mesh_pool[index];
        }
    }

    if (mesh_pool_count == mesh_pool_capacity)
    {
        size_t new_capacity = mesh_pool_capacity ? mesh_pool_capacity * 2 : 16;
        mesh_t *new_pool = realloc(mesh_pool, new_capacity * sizeof(mesh_t));

        if (!new_pool)
            return NULL;

        mesh_pool = new_pool;
        mesh_pool_capacity = new_capacity;
    }

    index = mesh_pool_count++;
    mesh_pool[index].used = true;
    *out_index = index;

    return &mesh_pool[index];
}

mesh_handle_t RB_CreateMesh(const mesh_desc_t *desc)
{
    const size_t floats_per_vertex = 8; // position (3) + normal (3) + uv (2)
    size_t vertex_buffer_size;
    float *vertex_buffer;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    mesh_t *mesh;
    size_t mesh_index;
    int i;

    if (!desc || desc->vertex_count <= 0 || desc->index_count <= 0 || !desc->positions || !desc->indices)
        return 0;

    vertex_buffer_size = (size_t)desc->vertex_count * floats_per_vertex;
    vertex_buffer = malloc(vertex_buffer_size * sizeof(float));

    if (!vertex_buffer)
        return 0;

    for (i = 0; i < desc->vertex_count; ++i)
    {
        const float *position = desc->positions + i * 3;
        const float *normal = desc->normals ? desc->normals + i * 3 : NULL;
        const float *uv = desc->uvs ? desc->uvs + i * 2 : NULL;
        size_t base = (size_t)i * floats_per_vertex;

        vertex_buffer[base + 0] = position[0];
        vertex_buffer[base + 1] = position[1];
        vertex_buffer[base + 2] = position[2];

        vertex_buffer[base + 3] = normal ? normal[0] : 0.0f;
        vertex_buffer[base + 4] = normal ? normal[1] : 0.0f;
        vertex_buffer[base + 5] = normal ? normal[2] : 0.0f;

        vertex_buffer[base + 6] = uv ? uv[0] : 0.0f;
        vertex_buffer[base + 7] = uv ? uv[1] : 0.0f;
    }

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_buffer_size * sizeof(float), vertex_buffer, GL_STATIC_DRAW);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)desc->index_count * sizeof(int), desc->indices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)(floats_per_vertex * sizeof(float)), (void *)(uintptr_t)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)(floats_per_vertex * sizeof(float)), (void *)(uintptr_t)(3 * sizeof(float)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)(floats_per_vertex * sizeof(float)), (void *)(uintptr_t)(6 * sizeof(float)));

    free(vertex_buffer);

    mesh = RB_AllocateMeshSlot(&mesh_index);

    if (!mesh)
    {
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ibo);
        glDeleteVertexArrays(1, &vao);
        return 0;
    }

    mesh->vao = vao;
    mesh->vbo = vbo;
    mesh->ibo = ibo;
    mesh->vertex_count = desc->vertex_count;

    return (mesh_handle_t)(mesh_index + 1);
}

void RB_DestroyMesh(mesh_handle_t handle)
{
    size_t index;
    mesh_t *mesh;

    if (handle == 0)
        return;

    index = handle - 1;

    if (index >= mesh_pool_count || !mesh_pool[index].used)
        return;

    mesh = &mesh_pool[index];

    glDeleteBuffers(1, &mesh->vbo);
    glDeleteBuffers(1, &mesh->ibo);
    glDeleteVertexArrays(1, &mesh->vao);

    mesh->used = false;
}

void RB_BeginPass(const pass_info_t *info)
{
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)info->target);

    glClearColor(info->clear_color[0], info->clear_color[1], info->clear_color[2], info->clear_color[3]);
    glClear(info->clear_flags);
}

void RB_EndPass(void)
{
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

