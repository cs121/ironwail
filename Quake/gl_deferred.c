/*
Copyright (C) 2024

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#define GL_GLEXT_PROTOTYPES 1
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL_opengl.h>
#else
#include "SDL_opengl.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "quakedef.h"
#include "gl_deferred.h"
#include "gl_gbuffer.h"

#define DEFERRED_SPHERE_SECTORS 24
#define DEFERRED_SPHERE_STACKS  16

extern cvar_t r_dynamic;
extern dlight_t cl_dlights[MAX_DLIGHTS];

static GLuint deferred_program;
static GLuint deferred_vao;
static GLuint deferred_vbo;
static GLuint deferred_ebo;
static GLsizei deferred_index_count;
static GLuint composite_program;
static GLuint composite_vao;
static GLuint composite_vbo;

static GLint loc_viewproj;
static GLint loc_inv_viewproj;
static GLint loc_light_pos;
static GLint loc_light_color;
static GLint loc_light_radius;
static GLint loc_camera_pos;
static GLint loc_screen_size;
static GLint loc_spec_power;

static void R_Deferred_LogShader(GLuint object, qboolean is_program)
{
    GLint status = 0;

    if (is_program)
        GL_GetProgramivFunc(object, GL_LINK_STATUS, &status);
    else
        GL_GetShaderivFunc(object, GL_COMPILE_STATUS, &status);

    if (status)
        return;

    GLint log_length = 0;
    if (is_program)
        GL_GetProgramivFunc(object, GL_INFO_LOG_LENGTH, &log_length);
    else
        GL_GetShaderivFunc(object, GL_INFO_LOG_LENGTH, &log_length);

    if (log_length > 1)
    {
        char *log = (char *)malloc((size_t)log_length);
        if (log)
        {
            if (is_program)
                GL_GetProgramInfoLogFunc(object, log_length, NULL, log);
            else
                GL_GetShaderInfoLogFunc(object, log_length, NULL, log);
            Con_Printf("Deferred shader error: %s\n", log);
            free(log);
        }
    }
}

static GLuint R_Deferred_Compile(GLenum type, const char *source)
{
    GLuint shader = GL_CreateShaderFunc(type);
    GL_ShaderSourceFunc(shader, 1, &source, NULL);
    GL_CompileShaderFunc(shader);
    R_Deferred_LogShader(shader, false);
    return shader;
}

static qboolean R_Deferred_CreateProgram(void)
{
    const char *vs_source =
        "#version 330 core\n"
        "layout(location = 0) in vec3 aPosition;\n"
        "uniform mat4 uViewProj;\n"
        "uniform vec3 uLightPos;\n"
        "uniform float uLightRadius;\n"
        "void main() {\n"
        "    vec3 world = aPosition * uLightRadius + uLightPos;\n"
        "    gl_Position = uViewProj * vec4(world, 1.0);\n"
        "}\n";

    const char *fs_source =
        "#version 330 core\n"
        "uniform sampler2D uAlbedo;\n"
        "uniform sampler2D uNormalSpec;\n"
        "uniform sampler2D uDepth;\n"
        "uniform mat4 uInvViewProj;\n"
        "uniform vec3 uLightPos;\n"
        "uniform vec3 uLightColor;\n"
        "uniform float uLightRadius;\n"
        "uniform vec3 uCameraPos;\n"
        "uniform vec2 uScreenSize;\n"
        "uniform float uSpecPower;\n"
        "out vec4 FragColor;\n"
        "vec3 DecodeNormal(vec3 n) { return normalize(n); }\n"
        "void main() {\n"
        "    vec2 uv = gl_FragCoord.xy / uScreenSize;\n"
        "    float depth = texture(uDepth, uv).r;\n"
        "    if (depth >= 1.0) discard;\n"
        "    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);\n"
        "    vec4 world = uInvViewProj * ndc;\n"
        "    world /= world.w;\n"
        "    vec3 pos = world.xyz;\n"
        "    vec4 normalSpec = texture(uNormalSpec, uv);\n"
        "    vec3 normal = DecodeNormal(normalSpec.xyz);\n"
        "    float specPow = normalSpec.w;\n"
        "    if (specPow <= 0.0) specPow = uSpecPower;\n"
        "    vec3 albedo = texture(uAlbedo, uv).rgb;\n"
        "    vec3 L = uLightPos - pos;\n"
        "    float dist = length(L);\n"
        "    if (dist > uLightRadius) discard;\n"
        "    vec3 Ldir = L / max(dist, 0.0001);\n"
        "    float atten = max(1.0 - dist / uLightRadius, 0.0);\n"
        "    float diffuse = max(dot(normal, Ldir), 0.0);\n"
        "    vec3 V = normalize(uCameraPos - pos);\n"
        "    vec3 R = reflect(-Ldir, normal);\n"
        "    float specular = pow(max(dot(R, V), 0.0), specPow);\n"
        "    vec3 lighting = (diffuse * albedo + specular) * uLightColor * atten;\n"
        "    FragColor = vec4(lighting, 1.0);\n"
        "}\n";

    GLuint vs = R_Deferred_Compile(GL_VERTEX_SHADER, vs_source);
    GLuint fs = R_Deferred_Compile(GL_FRAGMENT_SHADER, fs_source);

    deferred_program = GL_CreateProgramFunc();
    GL_AttachShaderFunc(deferred_program, vs);
    GL_AttachShaderFunc(deferred_program, fs);
    GL_LinkProgramFunc(deferred_program);
    R_Deferred_LogShader(deferred_program, true);

    GL_DeleteShaderFunc(vs);
    GL_DeleteShaderFunc(fs);

    if (!deferred_program)
        return false;

    GL_UseProgramFunc(deferred_program);
    GL_Uniform1iFunc(GL_GetUniformLocationFunc(deferred_program, "uAlbedo"), 0);
    GL_Uniform1iFunc(GL_GetUniformLocationFunc(deferred_program, "uNormalSpec"), 1);
    GL_Uniform1iFunc(GL_GetUniformLocationFunc(deferred_program, "uDepth"), 2);

    loc_viewproj = GL_GetUniformLocationFunc(deferred_program, "uViewProj");
    loc_inv_viewproj = GL_GetUniformLocationFunc(deferred_program, "uInvViewProj");
    loc_light_pos = GL_GetUniformLocationFunc(deferred_program, "uLightPos");
    loc_light_color = GL_GetUniformLocationFunc(deferred_program, "uLightColor");
    loc_light_radius = GL_GetUniformLocationFunc(deferred_program, "uLightRadius");
    loc_camera_pos = GL_GetUniformLocationFunc(deferred_program, "uCameraPos");
    loc_screen_size = GL_GetUniformLocationFunc(deferred_program, "uScreenSize");
    loc_spec_power = GL_GetUniformLocationFunc(deferred_program, "uSpecPower");

    return true;
}

static void R_Deferred_CreateSphere(void)
{
    const int sectors = DEFERRED_SPHERE_SECTORS;
    const int stacks = DEFERRED_SPHERE_STACKS;
    const int vertex_count = (sectors + 1) * (stacks + 1);
    const int index_count = sectors * stacks * 6;
    float *vertices = (float *)malloc((size_t)vertex_count * 3 * sizeof(float));
    GLuint *indices = (GLuint *)malloc((size_t)index_count * sizeof(GLuint));

    if (!vertices || !indices)
    {
        free(vertices);
        free(indices);
        return;
    }

    for (int i = 0; i <= stacks; ++i)
    {
        float v = (float)i / (float)stacks;
        float phi = (float)M_PI * v;
        float sin_phi = sinf(phi);
        float cos_phi = cosf(phi);

        for (int j = 0; j <= sectors; ++j)
        {
            float u = (float)j / (float)sectors;
            float theta = 2.0f * (float)M_PI * u;
            float sin_theta = sinf(theta);
            float cos_theta = cosf(theta);

            int index = (i * (sectors + 1) + j) * 3;
            vertices[index + 0] = sin_phi * cos_theta;
            vertices[index + 1] = cos_phi;
            vertices[index + 2] = sin_phi * sin_theta;
        }
    }

    GLuint *idx = indices;
    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < sectors; ++j)
        {
            int first = i * (sectors + 1) + j;
            int second = first + sectors + 1;

            *idx++ = (GLuint)first;
            *idx++ = (GLuint)second;
            *idx++ = (GLuint)(first + 1);

            *idx++ = (GLuint)(first + 1);
            *idx++ = (GLuint)second;
            *idx++ = (GLuint)(second + 1);
        }
    }

    deferred_index_count = (GLsizei)index_count;

    GL_GenVertexArraysFunc(1, &deferred_vao);
    GL_BindVertexArrayFunc(deferred_vao);

    GL_GenBuffersFunc(1, &deferred_vbo);
    GL_BindBufferFunc(GL_ARRAY_BUFFER, deferred_vbo);
    GL_BufferDataFunc(GL_ARRAY_BUFFER, (size_t)vertex_count * 3 * sizeof(float), vertices, GL_STATIC_DRAW);

    GL_GenBuffersFunc(1, &deferred_ebo);
    GL_BindBufferFunc(GL_ELEMENT_ARRAY_BUFFER, deferred_ebo);
    GL_BufferDataFunc(GL_ELEMENT_ARRAY_BUFFER, (size_t)index_count * sizeof(GLuint), indices, GL_STATIC_DRAW);

    GL_EnableVertexAttribArrayFunc(0);
    GL_VertexAttribPointerFunc(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)(uintptr_t)0);

    GL_BindVertexArrayFunc(0);

    free(vertices);
    free(indices);
}

static qboolean R_Deferred_EnsureResources(void)
{
    if (!deferred_program)
    {
        if (!R_Deferred_CreateProgram())
            return false;
    }

    if (!deferred_vao)
        R_Deferred_CreateSphere();

    return deferred_program != 0 && deferred_vao != 0;
}

static qboolean R_Deferred_Invert(const float m[16], float out[16])
{
    float inv[16];
    float det;

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15]
             - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15]
             + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14]
              - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15]
             - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15]
             + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15]
             - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14]
              + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15]
             + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15]
             - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15]
              + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14]
              - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
             - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
             + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
              - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
              + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-8f)
        return false;

    det = 1.f / det;

    for (int i = 0; i < 16; i++)
        out[i] = inv[i] * det;

    return true;
}

static void R_Deferred_SetMatrices(void)
{
    float inv_viewproj[16];
    float viewproj[16];

    memcpy(viewproj, r_matviewproj, sizeof(viewproj));

    if (!R_Deferred_Invert(viewproj, inv_viewproj))
        memcpy(inv_viewproj, r_identity_mat4, sizeof(inv_viewproj));

    GL_UniformMatrix4fvFunc(loc_viewproj, 1, GL_FALSE, viewproj);
    GL_UniformMatrix4fvFunc(loc_inv_viewproj, 1, GL_FALSE, inv_viewproj);
}

void R_Deferred_LightPass(void)
{
    if (!r_dynamic.value)
        return;

    if (!R_Deferred_EnsureResources())
        return;

    GL_UseProgramFunc(deferred_program);
    GL_BindVertexArrayFunc(deferred_vao);

    GL_ActiveTextureFunc(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R_GBuffer_GetAlbedoTexture());
    GL_ActiveTextureFunc(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, R_GBuffer_GetNormalSpecTexture());
    GL_ActiveTextureFunc(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, R_GBuffer_GetDepthTexture());

    GL_Uniform2fFunc(loc_screen_size, (float)glwidth, (float)glheight);
    GL_Uniform3fvFunc(loc_camera_pos, 1, r_origin);
    GL_Uniform1fFunc(loc_spec_power, 32.0f);
    R_Deferred_SetMatrices();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);

    for (int i = 0; i < MAX_DLIGHTS; ++i)
    {
        dlight_t *dl = &cl_dlights[i];

        if (dl->die < cl.time || dl->radius <= 0.0f)
            continue;

        GL_Uniform3fvFunc(loc_light_pos, 1, dl->origin);
        GL_Uniform3fvFunc(loc_light_color, 1, dl->color);
        GL_Uniform1fFunc(loc_light_radius, dl->radius);
        glDrawElements(GL_TRIANGLES, deferred_index_count, GL_UNSIGNED_INT, (void *)(uintptr_t)0);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    GL_UseProgramFunc(0);
    GL_BindVertexArrayFunc(0);
}

static qboolean R_Deferred_CreateCompositeProgram(void)
{
    const char *vs_source =
        "#version 330 core\n"
        "layout(location = 0) in vec2 aPosition;\n"
        "layout(location = 1) in vec2 aTexCoord;\n"
        "out vec2 vTexCoord;\n"
        "void main() {\n"
        "    vTexCoord = aTexCoord;\n"
        "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "}\n";

    char *fs_source = (char *)COM_LoadMallocFile("shaders/deferred_composite.frag", NULL);
    if (!fs_source)
    {
        Con_Printf("Failed to load deferred composite shader\n");
        return false;
    }

    GLuint vs = R_Deferred_Compile(GL_VERTEX_SHADER, vs_source);
    GLuint fs = R_Deferred_Compile(GL_FRAGMENT_SHADER, fs_source);

    composite_program = GL_CreateProgramFunc();
    GL_AttachShaderFunc(composite_program, vs);
    GL_AttachShaderFunc(composite_program, fs);
    GL_LinkProgramFunc(composite_program);
    R_Deferred_LogShader(composite_program, true);

    GL_DeleteShaderFunc(vs);
    GL_DeleteShaderFunc(fs);
    Z_Free(fs_source);

    if (!composite_program)
        return false;

    GL_UseProgramFunc(composite_program);
    GL_Uniform1iFunc(GL_GetUniformLocationFunc(composite_program, "u_ForwardColor"), 0);
    GL_Uniform1iFunc(GL_GetUniformLocationFunc(composite_program, "u_DeferredLight"), 1);

    return true;
}

static void R_Deferred_CreateCompositeQuad(void)
{
    const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };

    GL_GenVertexArraysFunc(1, &composite_vao);
    GL_BindVertexArrayFunc(composite_vao);

    GL_GenBuffersFunc(1, &composite_vbo);
    GL_BindBufferFunc(GL_ARRAY_BUFFER, composite_vbo);
    GL_BufferDataFunc(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GL_EnableVertexAttribArrayFunc(0);
    GL_VertexAttribPointerFunc(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(uintptr_t)0);
    GL_EnableVertexAttribArrayFunc(1);
    GL_VertexAttribPointerFunc(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(uintptr_t)(2 * sizeof(float)));

    GL_BindVertexArrayFunc(0);
}

static qboolean R_Deferred_EnsureComposite(void)
{
    if (!composite_program)
    {
        if (!R_Deferred_CreateCompositeProgram())
            return false;
    }

    if (!composite_vao)
        R_Deferred_CreateCompositeQuad();

    return composite_program != 0 && composite_vao != 0;
}

void R_Deferred_Composite(void)
{
    if (!R_Deferred_EnsureComposite())
        return;

    GL_UseProgramFunc(composite_program);
    GL_BindVertexArrayFunc(composite_vao);

    const qboolean msaa = framebufs.scene.samples > 1;
    GLuint forward_color = msaa ? framebufs.resolved_scene.color_tex : framebufs.scene.color_tex;

    GL_ActiveTextureFunc(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, forward_color);
    GL_ActiveTextureFunc(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, framebufs.composite.color_tex);

    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_DEPTH_TEST);

    GL_BindVertexArrayFunc(0);
    GL_UseProgramFunc(0);
}
