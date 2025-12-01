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

#include "quakedef.h"
#include "gl_gbuffer.h"

static GLuint gbuffer_fbo = 0;
static GLuint gbuffer_albedo = 0;
static GLuint gbuffer_normal_spec = 0;
static GLuint gbuffer_depth = 0;
static GLuint forward_fbo = 0;

static GLuint R_GBuffer_CreateTexture(int width, int height, GLenum internal_format, GLenum format, GLenum type)
{
    GLuint texture = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return texture;
}

void R_GBuffer_Shutdown(void)
{
    if (gbuffer_albedo)
    {
        glDeleteTextures(1, &gbuffer_albedo);
        gbuffer_albedo = 0;
    }

    if (gbuffer_normal_spec)
    {
        glDeleteTextures(1, &gbuffer_normal_spec);
        gbuffer_normal_spec = 0;
    }

    if (gbuffer_depth)
    {
        glDeleteTextures(1, &gbuffer_depth);
        gbuffer_depth = 0;
    }

    if (gbuffer_fbo)
    {
        glDeleteFramebuffers(1, &gbuffer_fbo);
        gbuffer_fbo = 0;
    }
}

void R_GBuffer_Init(int width, int height)
{
    GLenum status;
    GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };

    R_GBuffer_Shutdown();

    gbuffer_albedo = R_GBuffer_CreateTexture(width, height, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);
    gbuffer_normal_spec = R_GBuffer_CreateTexture(width, height, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    gbuffer_depth = R_GBuffer_CreateTexture(width, height, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);

    GL_GenFramebuffersFunc(1, &gbuffer_fbo);
    GL_BindFramebufferFunc(GL_FRAMEBUFFER, gbuffer_fbo);

    GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gbuffer_albedo, 0);
    GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbuffer_normal_spec, 0);
    GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gbuffer_depth, 0);

    GL_DrawBuffersFunc(2, draw_buffers);

    status = GL_CheckFramebufferStatusFunc(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        Sys_Error("R_GBuffer_Init: framebuffer incomplete (0x%X)", status);

    GL_BindFramebufferFunc(GL_FRAMEBUFFER, 0);
}

void R_GBuffer_Begin(void)
{
    GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&forward_fbo);

    GL_BindFramebufferFunc(GL_FRAMEBUFFER, gbuffer_fbo);
    GL_DrawBuffersFunc(2, draw_buffers);

    glEnable(GL_DEPTH_TEST);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void R_GBuffer_End(void)
{
    GL_BindFramebufferFunc(GL_FRAMEBUFFER, forward_fbo);

    if (forward_fbo)
    {
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    else
    {
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
    }
}

GLuint R_GBuffer_GetAlbedoTexture(void)
{
    return gbuffer_albedo;
}

GLuint R_GBuffer_GetNormalSpecTexture(void)
{
    return gbuffer_normal_spec;
}

GLuint R_GBuffer_GetDepthTexture(void)
{
    return gbuffer_depth;
}
