/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend_null.h"

static rb_caps_t rb_null_caps = {0, 0};

static void RB_NULL_Init(void)
{
	Con_DPrintf("RB_NULL_Init: null backend initialized.\n");
}

static void RB_NULL_Shutdown(void)
{
	Con_DPrintf("RB_NULL_Shutdown: null backend shutdown.\n");
}

static void RB_NULL_BeginFrame(void)
{
}

static void RB_NULL_EndFrame(void)
{
}

static void RB_NULL_Resize(int width, int height)
{
	(void)width;
	(void)height;
}

static void RB_NULL_NewMap(void)
{
}

static rb_tex_t RB_NULL_CreateTexture(const rb_tex_desc_t *desc)
{
	(void)desc;
	return NULL;
}

static void RB_NULL_DestroyTexture(rb_tex_t tex)
{
	(void)tex;
}

static rb_buf_t RB_NULL_CreateBuffer(const rb_buf_desc_t *desc)
{
	(void)desc;
	return NULL;
}

static void RB_NULL_DestroyBuffer(rb_buf_t buf)
{
	(void)buf;
}

static rb_fbo_t RB_NULL_CreateFBO(const rb_fbo_desc_t *desc)
{
	(void)desc;
	return NULL;
}

static void RB_NULL_DestroyFBO(rb_fbo_t fbo)
{
	(void)fbo;
}

static void RB_NULL_Submit(const rb_draw_desc_t *desc)
{
	(void)desc;
}

static void RB_NULL_Enable(int cap)
{
	(void)cap;
}

static void RB_NULL_Disable(int cap)
{
	(void)cap;
}

static void RB_NULL_BlendFunc(int sfactor, int dfactor)
{
	(void)sfactor;
	(void)dfactor;
}

static void RB_NULL_CullFace(int mode)
{
	(void)mode;
}

static void RB_NULL_DepthMask(int flag)
{
	(void)flag;
}

static void RB_NULL_Viewport(int x, int y, int width, int height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void RB_NULL_Scissor(int x, int y, int width, int height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void RB_NULL_BindFramebuffer(int target, unsigned int framebuffer)
{
	(void)target;
	(void)framebuffer;
}

static void RB_NULL_BindTexture(int target, unsigned int texture)
{
	(void)target;
	(void)texture;
}

static void RB_NULL_GenBuffers(int n, unsigned int *buffers)
{
	(void)n;
	(void)buffers;
}

static void RB_NULL_DeleteBuffers(int n, const unsigned int *buffers)
{
	(void)n;
	(void)buffers;
}

static void RB_NULL_BindBuffer(int target, unsigned int buffer)
{
	(void)target;
	(void)buffer;
}

static void RB_NULL_BufferData(int target, size_t size, const void *data, int usage)
{
	(void)target;
	(void)size;
	(void)data;
	(void)usage;
}

static void *RB_NULL_BufferMapRange(int target, unsigned int buffer, size_t offset, size_t length, unsigned int flags, size_t full_size, const char *label)
{
	(void)target;
	(void)buffer;
	(void)offset;
	(void)length;
	(void)flags;
	(void)full_size;
	(void)label;
	return NULL;
}

static void RB_NULL_GenVertexArrays(int n, unsigned int *arrays)
{
	(void)n;
	(void)arrays;
}

static void RB_NULL_DeleteVertexArrays(int n, const unsigned int *arrays)
{
	(void)n;
	(void)arrays;
}

static void RB_NULL_BindVertexArray(unsigned int array)
{
	(void)array;
}

static const rb_caps_t *RB_NULL_GetCaps(void)
{
	return &rb_null_caps;
}

static void RB_NULL_DebugMarkerBegin(const char *label)
{
	(void)label;
}

static void RB_NULL_DebugMarkerEnd(void)
{
}

static const rb_backend_api_t rb_null_api = {
	"Null",
	RB_NULL_Init,
	RB_NULL_Shutdown,
	RB_NULL_BeginFrame,
	RB_NULL_EndFrame,
	RB_NULL_Resize,
	RB_NULL_NewMap,
	RB_NULL_CreateTexture,
	RB_NULL_DestroyTexture,
	RB_NULL_CreateBuffer,
	RB_NULL_DestroyBuffer,
	RB_NULL_CreateFBO,
	RB_NULL_DestroyFBO,
	RB_NULL_Submit,
	RB_NULL_Enable,
	RB_NULL_Disable,
	RB_NULL_BlendFunc,
	RB_NULL_CullFace,
	RB_NULL_DepthMask,
	RB_NULL_Viewport,
	RB_NULL_Scissor,
	RB_NULL_BindFramebuffer,
	RB_NULL_BindTexture,
	RB_NULL_GenBuffers,
	RB_NULL_DeleteBuffers,
	RB_NULL_BindBuffer,
	RB_NULL_BufferData,
	RB_NULL_BufferMapRange,
	RB_NULL_GenVertexArrays,
	RB_NULL_DeleteVertexArrays,
	RB_NULL_BindVertexArray,
	RB_NULL_GetCaps,
	RB_NULL_DebugMarkerBegin,
	RB_NULL_DebugMarkerEnd
};

const rb_backend_api_t *RB_NULL_GetApi(void)
{
	return &rb_null_api;
}
