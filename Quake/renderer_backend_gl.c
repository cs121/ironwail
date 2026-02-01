/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend_gl.h"

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

static void RB_GL_Init(void)
{
	Con_DPrintf("RB_GL_Init: stub backend initialized.\n");
}

static void RB_GL_Shutdown(void)
{
	Con_DPrintf("RB_GL_Shutdown: stub backend shutdown.\n");
}

static void RB_GL_BeginFrame(void)
{
}

static void RB_GL_EndFrame(void)
{
}

static void RB_GL_Resize(int width, int height)
{
	(void)width;
	(void)height;
}

static void RB_GL_NewMap(void)
{
}

static rb_tex_t RB_GL_CreateTexture(const rb_tex_desc_t *desc)
{
	(void)desc;
	return (rb_tex_t)Z_Malloc(sizeof(rb_gl_tex_t));
}

static void RB_GL_DestroyTexture(rb_tex_t tex)
{
	Z_Free(tex);
}

static rb_buf_t RB_GL_CreateBuffer(const rb_buf_desc_t *desc)
{
	(void)desc;
	return (rb_buf_t)Z_Malloc(sizeof(rb_gl_buf_t));
}

static void RB_GL_DestroyBuffer(rb_buf_t buf)
{
	Z_Free(buf);
}

static rb_fbo_t RB_GL_CreateFBO(const rb_fbo_desc_t *desc)
{
	(void)desc;
	return (rb_fbo_t)Z_Malloc(sizeof(rb_gl_fbo_t));
}

static void RB_GL_DestroyFBO(rb_fbo_t fbo)
{
	Z_Free(fbo);
}

static void RB_GL_Submit(const rb_draw_desc_t *desc)
{
	(void)desc;
}

static const rb_caps_t *RB_GL_GetCaps(void)
{
	return &rb_gl_caps;
}

static void RB_GL_DebugMarkerBegin(const char *label)
{
	if (label && *label)
		Con_DPrintf("RB_GL_DebugMarkerBegin: %s\n", label);
}

static void RB_GL_DebugMarkerEnd(void)
{
}

static const rb_backend_api_t rb_gl_api = {
	"OpenGL",
	RB_GL_Init,
	RB_GL_Shutdown,
	RB_GL_BeginFrame,
	RB_GL_EndFrame,
	RB_GL_Resize,
	RB_GL_NewMap,
	RB_GL_CreateTexture,
	RB_GL_DestroyTexture,
	RB_GL_CreateBuffer,
	RB_GL_DestroyBuffer,
	RB_GL_CreateFBO,
	RB_GL_DestroyFBO,
	RB_GL_Submit,
	RB_GL_GetCaps,
	RB_GL_DebugMarkerBegin,
	RB_GL_DebugMarkerEnd
};

const rb_backend_api_t *RB_GL_GetApi(void)
{
	return &rb_gl_api;
}
