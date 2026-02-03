/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend_gl.h"

#include <assert.h>

typedef struct rb_gl_tex_s
{
	GLuint id;
	GLenum target;
	int width;
	int height;
	int mip_levels;
	rb_tex_format_t format;
} rb_gl_tex_t;

typedef struct rb_gl_buf_s
{
	GLuint id;
	GLenum target;
	size_t size;
	rb_buffer_type_t type;
	rb_buffer_usage_t usage;
} rb_gl_buf_t;

typedef struct rb_gl_fbo_s
{
	GLuint id;
	rb_tex_t color;
	rb_tex_t depth_stencil;
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

static void RBGL_BindFramebuffer(int target, unsigned int framebuffer)
{
	GL_BindFramebufferFunc(target, framebuffer);
}

static void RBGL_BindTexture(int target, unsigned int texture)
{
	glBindTexture(target, texture);
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

static GLenum RBGL_TextureInternalFormat(rb_tex_format_t format)
{
	switch (format)
	{
	case RB_TEXFMT_RGBA8:
		return GL_RGBA8;
	case RB_TEXFMT_DEPTH24_STENCIL8:
		return GL_DEPTH24_STENCIL8;
	}
	return GL_RGBA8;
}

static GLenum RBGL_TextureFormat(rb_tex_format_t format)
{
	switch (format)
	{
	case RB_TEXFMT_RGBA8:
		return GL_RGBA;
	case RB_TEXFMT_DEPTH24_STENCIL8:
		return GL_DEPTH_STENCIL;
	}
	return GL_RGBA;
}

static GLenum RBGL_TextureType(rb_tex_format_t format)
{
	switch (format)
	{
	case RB_TEXFMT_RGBA8:
		return GL_UNSIGNED_BYTE;
	case RB_TEXFMT_DEPTH24_STENCIL8:
		return GL_UNSIGNED_INT_24_8;
	}
	return GL_UNSIGNED_BYTE;
}

static GLenum RBGL_BufferTarget(rb_buffer_type_t type)
{
	switch (type)
	{
	case RB_BUFFER_VERTEX:
		return GL_ARRAY_BUFFER;
	case RB_BUFFER_INDEX:
		return GL_ELEMENT_ARRAY_BUFFER;
	case RB_BUFFER_UNIFORM:
		return GL_UNIFORM_BUFFER;
	}
	return GL_ARRAY_BUFFER;
}

static GLenum RBGL_BufferUsage(rb_buffer_usage_t usage)
{
	switch (usage)
	{
	case RB_BUFFER_USAGE_STATIC:
		return GL_STATIC_DRAW;
	case RB_BUFFER_USAGE_DYNAMIC:
		return GL_DYNAMIC_DRAW;
	}
	return GL_STATIC_DRAW;
}

static rb_tex_t RBGL_CreateTexture(const rb_tex_desc_t *desc)
{
	rb_gl_tex_t *tex;
	GLenum internal_format;
	GLenum format;
	GLenum type;
	GLenum filter;
	int levels;

	assert(desc);
	tex = (rb_gl_tex_t *)Z_Malloc(sizeof(*tex));
	tex->target = GL_TEXTURE_2D;
	tex->width = desc->width;
	tex->height = desc->height;
	tex->mip_levels = desc->mip_levels > 0 ? desc->mip_levels : 1;
	tex->format = desc->format;

	internal_format = RBGL_TextureInternalFormat(desc->format);
	format = RBGL_TextureFormat(desc->format);
	type = RBGL_TextureType(desc->format);
	filter = desc->format == RB_TEXFMT_DEPTH24_STENCIL8 ? GL_NEAREST : GL_LINEAR;
	levels = tex->mip_levels - 1;

	glGenTextures(1, &tex->id);
	GL_BindNative(GL_TEXTURE0, tex->target, tex->id);
	glTexImage2D(tex->target, 0, internal_format, desc->width, desc->height, 0, format, type, desc->data);
	glTexParameteri(tex->target, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(tex->target, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(tex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex->target, GL_TEXTURE_MAX_LEVEL, levels);
	if (desc->format == RB_TEXFMT_DEPTH24_STENCIL8)
		glTexParameteri(tex->target, GL_TEXTURE_COMPARE_MODE, GL_NONE);

	return (rb_tex_t)tex;
}

static void RBGL_DestroyTexture(rb_tex_t tex)
{
	rb_gl_tex_t *gl_tex = (rb_gl_tex_t *)tex;

	assert(gl_tex);
	if (gl_tex->id)
		GL_DeleteNativeTexture(gl_tex->id);
	Z_Free(gl_tex);
}

static rb_buf_t RBGL_CreateBuffer(const rb_buf_desc_t *desc)
{
	rb_gl_buf_t *buf;
	GLenum target;
	GLenum usage;
	size_t size;

	assert(desc);
	buf = (rb_gl_buf_t *)Z_Malloc(sizeof(*buf));
	buf->type = desc->type;
	buf->usage = desc->usage;
	target = RBGL_BufferTarget(desc->type);
	usage = RBGL_BufferUsage(desc->usage);
	size = desc->size ? desc->size : desc->data_size;

	buf->target = target;
	buf->size = size;

	GL_GenBuffersFunc(1, &buf->id);
	GL_BindBufferFunc(target, buf->id);
	GL_BufferDataFunc(target, size, desc->data, usage);

	return (rb_buf_t)buf;
}

static void RBGL_DestroyBuffer(rb_buf_t buf)
{
	rb_gl_buf_t *gl_buf = (rb_gl_buf_t *)buf;

	assert(gl_buf);
	if (gl_buf->id)
		GL_DeleteBuffersFunc(1, &gl_buf->id);
	Z_Free(gl_buf);
}

static rb_fbo_t RBGL_CreateFBO(const rb_fbo_desc_t *desc)
{
	rb_gl_fbo_t *fbo;
	GLenum draw_buffers[1];

	assert(desc);
	fbo = (rb_gl_fbo_t *)Z_Malloc(sizeof(*fbo));
	fbo->color = desc->color;
	fbo->depth_stencil = desc->depth_stencil;

	GL_GenFramebuffersFunc(1, &fbo->id);
	GL_BindFramebufferFunc(GL_FRAMEBUFFER, fbo->id);

	if (desc->color)
	{
		const rb_gl_tex_t *color_tex = (const rb_gl_tex_t *)desc->color;
		GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color_tex->target, color_tex->id, 0);
		draw_buffers[0] = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, draw_buffers);
	}
	else
	{
		glDrawBuffer(GL_NONE);
		glReadBuffer(GL_NONE);
	}

	if (desc->depth_stencil)
	{
		const rb_gl_tex_t *depth_tex = (const rb_gl_tex_t *)desc->depth_stencil;
		if (depth_tex->format == RB_TEXFMT_DEPTH24_STENCIL8)
			GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth_tex->target, depth_tex->id, 0);
		else
			GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depth_tex->target, depth_tex->id, 0);
	}

	GL_BindFramebufferFunc(GL_FRAMEBUFFER, 0);

	return (rb_fbo_t)fbo;
}

