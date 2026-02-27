#ifndef RB_GL_H
#define RB_GL_H

#include "glquake.h"
#include "gl_texmgr.h"
#include "rb_pass.h"

/*
 * Build guard: wrappers are pass-through only during the initial migration.
 * Keep this set to 1 until backend behavior intentionally diverges.
 */
#define RB_GL_PASSTHROUGH_ONLY 1

void RB_SetState (unsigned mask);
void RB_UseProgram (GLuint program);
qboolean RB_BindTexture (GLenum texunit, gltexture_t *texture);
void RB_BindFramebuffer (GLenum target, GLuint framebuffer);
void RB_DrawArrays (GLenum mode, GLint first, GLsizei count);
void RB_Clear (GLbitfield mask);
void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height);

typedef void (*rb_pass_setup_hook_t) (rb_pass_t pass);
void RB_SetPassSetupHook (rb_pass_setup_hook_t hook);
void RB_BeginPass (rb_pass_t pass);
void RB_EndPass (void);

#endif
