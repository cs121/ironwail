#include "quakedef.h"
#include "glquake.h"
#include "gl_backend.h"

static gl_proc_address_loader_t s_gl_proc_loader = NULL;

static struct gl_backend_state_cache_s
{
	qboolean viewport_valid;
	GLint viewport[4];
	qboolean color_mask_valid;
	GLboolean color_mask[4];
	qboolean depth_mask_valid;
	GLboolean depth_mask;
	qboolean depth_func_valid;
	GLenum depth_func;
	qboolean stencil_test_valid;
	qboolean stencil_test_enabled;
	qboolean stencil_mask_valid;
	GLuint stencil_mask;
	qboolean stencil_func_valid;
	GLenum stencil_func;
	GLint stencil_ref;
	GLuint stencil_value_mask;
	qboolean stencil_op_valid;
	GLenum stencil_op_sfail;
	GLenum stencil_op_dpfail;
	GLenum stencil_op_dppass;
} s_gl_state_cache;

void GL_Backend_ResetStateCache (void)
{
	memset (&s_gl_state_cache, 0, sizeof (s_gl_state_cache));
}

void GL_Backend_SetViewportCached (int x, int y, int width, int height)
{
	if (!s_gl_state_cache.viewport_valid
		|| s_gl_state_cache.viewport[0] != x
		|| s_gl_state_cache.viewport[1] != y
		|| s_gl_state_cache.viewport[2] != width
		|| s_gl_state_cache.viewport[3] != height)
	{
		glViewport (x, y, width, height);
		s_gl_state_cache.viewport[0] = x;
		s_gl_state_cache.viewport[1] = y;
		s_gl_state_cache.viewport[2] = width;
		s_gl_state_cache.viewport[3] = height;
		s_gl_state_cache.viewport_valid = true;
	}
}

void GL_Backend_SetColorMaskCached (int r, int g, int b, int a)
{
	const GLboolean nr = r ? GL_TRUE : GL_FALSE;
	const GLboolean ng = g ? GL_TRUE : GL_FALSE;
	const GLboolean nb = b ? GL_TRUE : GL_FALSE;
	const GLboolean na = a ? GL_TRUE : GL_FALSE;

	if (!s_gl_state_cache.color_mask_valid
		|| s_gl_state_cache.color_mask[0] != nr
		|| s_gl_state_cache.color_mask[1] != ng
		|| s_gl_state_cache.color_mask[2] != nb
		|| s_gl_state_cache.color_mask[3] != na)
	{
		glColorMask (nr, ng, nb, na);
		s_gl_state_cache.color_mask[0] = nr;
		s_gl_state_cache.color_mask[1] = ng;
		s_gl_state_cache.color_mask[2] = nb;
		s_gl_state_cache.color_mask[3] = na;
		s_gl_state_cache.color_mask_valid = true;
	}
}

void GL_Backend_SetDepthMaskCached (int enabled)
{
	const GLboolean mask = enabled ? GL_TRUE : GL_FALSE;

	if (!s_gl_state_cache.depth_mask_valid || s_gl_state_cache.depth_mask != mask)
	{
		glDepthMask (mask);
		s_gl_state_cache.depth_mask = mask;
		s_gl_state_cache.depth_mask_valid = true;
	}
}

void GL_Backend_SetDepthFuncCached (unsigned func)
{
	if (!s_gl_state_cache.depth_func_valid || s_gl_state_cache.depth_func != (GLenum)func)
	{
		glDepthFunc ((GLenum)func);
		s_gl_state_cache.depth_func = (GLenum)func;
		s_gl_state_cache.depth_func_valid = true;
	}
}

void GL_Backend_SetStencilTestCached (qboolean enabled)
{
	if (!s_gl_state_cache.stencil_test_valid || s_gl_state_cache.stencil_test_enabled != enabled)
	{
		if (enabled)
			glEnable (GL_STENCIL_TEST);
		else
			glDisable (GL_STENCIL_TEST);
		s_gl_state_cache.stencil_test_enabled = enabled;
		s_gl_state_cache.stencil_test_valid = true;
	}
}

void GL_Backend_SetStencilMaskCached (unsigned mask)
{
	if (!s_gl_state_cache.stencil_mask_valid || s_gl_state_cache.stencil_mask != (GLuint)mask)
	{
		glStencilMask ((GLuint)mask);
		s_gl_state_cache.stencil_mask = (GLuint)mask;
		s_gl_state_cache.stencil_mask_valid = true;
	}
}

void GL_Backend_SetStencilFuncCached (unsigned func, int ref, unsigned mask)
{
	if (!s_gl_state_cache.stencil_func_valid
		|| s_gl_state_cache.stencil_func != (GLenum)func
		|| s_gl_state_cache.stencil_ref != (GLint)ref
		|| s_gl_state_cache.stencil_value_mask != (GLuint)mask)
	{
		glStencilFunc ((GLenum)func, (GLint)ref, (GLuint)mask);
		s_gl_state_cache.stencil_func = (GLenum)func;
		s_gl_state_cache.stencil_ref = (GLint)ref;
		s_gl_state_cache.stencil_value_mask = (GLuint)mask;
		s_gl_state_cache.stencil_func_valid = true;
	}
}

void GL_Backend_SetStencilOpCached (unsigned sfail, unsigned dpfail, unsigned dppass)
{
	if (!s_gl_state_cache.stencil_op_valid
		|| s_gl_state_cache.stencil_op_sfail != (GLenum)sfail
		|| s_gl_state_cache.stencil_op_dpfail != (GLenum)dpfail
		|| s_gl_state_cache.stencil_op_dppass != (GLenum)dppass)
	{
		glStencilOp ((GLenum)sfail, (GLenum)dpfail, (GLenum)dppass);
		s_gl_state_cache.stencil_op_sfail = (GLenum)sfail;
		s_gl_state_cache.stencil_op_dpfail = (GLenum)dpfail;
		s_gl_state_cache.stencil_op_dppass = (GLenum)dppass;
		s_gl_state_cache.stencil_op_valid = true;
	}
}

void GL_Backend_SetProcAddressLoader (gl_proc_address_loader_t loader)
{
	s_gl_proc_loader = loader;
}

void *GL_Backend_GetProcAddress (const char *name)
{
	if (!s_gl_proc_loader || !name || !name[0])
		return NULL;
	return s_gl_proc_loader (name);
}

