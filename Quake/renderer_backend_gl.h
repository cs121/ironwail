/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#ifndef RENDERER_BACKEND_GL_H
#define RENDERER_BACKEND_GL_H

#include "renderer_backend.h"

const rb_backend_api_t *RBGL_GetApi(void);
void *RB_GL_MapBufferRange(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield flags, GLsizeiptr expected_total_size);
void RB_GL_UnmapBuffer(GLenum target, GLuint buffer, const char *tag);

#endif
