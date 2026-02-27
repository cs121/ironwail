#include "quakedef.h"
#include "rb_gl.h"
#include "render_backend.h"

static void BackendGL_BeginFrame (const char *label)
{
	if (label)
		Con_DPrintf ("render backend: BeginFrame (%s)\n", label);
	else
		Con_DPrintf ("render backend: BeginFrame\n");
}

static void BackendGL_EndFrame (void)
{
	Con_DPrintf ("render backend: EndFrame\n");
}

static void BackendGL_BeginPass (rb_pass_t pass)
{
	RB_BeginPass (pass);
}

static void BackendGL_EndPass (void)
{
	RB_EndPass ();
}

static void BackendGL_DrawArrays (GLenum mode, GLint first, GLsizei count)
{
	RB_DrawArrays (mode, first, count);
}

static void BackendGL_DrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	glDrawElements (mode, count, type, indices);
}

static void BackendGL_DispatchCompute (GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z)
{
	GL_DispatchComputeFunc (num_groups_x, num_groups_y, num_groups_z);
}

static void BackendGL_BindFramebuffer (GLenum target, GLuint framebuffer)
{
	RB_BindFramebuffer (target, framebuffer);
}

static void BackendGL_BindTexture (GLenum texunit, GLenum target, GLuint texture)
{
	GL_BindNative (texunit, target, texture);
}

static void BackendGL_BindBuffer (GLenum target, GLuint buffer)
{
	GL_BindBuffer (target, buffer);
}

static const render_backend_vtable_t backend_gl_vtable = {
	BackendGL_BeginFrame,
	BackendGL_EndFrame,
	BackendGL_BeginPass,
	BackendGL_EndPass,
	BackendGL_DrawArrays,
	BackendGL_DrawElements,
	BackendGL_DispatchCompute,
	BackendGL_BindFramebuffer,
	BackendGL_BindTexture,
	BackendGL_BindBuffer
};

const render_backend_vtable_t *RBackend_GetVTable (void)
{
	return &backend_gl_vtable;
}

void RBackend_DispatchRenderView (void (*legacy_fn)(void))
{
	if (!legacy_fn)
		return;

	if ((int)r_backend.value == 1)
	{
		backend_gl_vtable.BeginFrame ("R_RenderView");
		legacy_fn ();
		backend_gl_vtable.EndFrame ();
		return;
	}

	legacy_fn ();
}

void RBackend_DispatchUpdateScreen (void (*legacy_fn)(void))
{
	if (!legacy_fn)
		return;

	if ((int)r_backend.value == 1)
	{
		backend_gl_vtable.BeginFrame ("SCR_UpdateScreen");
		legacy_fn ();
		backend_gl_vtable.EndFrame ();
		return;
	}

	legacy_fn ();
}

void RBackend_DispatchBlock (const char *block_name, cvar_t *toggle, qboolean backend_path_available,
	rbackend_block_fn_t backend_fn, void (*legacy_fn)(void), qboolean *warned_once)
{
	qboolean can_use_backend;

	if (!legacy_fn)
		return;

	can_use_backend = ((int)r_backend.value == 1) && toggle && (toggle->value != 0.f);
	if (!can_use_backend)
	{
		legacy_fn ();
		return;
	}

	if (!backend_path_available || !backend_fn)
	{
		if (warned_once && !*warned_once)
		{
			Con_Warning ("render backend: %s backend path unavailable, falling back to legacy\n",
				block_name ? block_name : "(unknown block)");
			*warned_once = true;
		}
		legacy_fn ();
		return;
	}

	if (!backend_fn ())
	{
		if (warned_once && !*warned_once)
		{
			Con_Warning ("render backend: %s backend block failed, falling back to legacy\n",
				block_name ? block_name : "(unknown block)");
			*warned_once = true;
		}
		legacy_fn ();
	}
}
