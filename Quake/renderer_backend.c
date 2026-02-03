/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend.h"
#include "renderer_backend_gl.h"
#include "renderer_backend_null.h"

#include <assert.h>

static const rb_backend_api_t *rb_backend = NULL;
static rb_caps_t rb_caps_fallback = {0, 0};
static cvar_t r_backend = {"r_backend", "gl", CVAR_ARCHIVE};
static qboolean rb_cvars_registered = false;

static const rb_backend_api_t *RB_SelectBackend(void)
{
#if RENDERER_BACKEND_GL
	if (!q_strcasecmp(r_backend.string, "null"))
		return RB_NULL_GetApi();
	return RBGL_GetApi();
#else
	return RB_NULL_GetApi();
#endif
}

void RB_Init(void)
{
	if (rb_backend)
		return;

	if (!rb_cvars_registered)
	{
		Cvar_RegisterVariable(&r_backend);
		rb_cvars_registered = true;
	}

	if (q_strcasecmp(r_backend.string, "gl") && q_strcasecmp(r_backend.string, "null"))
		Con_Printf("RB_Init: unknown backend '%s', using default.\n", r_backend.string);

	rb_backend = RB_SelectBackend();
	if (!rb_backend)
	{
		Con_Printf("RB_Init: no backend available.\n");
		return;
	}
	if (rb_backend == RB_NULL_GetApi() && q_strcasecmp(r_backend.string, "null"))
		Con_Printf("RB_Init: backend '%s' unavailable, using null backend.\n", r_backend.string);
	if (rb_backend->init)
		rb_backend->init();
}

void RB_Shutdown(void)
{
	if (!rb_backend)
		return;
	if (rb_backend->shutdown)
		rb_backend->shutdown();
	rb_backend = NULL;
}

void RB_BeginFrame(void)
{
	if (!rb_backend)
		return;
	if (rb_backend->begin_frame)
		rb_backend->begin_frame();
}

void RB_EndFrame(void)
{
	if (!rb_backend)
		return;
	if (rb_backend->end_frame)
		rb_backend->end_frame();
}

void RB_Resize(int width, int height)
{
	if (!rb_backend)
		return;
	if (rb_backend->resize)
		rb_backend->resize(width, height);
}

void RB_NewMap(void)
{
	if (!rb_backend)
		return;
	if (rb_backend->new_map)
		rb_backend->new_map();
}

rb_tex_t RB_CreateTexture(const rb_tex_desc_t *desc)
{
	assert(desc);
	if (!rb_backend || !rb_backend->create_texture)
		return NULL;
	return rb_backend->create_texture(desc);
}

void RB_DestroyTexture(rb_tex_t tex)
{
	if (!rb_backend || !rb_backend->destroy_texture)
		return;
	if (!tex)
		return;
	rb_backend->destroy_texture(tex);
}

rb_buf_t RB_CreateBuffer(const rb_buf_desc_t *desc)
{
	assert(desc);
	if (!rb_backend || !rb_backend->create_buffer)
		return NULL;
	return rb_backend->create_buffer(desc);
}

void RB_DestroyBuffer(rb_buf_t buf)
{
	if (!rb_backend || !rb_backend->destroy_buffer)
		return;
	if (!buf)
		return;
	rb_backend->destroy_buffer(buf);
}

rb_fbo_t RB_CreateFBO(const rb_fbo_desc_t *desc)
{
	assert(desc);
	if (!rb_backend || !rb_backend->create_fbo)
		return NULL;
	return rb_backend->create_fbo(desc);
}

void RB_DestroyFBO(rb_fbo_t fbo)
{
	if (!rb_backend || !rb_backend->destroy_fbo)
		return;
	if (!fbo)
		return;
	rb_backend->destroy_fbo(fbo);
}

void RB_Submit(const rb_draw_desc_t *desc)
{
	assert(desc);
	if (!rb_backend || !rb_backend->submit)
		return;
	rb_backend->submit(desc);
}

void RB_Enable(int cap)
{
	if (!rb_backend || !rb_backend->enable)
		return;
	rb_backend->enable(cap);
}

void RB_Disable(int cap)
{
	if (!rb_backend || !rb_backend->disable)
		return;
	rb_backend->disable(cap);
}

void RB_BlendFunc(int sfactor, int dfactor)
{
	if (!rb_backend || !rb_backend->blend_func)
		return;
	rb_backend->blend_func(sfactor, dfactor);
}

void RB_CullFace(int mode)
{
	if (!rb_backend || !rb_backend->cull_face)
		return;
	rb_backend->cull_face(mode);
}

void RB_DepthMask(int flag)
{
	if (!rb_backend || !rb_backend->depth_mask)
		return;
	rb_backend->depth_mask(flag);
}

void RB_Viewport(int x, int y, int width, int height)
{
	if (!rb_backend || !rb_backend->viewport)
		return;
	rb_backend->viewport(x, y, width, height);
}

void RB_Scissor(int x, int y, int width, int height)
{
	if (!rb_backend || !rb_backend->scissor)
		return;
	rb_backend->scissor(x, y, width, height);
}

void RB_BindFramebuffer(int target, unsigned int framebuffer)
{
	if (!rb_backend || !rb_backend->bind_framebuffer)
		return;
	rb_backend->bind_framebuffer(target, framebuffer);
}

void RB_BindTexture(int target, unsigned int texture)
{
	if (!rb_backend || !rb_backend->bind_texture)
		return;
	rb_backend->bind_texture(target, texture);
}

void RB_GenBuffers(int n, unsigned int *buffers)
{
	if (!rb_backend || !rb_backend->gen_buffers)
		return;
	rb_backend->gen_buffers(n, buffers);
}

void RB_DeleteBuffers(int n, const unsigned int *buffers)
{
	if (!rb_backend || !rb_backend->delete_buffers)
		return;
	rb_backend->delete_buffers(n, buffers);
}

void RB_BindBuffer(int target, unsigned int buffer)
{
	if (!rb_backend || !rb_backend->bind_buffer)
		return;
	rb_backend->bind_buffer(target, buffer);
}

void RB_BufferData(int target, size_t size, const void *data, int usage)
{
	if (!rb_backend || !rb_backend->buffer_data)
		return;
	rb_backend->buffer_data(target, size, data, usage);
}

void RB_GenVertexArrays(int n, unsigned int *arrays)
{
	if (!rb_backend || !rb_backend->gen_vertex_arrays)
		return;
	rb_backend->gen_vertex_arrays(n, arrays);
}

void RB_DeleteVertexArrays(int n, const unsigned int *arrays)
{
	if (!rb_backend || !rb_backend->delete_vertex_arrays)
		return;
	rb_backend->delete_vertex_arrays(n, arrays);
}

void RB_BindVertexArray(unsigned int array)
{
	if (!rb_backend || !rb_backend->bind_vertex_array)
		return;
	rb_backend->bind_vertex_array(array);
}

const rb_caps_t *RB_GetCaps(void)
{
	if (!rb_backend || !rb_backend->get_caps)
		return &rb_caps_fallback;
	return rb_backend->get_caps();
}

void RB_DebugMarkerBegin(const char *label)
{
	if (!rb_backend || !rb_backend->debug_marker_begin)
		return;
	rb_backend->debug_marker_begin(label);
}

void RB_DebugMarkerEnd(void)
{
	if (!rb_backend || !rb_backend->debug_marker_end)
		return;
	rb_backend->debug_marker_end();
}
