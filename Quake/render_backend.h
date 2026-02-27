#ifndef RENDER_BACKEND_H
#define RENDER_BACKEND_H

#include "quakedef.h"
#include "rb_pass.h"

typedef struct render_backend_vtable_s
{
	/*
	 * A2 render-hotpath state rule:
	 * - GL state changes in A2 paths must go through RB_* / IRenderBackend entry points.
	 * - direct GL_* / GL_SetState calls are forbidden in A2 hotpaths.
	 * - temporary migration exceptions are explicitly listed below and treated as
	 *   transition-only helpers that must be removed once their callers migrate.
	 */
	/* Preconditions: active GL context on render thread; Side effects: starts backend frame bookkeeping/debug scope; Threading: single render thread only. */
	void (*BeginFrame) (const char *label);
	/* Preconditions: matching BeginFrame already issued for current frame; Side effects: flushes/completes backend frame scope; Threading: single render thread only. */
	void (*EndFrame) (void);
	/* Preconditions: inside BeginFrame/EndFrame and pass enum describes the next phase; Side effects: binds pass-local GPU state; Threading: single render thread only. */
	void (*BeginPass) (rb_pass_t pass);
	/* Preconditions: matching BeginPass is active; Side effects: finalizes pass-local state; Threading: single render thread only. */
	void (*EndPass) (void);
	/* Preconditions: required VAO/VBO/program state already bound by caller; Side effects: submits non-indexed draw call to GPU driver; Threading: single render thread only. */
	void (*DrawArrays) (GLenum mode, GLint first, GLsizei count);
	/* Preconditions: required indexed draw state and index buffer/pointer are valid; Side effects: submits indexed draw call to GPU driver; Threading: single render thread only. */
	void (*DrawElements) (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
	/* Preconditions: compute pipeline state/resources already prepared; Side effects: dispatches compute workload to GPU queue; Threading: single render thread only. */
	void (*DispatchCompute) (GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
	/* Preconditions: target/framebuffer are valid for current context; Side effects: mutates framebuffer binding state; Threading: single render thread only. */
	void (*BindFramebuffer) (GLenum target, GLuint framebuffer);
	/* Preconditions: texture unit/target/texture are valid handles; Side effects: mutates texture binding state for texunit; Threading: single render thread only. */
	void (*BindTexture) (GLenum texunit, GLenum target, GLuint texture);
	/* Preconditions: target/buffer are valid handles; Side effects: mutates buffer binding state; Threading: single render thread only. */
	void (*BindBuffer) (GLenum target, GLuint buffer);
} render_backend_vtable_t;

typedef render_backend_vtable_t IRenderBackend;

/*
 * Central A2 render-state transition exceptions (temporary, explicitly allowed):
 * - RB_DepthFunc: legacy material code still overrides depth func per stage.
 * - RB_BlendFunc: legacy material code still uses custom blend tuples not yet
 *   representable through GLS_* blend policy.
 *
 * Everything else in A2 hotpaths must route state through RB_SetState plus the
 * RB_* owner wrappers.
 */

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
/*
 * Fallback rule:
 * - backend path is considered only when global backend toggle is on (r_backend == 1)
 *   and the per-block toggle is enabled (toggle && toggle->value != 0).
 * - otherwise legacy_fn runs immediately.
 * - with toggles enabled, legacy_fn is still used when backend path is unavailable
 *   (!backend_path_available or backend_fn == NULL) or when backend_fn returns false.
 */
void RBackend_DispatchBlock (const char *block_name, cvar_t *toggle, qboolean backend_path_available,
	const char *unavailable_reason, rbackend_block_fn_t backend_fn, void (*legacy_fn)(void), qboolean *warned_once);
void RBackend_DebugCaptureEndFrameHash (void);

#endif
