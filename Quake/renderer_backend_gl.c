/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend_gl.h"

#include <assert.h>

typedef struct rb_gl_tex_s
{
	int unused;
} rb_gl_tex_t;

typedef struct rb_gl_buf_s
{
	int unused;
} rb_gl_buf_t;

typedef struct rb_gl_fbo_s
{
	int unused;
} rb_gl_fbo_t;

static rb_caps_t rb_gl_caps = {0, 0};
static cvar_t r_backend_gl_errors = {"r_backend_gl_errors", "0", CVAR_NONE};

static void RBGL_CheckError(const char *label)
{
	unsigned int err;

	if (!r_backend_gl_errors.value)
		return;

	err = glGetError();
	if (err != GL_NO_ERROR)
		Con_DPrintf("RBGL_CheckError: %s (0x%x)\n", label, err);
}

static void RBGL_Enable(int cap)
{
	glEnable (cap);
}

static void RBGL_Disable(int cap)
{
	glDisable (cap);
}

static void RBGL_BlendFunc(int sfactor, int dfactor)
{
	glBlendFunc (sfactor, dfactor);
}

static void RBGL_CullFace(int mode)
{
	glCullFace (mode);
}

static void RBGL_DepthMask(int flag)
{
	glDepthMask (flag);
}

static void RBGL_Viewport(int x, int y, int width, int height)
{
	glViewport (x, y, width, height);
}

static void RBGL_Scissor(int x, int y, int width, int height)
{
	glScissor (x, y, width, height);
}

static void RBGL_GenBuffers(int n, unsigned int *buffers)
{
	GL_GenBuffersFunc (n, buffers);
}

static void RBGL_DeleteBuffers(int n, const unsigned int *buffers)
{
	GL_DeleteBuffersFunc (n, buffers);
}

static void RBGL_BindBuffer(int target, unsigned int buffer)
{
	GL_BindBufferFunc (target, buffer);
}

static void RBGL_BufferData(int target, size_t size, const void *data, int usage)
{
	GL_BufferDataFunc (target, size, data, usage);
}

static void RBGL_GenVertexArrays(int n, unsigned int *arrays)
{
	GL_GenVertexArraysFunc (n, arrays);
}

static void RBGL_DeleteVertexArrays(int n, const unsigned int *arrays)
{
	GL_DeleteVertexArraysFunc (n, arrays);
}

static void RBGL_BindVertexArray(unsigned int array)
{
	GL_BindVertexArrayFunc (array);
}

static void RBGL_Init(void)
{
	Cvar_RegisterVariable(&r_backend_gl_errors);
	Con_DPrintf("RBGL_Init: backend initialized.\n");
}

static void RBGL_Shutdown(void)
{
	Con_DPrintf("RBGL_Shutdown: backend shutdown.\n");
}

static void RBGL_BeginFrame(void)
{
	RBGL_CheckError("begin_frame");
}

static void RBGL_EndFrame(void)
{
	RBGL_CheckError("end_frame");
}

static void RBGL_Resize(int width, int height)
{
	(void)width;
	(void)height;
}

static void RBGL_NewMap(void)
{
}

static rb_tex_t RBGL_CreateTexture(const rb_tex_desc_t *desc)
{
	(void)desc;
	return (rb_tex_t)Z_Malloc(sizeof(rb_gl_tex_t));
}

static void RBGL_DestroyTexture(rb_tex_t tex)
{
	assert(tex);
	Z_Free(tex);
}

static rb_buf_t RBGL_CreateBuffer(const rb_buf_desc_t *desc)
{
	(void)desc;
	return (rb_buf_t)Z_Malloc(sizeof(rb_gl_buf_t));
}

static void RBGL_DestroyBuffer(rb_buf_t buf)
{
	assert(buf);
	Z_Free(buf);
}

static rb_fbo_t RBGL_CreateFBO(const rb_fbo_desc_t *desc)
{
	(void)desc;
	return (rb_fbo_t)Z_Malloc(sizeof(rb_gl_fbo_t));
}

static void RBGL_DestroyFBO(rb_fbo_t fbo)
{
	assert(fbo);
	Z_Free(fbo);
}

static void RBGL_Submit(const rb_draw_desc_t *desc)
{
	(void)desc;
}

static const rb_caps_t *RBGL_GetCaps(void)
{
	return &rb_gl_caps;
}

static void RBGL_DebugMarkerBegin(const char *label)
{
	if (label && *label)
		Con_DPrintf("RBGL_DebugMarkerBegin: %s\n", label);
}

static void RBGL_DebugMarkerEnd(void)
{
}

static const rb_backend_api_t rb_gl_api = {
	"OpenGL",
	RBGL_Init,
	RBGL_Shutdown,
	RBGL_BeginFrame,
	RBGL_EndFrame,
	RBGL_Resize,
	RBGL_NewMap,
	RBGL_CreateTexture,
	RBGL_DestroyTexture,
	RBGL_CreateBuffer,
	RBGL_DestroyBuffer,
	RBGL_CreateFBO,
	RBGL_DestroyFBO,
	RBGL_Submit,
	RBGL_Enable,
	RBGL_Disable,
	RBGL_BlendFunc,
	RBGL_CullFace,
	RBGL_DepthMask,
	RBGL_Viewport,
	RBGL_Scissor,
	RBGL_GenBuffers,
	RBGL_DeleteBuffers,
	RBGL_BindBuffer,
	RBGL_BufferData,
	RBGL_GenVertexArrays,
	RBGL_DeleteVertexArrays,
	RBGL_BindVertexArray,
	RBGL_GetCaps,
	RBGL_DebugMarkerBegin,
	RBGL_DebugMarkerEnd
};

const rb_backend_api_t *RBGL_GetApi(void)
{
	return &rb_gl_api;
}
