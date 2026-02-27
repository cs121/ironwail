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
extern cvar_t r_backend_ui;
extern cvar_t r_backend_postfx;
extern cvar_t r_backend_particles;
extern cvar_t r_backend_alias;
extern cvar_t r_backend_world;
extern cvar_t r_backend_fogvol;
extern cvar_t r_backend_framehash_debug;
extern cvar_t r_backend_framehash_scene;
extern cvar_t r_backend_framehash_epsilon;

typedef qboolean (*rbackend_block_fn_t) (void);

const render_backend_vtable_t *RBackend_GetVTable (void);
void RBackend_DispatchRenderView (void (*legacy_fn)(void));
void RBackend_DispatchUpdateScreen (void (*legacy_fn)(void));
void RBackend_DispatchBlock (const char *block_name, cvar_t *toggle, qboolean backend_path_available,
	rbackend_block_fn_t backend_fn, void (*legacy_fn)(void), qboolean *warned_once);
void RBackend_DebugCaptureEndFrameHash (void);

#endif
