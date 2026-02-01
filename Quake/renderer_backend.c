/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend.h"
#include "renderer_backend_gl.h"
#include "renderer_backend_null.h"

static const rb_backend_api_t *rb_backend = NULL;
static rb_caps_t rb_caps_fallback = {0, 0};

static const rb_backend_api_t *RB_SelectBackend(void)
{
#if RENDERER_BACKEND_GL
	return RB_GL_GetApi();
#else
	return RB_NULL_GetApi();
#endif
}

void RB_Init(void)
{
	if (rb_backend)
		return;

	rb_backend = RB_SelectBackend();
	if (!rb_backend)
	{
		Con_Printf("RB_Init: no backend available.\n");
		return;
	}
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
	if (!rb_backend || !rb_backend->create_texture)
		return NULL;
	return rb_backend->create_texture(desc);
}

void RB_DestroyTexture(rb_tex_t tex)
{
	if (!rb_backend || !rb_backend->destroy_texture)
		return;
	rb_backend->destroy_texture(tex);
}

rb_buf_t RB_CreateBuffer(const rb_buf_desc_t *desc)
{
	if (!rb_backend || !rb_backend->create_buffer)
		return NULL;
	return rb_backend->create_buffer(desc);
}

void RB_DestroyBuffer(rb_buf_t buf)
{
	if (!rb_backend || !rb_backend->destroy_buffer)
		return;
	rb_backend->destroy_buffer(buf);
}

rb_fbo_t RB_CreateFBO(const rb_fbo_desc_t *desc)
{
	if (!rb_backend || !rb_backend->create_fbo)
		return NULL;
	return rb_backend->create_fbo(desc);
}

void RB_DestroyFBO(rb_fbo_t fbo)
{
	if (!rb_backend || !rb_backend->destroy_fbo)
		return;
	rb_backend->destroy_fbo(fbo);
}

void RB_Submit(const rb_draw_desc_t *desc)
{
	if (!rb_backend || !rb_backend->submit)
		return;
	rb_backend->submit(desc);
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
