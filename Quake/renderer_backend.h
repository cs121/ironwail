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
const rb_caps_t	*RB_GetCaps(void);
void			RB_DebugMarkerBegin(const char *label);
void			RB_DebugMarkerEnd(void);

#ifdef __cplusplus
}
#endif

#endif
