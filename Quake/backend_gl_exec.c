#include "quakedef.h"
#include "rb_gl.h"
#include "render_backend.h"


extern cvar_t r_postfx;
extern cvar_t r_postfx_damage;
extern cvar_t r_postfx_pickup;
extern cvar_t r_postfx_powerup;
extern cvar_t r_postfx_underwater;
extern cvar_t r_motionblur;
extern cvar_t r_dof;
extern cvar_t r_autoexposure;

static unsigned long long RBackend_DebugHashFrameRGB (const byte *pixels, size_t count)
{
	const unsigned long long fnv_offset = 1469598103934665603ull;
	const unsigned long long fnv_prime = 1099511628211ull;
	unsigned long long hash = fnv_offset;

	for (size_t i = 0; i < count; ++i)
	{
		hash ^= (unsigned long long)pixels[i];
		hash *= fnv_prime;
	}

	return hash;
}

typedef struct rbackend_framehash_debug_state_s
{
	char mapname[sizeof (cl.mapname)];
	int scene_id;
	int width;
	int height;
	unsigned long long hash[2];
	int frame[2];
	byte *pixels[2];
	qboolean valid[2];
} rbackend_framehash_debug_state_t;

static rbackend_framehash_debug_state_t r_backend_framehash_debug_state;

static qboolean RBackend_DebugPostFXKnownExceptionActive (void)
{
	if (r_postfx.value <= 0.f)
		return false;

	if (r_postfx_damage.value > 0.f || r_postfx_pickup.value > 0.f || r_postfx_powerup.value > 0.f
		|| r_postfx_underwater.value > 0.f || r_motionblur.value > 0.f || r_dof.value > 0.f
		|| r_autoexposure.value > 0.f)
		return true;

	return false;
}

static qboolean RBackend_DebugCompareWithEpsilon (const byte *lhs, const byte *rhs, size_t count, int epsilon)
{
	for (size_t i = 0; i < count; ++i)
	{
		int delta = (int)lhs[i] - (int)rhs[i];
		if (delta < 0)
			delta = -delta;
		if (delta > epsilon)
			return false;
	}

	return true;
}

static void RBackend_DebugResetFrameHashState (rbackend_framehash_debug_state_t *state)
{
	if (!state)
		return;

	for (int i = 0; i < 2; ++i)
	{
		if (state->pixels[i])
			free (state->pixels[i]);
		state->pixels[i] = NULL;
		state->valid[i] = false;
		state->hash[i] = 0;
		state->frame[i] = 0;
	}

	state->mapname[0] = '\0';
	state->scene_id = 0;
	state->width = 0;
	state->height = 0;
}


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
	RB_DrawElements (mode, count, type, indices);
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
	const char *unavailable_reason, rbackend_block_fn_t backend_fn, void (*legacy_fn)(void), qboolean *warned_once)
{
	const qboolean backend_globally_enabled = ((int)r_backend.value == 1);
	const qboolean block_toggle_enabled = toggle && (toggle->value != 0.f);
	const qboolean can_try_backend = backend_globally_enabled && block_toggle_enabled;

	if (!legacy_fn)
		return;

	if (!can_try_backend)
	{
		legacy_fn ();
		return;
	}

	if (!backend_path_available || !backend_fn)
	{
		if (warned_once && !*warned_once)
		{
			Con_Warning ("render backend: %s backend path unavailable (%s), falling back to legacy\n",
				block_name ? block_name : "(unknown block)",
				unavailable_reason ? unavailable_reason : "unspecified");
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


void RBackend_DebugCaptureEndFrameHash (void)
{
	rbackend_framehash_debug_state_t *state = &r_backend_framehash_debug_state;
	const int backend_index = ((int)r_backend.value == 1) ? 1 : 0;
	const int scene_id = (int)Q_rint (r_backend_framehash_scene.value);
	const size_t rgb_bytes = (size_t)glwidth * (size_t)glheight * 3;
	GLint prev_pack_alignment = 4;
	byte *pixels;
	qboolean scene_changed;

	if (r_backend_framehash_debug.value <= 0.f)
		return;
	if (glwidth <= 0 || glheight <= 0)
		return;

	scene_changed = q_strcasecmp (state->mapname, cl.mapname) || state->scene_id != scene_id
		|| state->width != glwidth || state->height != glheight;
	if (scene_changed)
		RBackend_DebugResetFrameHashState (state);

	if (!state->mapname[0])
		q_strlcpy (state->mapname, cl.mapname[0] ? cl.mapname : "(nomap)", sizeof (state->mapname));
	state->scene_id = scene_id;
	state->width = glwidth;
	state->height = glheight;

	pixels = (byte *)malloc (rgb_bytes);
	if (!pixels)
		return;

	glGetIntegerv (GL_PACK_ALIGNMENT, &prev_pack_alignment); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#backend_gl_execc
	glPixelStorei (GL_PACK_ALIGNMENT, 1); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#backend_gl_execc
	glReadPixels (glx, gly, glwidth, glheight, GL_RGB, GL_UNSIGNED_BYTE, pixels); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#backend_gl_execc
	glPixelStorei (GL_PACK_ALIGNMENT, prev_pack_alignment); // GL-EXCEPTION: docs/render_backend_gl_exceptions.md#backend_gl_execc

	if (state->pixels[backend_index])
		free (state->pixels[backend_index]);
	state->pixels[backend_index] = pixels;
	state->hash[backend_index] = RBackend_DebugHashFrameRGB (pixels, rgb_bytes);
	state->frame[backend_index] = host_framecount;
	state->valid[backend_index] = true;

	Con_DPrintf ("backend framehash: map=%s scene=%d backend=%d frame=%d hash=%016llx\n",
		state->mapname, state->scene_id, backend_index, host_framecount,
		(unsigned long long)state->hash[backend_index]);

	if (state->valid[0] && state->valid[1])
	{
		qboolean equal = (state->hash[0] == state->hash[1]);
		int epsilon = CLAMP (0, (int)Q_rint (r_backend_framehash_epsilon.value), 255);
		qboolean epsilon_allowed = epsilon > 0 && RBackend_DebugPostFXKnownExceptionActive ();
		qboolean epsilon_equal = false;

		if (!equal && epsilon_allowed)
			epsilon_equal = RBackend_DebugCompareWithEpsilon (state->pixels[0], state->pixels[1], rgb_bytes, epsilon);

		if (!equal && !epsilon_equal)
		{
			Con_Warning ("backend framehash mismatch: map=%s scene=%d backend0=%016llx@f%d backend1=%016llx@f%d "
				"ctx[r_backend=%d ui=%d postfx=%d alias=%d world=%d particles=%d fogvol=%d r_postfx=%d eps=%d]\n",
				state->mapname,
				state->scene_id,
				(unsigned long long)state->hash[0], state->frame[0],
				(unsigned long long)state->hash[1], state->frame[1],
				(int)Q_rint (r_backend.value),
				(int)Q_rint (r_backend_ui.value),
				(int)Q_rint (r_backend_postfx.value),
				(int)Q_rint (r_backend_alias.value),
				(int)Q_rint (r_backend_world.value),
				(int)Q_rint (r_backend_particles.value),
				(int)Q_rint (r_backend_fogvol.value),
				(int)Q_rint (r_postfx.value),
				epsilon);
		}
	}
}
