/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#ifndef RENDERER_BACKEND_GL_H
#define RENDERER_BACKEND_GL_H

#include "renderer_backend.h"

const rb_backend_api_t *RB_GL_GetApi(void);
void RB_GL_Enable(int cap);
void RB_GL_Disable(int cap);
void RB_GL_BlendFunc(int sfactor, int dfactor);
void RB_GL_CullFace(int mode);
void RB_GL_DepthMask(int flag);
void RB_GL_Viewport(int x, int y, int width, int height);
void RB_GL_Scissor(int x, int y, int width, int height);
void RB_GL_GenBuffers(int n, unsigned int *buffers);
void RB_GL_DeleteBuffers(int n, const unsigned int *buffers);
void RB_GL_BindBuffer(int target, unsigned int buffer);
void RB_GL_BufferData(int target, size_t size, const void *data, int usage);
void RB_GL_GenVertexArrays(int n, unsigned int *arrays);
void RB_GL_DeleteVertexArrays(int n, const unsigned int *arrays);
void RB_GL_BindVertexArray(unsigned int array);

#endif