static void RBGL_DestroyFBO(rb_fbo_t fbo)
{
	rb_gl_fbo_t *gl_fbo = (rb_gl_fbo_t *)fbo;

	assert(gl_fbo);
	if (gl_fbo->id)
		GL_DeleteFramebuffersFunc(1, &gl_fbo->id);
	Z_Free(gl_fbo);
}

static void RBGL_Submit(const rb_draw_desc_t *desc)
{
	const rb_gl_buf_t *vertex_buf;
	const rb_gl_buf_t *index_buf;
	GLsizei index_count;
	GLsizei vertex_count;

	assert(desc);
	vertex_buf = (const rb_gl_buf_t *)desc->vertex_buffer;
	index_buf = (const rb_gl_buf_t *)desc->index_buffer;
	index_count = desc->index_count;
	vertex_count = desc->vertex_count;

	if (vertex_buf)
		GL_BindBufferFunc(GL_ARRAY_BUFFER, vertex_buf->id);
	if (index_buf)
		GL_BindBufferFunc(GL_ELEMENT_ARRAY_BUFFER, index_buf->id);

	if (index_buf && index_count > 0)
	{
		const size_t index_offset = (size_t)desc->first_index * sizeof(unsigned short);
		glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, (const void *)index_offset);
	}
	else if (vertex_count > 0)
	{
		glDrawArrays(GL_TRIANGLES, desc->first_vertex, vertex_count);
	}
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
	RBGL_BindFramebuffer,
	RBGL_BindTexture,
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
