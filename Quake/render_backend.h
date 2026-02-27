#ifndef RENDER_BACKEND_H
#define RENDER_BACKEND_H

#include "quakedef.h"
#include "rb_pass.h"

typedef struct render_backend_vtable_s
{
	void (*BeginFrame) (const char *label);
	void (*EndFrame) (void);
	void (*BeginPass) (rb_pass_t pass);
	void (*EndPass) (void);
	void (*DrawArrays) (GLenum mode, GLint first, GLsizei count);
	void (*DrawElements) (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
	void (*DispatchCompute) (GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
	void (*BindFramebuffer) (GLenum target, GLuint framebuffer);
	void (*BindTexture) (GLenum texunit, GLenum target, GLuint texture);
	void (*BindBuffer) (GLenum target, GLuint buffer);
} render_backend_vtable_t;

extern cvar_t r_backend;

const render_backend_vtable_t *RBackend_GetVTable (void);
void RBackend_DispatchRenderView (void (*legacy_fn)(void));
void RBackend_DispatchUpdateScreen (void (*legacy_fn)(void));

#endif
