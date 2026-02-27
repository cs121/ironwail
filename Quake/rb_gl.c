#include "quakedef.h"
#include "rb_gl.h"

#if !RB_GL_PASSTHROUGH_ONLY
#error "RB wrappers must remain pass-through during this migration stage"
#endif

void RB_SetState (unsigned mask)
{
	GL_SetState (mask);
}

void RB_UseProgram (GLuint program)
{
	GL_UseProgram (program);
}

qboolean RB_BindTexture (GLenum texunit, gltexture_t *texture)
{
	return GL_Bind (texunit, texture);
}

void RB_BindFramebuffer (GLenum target, GLuint framebuffer)
{
	GL_BindFramebufferFunc (target, framebuffer);
}

void RB_DrawArrays (GLenum mode, GLint first, GLsizei count)
{
	glDrawArrays (mode, first, count);
}

void RB_Clear (GLbitfield mask)
{
	glClear (mask);
}

void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	glViewport (x, y, width, height);
}
