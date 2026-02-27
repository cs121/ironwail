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

void RB_SetState_Owner (unsigned mask, const char *owner);
void RB_UseProgram_Owner (GLuint program, const char *owner);
qboolean RB_BindTexture_Owner (GLenum texunit, gltexture_t *texture, const char *owner);
#define RB_SetState(mask) RB_SetState_Owner ((mask), __func__)
#define RB_UseProgram(program) RB_UseProgram_Owner ((program), __func__)
#define RB_BindTexture(texunit, texture) RB_BindTexture_Owner ((texunit), (texture), __func__)
#define RB_SetStateWithOwner(mask, owner) RB_SetState_Owner ((mask), (owner))
#define RB_UseProgramWithOwner(program, owner) RB_UseProgram_Owner ((program), (owner))
#define RB_BindTextureWithOwner(texunit, texture, owner) RB_BindTexture_Owner ((texunit), (texture), (owner))
void RB_BindFramebuffer (GLenum target, GLuint framebuffer);
void RB_DrawArrays (GLenum mode, GLint first, GLsizei count);
void RB_DrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void RB_Clear (GLbitfield mask);
void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height);
void RB_Scissor (GLint x, GLint y, GLsizei width, GLsizei height);
void RB_DepthFunc (GLenum func);
void RB_BlendFunc (GLenum sfactor, GLenum dfactor);
void RB_DrawBuffer (GLenum buf);
void RB_ReadBuffer (GLenum src);

typedef void (*rb_pass_setup_hook_t) (rb_pass_t pass);
void RB_SetPassSetupHook (rb_pass_setup_hook_t hook);
void RB_BeginPass (rb_pass_t pass);
void RB_EndPass (void);
const char *RB_DebugStateOwnersString (void);

#endif
