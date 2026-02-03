/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#ifndef RENDERER_BACKEND_H
#define RENDERER_BACKEND_H

#include "q_stdinc.h"

#include <stddef.h>

#ifndef RENDERER_BACKEND_GL
#define RENDERER_BACKEND_GL 1
#endif

/*
Renderer backend responsibilities:
- Owns graphics API calls, resource lifetimes, and backend state tracking/caching.
- Exposes a minimal RB_* interface for renderer orchestration to consume.
- Does NOT contain render graph logic, pass sequencing, or high-level effects.

Migration Notes:
- RenderView orchestration, passes, and effects like SSAO/postfx remain in the frontend.
- Backend should stay focused on API calls, resource management, and state control only.
*/

typedef struct rb_tex_s *rb_tex_t;
typedef struct rb_buf_s *rb_buf_t;
typedef struct rb_fbo_s *rb_fbo_t;

typedef enum rb_tex_format_e
{
	RB_TEXFMT_RGBA8 = 0,
	RB_TEXFMT_DEPTH24_STENCIL8
} rb_tex_format_t;

typedef enum rb_buffer_type_e
{
	RB_BUFFER_VERTEX = 0,
	RB_BUFFER_INDEX,
	RB_BUFFER_UNIFORM
} rb_buffer_type_t;

typedef enum rb_buffer_usage_e
{
	RB_BUFFER_USAGE_STATIC = 0,
	RB_BUFFER_USAGE_DYNAMIC
} rb_buffer_usage_t;

typedef struct rb_tex_desc_s
{
	int				width;
	int				height;
	int				mip_levels;
	rb_tex_format_t	format;
	const void		*data;
	size_t			data_size;
} rb_tex_desc_t;

typedef struct rb_buf_desc_s
{
	rb_buffer_type_t	type;
	rb_buffer_usage_t	usage;
	size_t				size;
	const void			*data;
	size_t				data_size;
} rb_buf_desc_t;

typedef struct rb_fbo_desc_s
{
	rb_tex_t			color;
	rb_tex_t			depth_stencil;
} rb_fbo_desc_t;

typedef struct rb_draw_desc_s
{
	rb_buf_t			vertex_buffer;
	rb_buf_t			index_buffer;
	int				first_vertex;
	int				vertex_count;
	int				first_index;
	int				index_count;
} rb_draw_desc_t;

typedef struct rb_caps_s
{
	int				supports_debug_markers;
	int				supports_instancing;
} rb_caps_t;

typedef struct rb_backend_api_s
{
	const char			*name;
	void				(*init)(void);
	void				(*shutdown)(void);
	void				(*begin_frame)(void);
	void				(*end_frame)(void);
	void				(*resize)(int width, int height);
	void				(*new_map)(void);
	rb_tex_t			(*create_texture)(const rb_tex_desc_t *desc);
	void				(*destroy_texture)(rb_tex_t tex);
	rb_buf_t			(*create_buffer)(const rb_buf_desc_t *desc);
	void				(*destroy_buffer)(rb_buf_t buf);
	rb_fbo_t			(*create_fbo)(const rb_fbo_desc_t *desc);
	void				(*destroy_fbo)(rb_fbo_t fbo);
	void				(*submit)(const rb_draw_desc_t *desc);
	void				(*enable)(int cap);
	void				(*disable)(int cap);
	void				(*blend_func)(int sfactor, int dfactor);
	void				(*cull_face)(int mode);
	void				(*depth_mask)(int flag);
	void				(*viewport)(int x, int y, int width, int height);
	void				(*scissor)(int x, int y, int width, int height);
	void				(*gen_buffers)(int n, unsigned int *buffers);
	void				(*delete_buffers)(int n, const unsigned int *buffers);
	void				(*bind_buffer)(int target, unsigned int buffer);
	void				(*buffer_data)(int target, size_t size, const void *data, int usage);
	void				(*gen_vertex_arrays)(int n, unsigned int *arrays);
	void				(*delete_vertex_arrays)(int n, const unsigned int *arrays);
	void				(*bind_vertex_array)(unsigned int array);
	const rb_caps_t	*(*get_caps)(void);
	void				(*debug_marker_begin)(const char *label);
	void				(*debug_marker_end)(void);
} rb_backend_api_t;

#ifdef __cplusplus
extern "C" {
#endif

void			RB_Init(void);
void			RB_Shutdown(void);
void			RB_BeginFrame(void);
void			RB_EndFrame(void);
void			RB_Resize(int width, int height);
void			RB_NewMap(void);

rb_tex_t		RB_CreateTexture(const rb_tex_desc_t *desc);
void			RB_DestroyTexture(rb_tex_t tex);
rb_buf_t		RB_CreateBuffer(const rb_buf_desc_t *desc);
void			RB_DestroyBuffer(rb_buf_t buf);
rb_fbo_t		RB_CreateFBO(const rb_fbo_desc_t *desc);
void			RB_DestroyFBO(rb_fbo_t fbo);

void			RB_Submit(const rb_draw_desc_t *desc);
void			RB_Enable(int cap);
void			RB_Disable(int cap);
void			RB_BlendFunc(int sfactor, int dfactor);
void			RB_CullFace(int mode);
void			RB_DepthMask(int flag);
void			RB_Viewport(int x, int y, int width, int height);
void			RB_Scissor(int x, int y, int width, int height);
void			RB_GenBuffers(int n, unsigned int *buffers);
void			RB_DeleteBuffers(int n, const unsigned int *buffers);
void			RB_BindBuffer(int target, unsigned int buffer);
void			RB_BufferData(int target, size_t size, const void *data, int usage);
void			RB_GenVertexArrays(int n, unsigned int *arrays);
void			RB_DeleteVertexArrays(int n, const unsigned int *arrays);
void			RB_BindVertexArray(unsigned int array);
const rb_caps_t	*RB_GetCaps(void);
void			RB_DebugMarkerBegin(const char *label);
void			RB_DebugMarkerEnd(void);

#ifdef __cplusplus
}
#endif

#endif
