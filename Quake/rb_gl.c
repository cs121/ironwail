#include "quakedef.h"
#include "rb_gl.h"

#if !RB_GL_PASSTHROUGH_ONLY
#error "RB wrappers must remain pass-through during this migration stage"
#endif

#ifndef RB_VALIDATE_TEXTURE_UNIT_LIMIT
	#define RB_VALIDATE_TEXTURE_UNIT_LIMIT 16
#endif

#define RB_PASS_BASELINE_TEXTURE_UNITS 3

typedef struct rb_pass_baseline_gl_s
{
	GLint draw_fbo;
	GLint read_fbo;
	GLint viewport[4];
	qboolean scissor_enable;
	GLint scissor_box[4];
	GLenum depth_func;
	GLboolean depth_mask;
	GLenum blend_src;
	GLenum blend_dst;
	GLenum blend_equation;
	GLenum cull_mode;
	GLboolean color_mask[4];
	qboolean stencil_enable;
	GLenum stencil_func;
	GLint stencil_ref;
	GLuint stencil_value_mask;
	GLenum stencil_sfail;
	GLenum stencil_dpfail;
	GLenum stencil_dppass;
	GLuint stencil_writemask;
	GLuint program;
	GLuint vao;
	GLuint array_buffer;
	GLuint element_array_buffer;
	GLenum active_texture;
	GLuint sampler_bindings[RB_VALIDATE_TEXTURE_UNIT_LIMIT];
	qboolean framebuffer_srgb;
	GLenum polygon_mode;
} rb_pass_baseline_gl_t;

typedef struct rb_pass_info_s
{
	const char *debug_name;
	unsigned baseline_state;
	rb_pass_baseline_gl_t baseline_gl;
	unsigned exit_allow_state_mask;
	unsigned exit_allow_texture_mask;
	unsigned exit_allow_sampler_mask;
	qboolean exit_allow_program;
	qboolean exit_allow_fbo;
	qboolean exit_allow_vao;
	qboolean exit_allow_array_buffer;
	qboolean exit_allow_element_array_buffer;
} rb_pass_info_t;

typedef struct rb_gl_state_snapshot_s
{
	GLint draw_fbo;
	GLint read_fbo;
	GLint viewport[4];
	GLboolean scissor_enable;
	GLint scissor_box[4];
	GLint depth_func;
	GLboolean depth_mask;
	GLint blend_src;
	GLint blend_dst;
	GLint blend_equation;
	GLboolean cull_enable;
	GLint cull_mode;
	GLboolean color_mask[4];
	GLboolean stencil_enable;
	GLint stencil_func;
	GLint stencil_ref;
	GLint stencil_value_mask;
	GLint stencil_sfail;
	GLint stencil_dpfail;
	GLint stencil_dppass;
	GLint stencil_writemask;
	GLint program;
	GLint vao;
	GLint array_buffer;
	GLint element_array_buffer;
	GLint active_texture;
	GLint sampler_bindings[RB_VALIDATE_TEXTURE_UNIT_LIMIT];
	GLboolean framebuffer_srgb;
	GLint polygon_mode[2];
	GLint tex2d[RB_VALIDATE_TEXTURE_UNIT_LIMIT];
} rb_gl_state_snapshot_t;

/*
 * Render-backend pass baseline matrix (pass -> expected start state).
 *
 * | Pass               | Blend  | Depth test | Depth write | Cull | Program | Texture units 0..2 (baseline reset) |
 * |--------------------|--------|------------|-------------|------|---------|--------------------|
 * | PASS_WORLD_OPAQUE  | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_DLIGHT        | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_SKY           | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_WATER_OPAQUE  | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_WATER_ALPHA   | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_ENTS_OPAQUE   | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_ENTS_ALPHA    | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_PARTICLES     | Opaque | On         | On          | Back | 0       | Unbound            |
 * | PASS_POSTFX        | Opaque | Off        | Off         | None | 0       | Unbound            |
 * | PASS_FOGVOL        | Opaque | Off        | Off         | None | 0       | Unbound            |
 * | PASS_UI2D          | Opaque | Off        | Off         | None | 0       | Unbound            |
 */
static const rb_pass_info_t rb_pass_info[PASS_COUNT] = {
	[PASS_WORLD_OPAQUE] = {"PASS_WORLD_OPAQUE", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_DLIGHT] = {"PASS_DLIGHT", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_SKY] = {"PASS_SKY", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_WATER_OPAQUE] = {"PASS_WATER_OPAQUE", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_WATER_ALPHA] = {"PASS_WATER_ALPHA", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_ENTS_OPAQUE] = {"PASS_ENTS_OPAQUE", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_ENTS_ALPHA] = {"PASS_ENTS_ALPHA", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_PARTICLES] = {"PASS_PARTICLES", GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_TRUE, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_BACK, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_POSTFX] = {"PASS_POSTFX", GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_FALSE, GL_ONE, GL_ZERO, GL_FUNC_ADD, 0, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_FOGVOL] = {"PASS_FOGVOL", GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_FALSE, GL_ONE, GL_ZERO, GL_FUNC_ADD, 0, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
	[PASS_UI2D] = {"PASS_UI2D", GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0), {{0, 0, {0, 0, 0, 0}, false, {0, 0, 0, 0}, GL_LEQUAL, GL_FALSE, GL_ONE, GL_ZERO, GL_FUNC_ADD, 0, {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, false, GL_ALWAYS, 0, ~0u, GL_KEEP, GL_KEEP, GL_KEEP, ~0u, 0, 0, 0, 0, GL_TEXTURE0, {0}, false, GL_FILL}}},
};

static rb_pass_setup_hook_t rb_pass_setup_hook;
static qboolean rb_pass_active;
static rb_pass_t rb_current_pass = PASS_WORLD_OPAQUE;

#ifndef RB_DEBUG_STATE
	#if !defined(NDEBUG) || defined(_DEBUG) || defined(DEBUG)
		#define RB_DEBUG_STATE 1
	#else
		#define RB_DEBUG_STATE 0
	#endif
#endif

#if RB_DEBUG_STATE
typedef struct rb_state_debug_s
{
	const char *last_blend_owner;
	const char *last_depth_owner;
	const char *last_cull_owner;
	const char *last_program_owner;
	const char *last_texture_owner[RB_VALIDATE_TEXTURE_UNIT_LIMIT];
	const char *last_fbo_owner;
	const char *last_vao_owner;
	const char *last_array_buffer_owner;
	const char *last_element_array_buffer_owner;
	const char *last_sampler_owner[RB_VALIDATE_TEXTURE_UNIT_LIMIT];
	const char *last_color_mask_owner;
	const char *last_scissor_owner;
	const char *last_stencil_owner;
	const char *last_polygon_mode_owner;
	const char *last_framebuffer_srgb_owner;
	const char *last_draw_buffer_owner;
	const char *last_read_buffer_owner;
} rb_state_debug_t;

static rb_state_debug_t rb_state_debug;

static const char *RB_DebugOwnerOrUnknown (const char *owner)
{
	return owner ? owner : "<unknown>";
}

static const char *RB_BoolName (int value)
{
	return value ? "on" : "off";
}

static void RB_CaptureGLStateSnapshot (rb_gl_state_snapshot_t *snapshot)
{
	GLint prev_active_tex;
	int i;

	memset (snapshot, 0, sizeof (*snapshot));

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &snapshot->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &snapshot->read_fbo);
	glGetIntegerv (GL_VIEWPORT, snapshot->viewport);
	snapshot->scissor_enable = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, snapshot->scissor_box);
	glGetIntegerv (GL_DEPTH_FUNC, &snapshot->depth_func);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &snapshot->depth_mask);
	glGetIntegerv (GL_BLEND_SRC_RGB, &snapshot->blend_src);
	glGetIntegerv (GL_BLEND_DST_RGB, &snapshot->blend_dst);
	glGetIntegerv (GL_BLEND_EQUATION_RGB, &snapshot->blend_equation);
	snapshot->cull_enable = glIsEnabled (GL_CULL_FACE);
	glGetIntegerv (GL_CULL_FACE_MODE, &snapshot->cull_mode);
	glGetBooleanv (GL_COLOR_WRITEMASK, snapshot->color_mask);
	snapshot->stencil_enable = glIsEnabled (GL_STENCIL_TEST);
	glGetIntegerv (GL_STENCIL_FUNC, &snapshot->stencil_func);
	glGetIntegerv (GL_STENCIL_REF, &snapshot->stencil_ref);
	glGetIntegerv (GL_STENCIL_VALUE_MASK, &snapshot->stencil_value_mask);
	glGetIntegerv (GL_STENCIL_FAIL, &snapshot->stencil_sfail);
	glGetIntegerv (GL_STENCIL_PASS_DEPTH_FAIL, &snapshot->stencil_dpfail);
	glGetIntegerv (GL_STENCIL_PASS_DEPTH_PASS, &snapshot->stencil_dppass);
	glGetIntegerv (GL_STENCIL_WRITEMASK, &snapshot->stencil_writemask);
	glGetIntegerv (GL_CURRENT_PROGRAM, &snapshot->program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &snapshot->vao);
	glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &snapshot->array_buffer);
	glGetIntegerv (GL_ELEMENT_ARRAY_BUFFER_BINDING, &snapshot->element_array_buffer);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &snapshot->active_texture);
	snapshot->framebuffer_srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	glGetIntegerv (GL_POLYGON_MODE, snapshot->polygon_mode);

	prev_active_tex = snapshot->active_texture;
	for (i = 0; i < RB_VALIDATE_TEXTURE_UNIT_LIMIT; ++i)
	{
		GL_ActiveTextureFunc (GL_TEXTURE0 + i);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &snapshot->tex2d[i]);
		GL_GetIntegeri_vFunc (GL_SAMPLER_BINDING, i, &snapshot->sampler_bindings[i]);
	}
	GL_ActiveTextureFunc (prev_active_tex);
}

static qboolean RB_LogIncomingStateDiff (rb_pass_t pass, const rb_pass_baseline_gl_t *expected, const rb_gl_state_snapshot_t *actual)
{
	qboolean mismatch = false;
	int i;

	#define RB_LOG_MISMATCH(fmt, ...) \
		do { \
			if (!mismatch) \
				Con_Warning ("RB_BeginPass(%s): incoming state mismatch\n", rb_pass_info[pass].debug_name); \
			Con_Warning (fmt, __VA_ARGS__); \
			mismatch = true; \
		} while (0)

	if (actual->draw_fbo != expected->draw_fbo)
		RB_LOG_MISMATCH ("  draw_fbo: expected=%d actual=%d last_owner=%s\n", expected->draw_fbo, actual->draw_fbo, RB_DebugOwnerOrUnknown (rb_state_debug.last_fbo_owner));
	if (actual->read_fbo != expected->read_fbo)
		RB_LOG_MISMATCH ("  read_fbo: expected=%d actual=%d last_owner=%s\n", expected->read_fbo, actual->read_fbo, RB_DebugOwnerOrUnknown (rb_state_debug.last_fbo_owner));
	if (memcmp (actual->viewport, expected->viewport, sizeof (actual->viewport)) != 0)
		RB_LOG_MISMATCH ("  viewport: expected=(%d %d %d %d) actual=(%d %d %d %d)\n", expected->viewport[0], expected->viewport[1], expected->viewport[2], expected->viewport[3], actual->viewport[0], actual->viewport[1], actual->viewport[2], actual->viewport[3]);
	if ((actual->scissor_enable != (GLboolean)expected->scissor_enable) || memcmp (actual->scissor_box, expected->scissor_box, sizeof (actual->scissor_box)) != 0)
		RB_LOG_MISMATCH ("  scissor: expected=(%s %d %d %d %d) actual=(%s %d %d %d %d)\n", RB_BoolName (expected->scissor_enable), expected->scissor_box[0], expected->scissor_box[1], expected->scissor_box[2], expected->scissor_box[3], RB_BoolName (actual->scissor_enable), actual->scissor_box[0], actual->scissor_box[1], actual->scissor_box[2], actual->scissor_box[3]);
	if (actual->depth_func != (GLint)expected->depth_func || actual->depth_mask != expected->depth_mask)
		RB_LOG_MISMATCH ("  depth: expected=(func=%#x mask=%d) actual=(func=%#x mask=%d) last_owner=%s\n", expected->depth_func, expected->depth_mask, actual->depth_func, actual->depth_mask, RB_DebugOwnerOrUnknown (rb_state_debug.last_depth_owner));
	if (actual->blend_src != (GLint)expected->blend_src || actual->blend_dst != (GLint)expected->blend_dst || actual->blend_equation != (GLint)expected->blend_equation)
		RB_LOG_MISMATCH ("  blend: expected=(src=%#x dst=%#x eq=%#x) actual=(src=%#x dst=%#x eq=%#x) last_owner=%s\n", expected->blend_src, expected->blend_dst, expected->blend_equation, actual->blend_src, actual->blend_dst, actual->blend_equation, RB_DebugOwnerOrUnknown (rb_state_debug.last_blend_owner));
	if (actual->cull_enable != (GLboolean)((expected->cull_mode == GL_FRONT) || (expected->cull_mode == GL_BACK)) || (actual->cull_enable && actual->cull_mode != (GLint)expected->cull_mode))
		RB_LOG_MISMATCH ("  cull: expected=(%s mode=%#x) actual=(%s mode=%#x) last_owner=%s\n", RB_BoolName ((expected->cull_mode == GL_FRONT) || (expected->cull_mode == GL_BACK)), expected->cull_mode, RB_BoolName (actual->cull_enable), actual->cull_mode, RB_DebugOwnerOrUnknown (rb_state_debug.last_cull_owner));
	if (memcmp (actual->color_mask, expected->color_mask, sizeof (actual->color_mask)) != 0)
		RB_LOG_MISMATCH ("  color_mask: expected=(%d%d%d%d) actual=(%d%d%d%d)\n", expected->color_mask[0], expected->color_mask[1], expected->color_mask[2], expected->color_mask[3], actual->color_mask[0], actual->color_mask[1], actual->color_mask[2], actual->color_mask[3]);
	if (actual->stencil_enable != (GLboolean)expected->stencil_enable || actual->stencil_func != (GLint)expected->stencil_func || actual->stencil_ref != expected->stencil_ref || actual->stencil_value_mask != (GLint)expected->stencil_value_mask || actual->stencil_sfail != (GLint)expected->stencil_sfail || actual->stencil_dpfail != (GLint)expected->stencil_dpfail || actual->stencil_dppass != (GLint)expected->stencil_dppass || actual->stencil_writemask != (GLint)expected->stencil_writemask)
		RB_LOG_MISMATCH ("  stencil: expected=(%s func=%#x ref=%d mask=%#x op=%#x/%#x/%#x wmask=%#x) actual=(%s func=%#x ref=%d mask=%#x op=%#x/%#x/%#x wmask=%#x)\n", RB_BoolName (expected->stencil_enable), expected->stencil_func, expected->stencil_ref, expected->stencil_value_mask, expected->stencil_sfail, expected->stencil_dpfail, expected->stencil_dppass, expected->stencil_writemask, RB_BoolName (actual->stencil_enable), actual->stencil_func, actual->stencil_ref, actual->stencil_value_mask, actual->stencil_sfail, actual->stencil_dpfail, actual->stencil_dppass, actual->stencil_writemask);
	if (actual->program != (GLint)expected->program)
		RB_LOG_MISMATCH ("  program: expected=%u actual=%d last_owner=%s\n", expected->program, actual->program, RB_DebugOwnerOrUnknown (rb_state_debug.last_program_owner));
	if (actual->vao != (GLint)expected->vao)
		RB_LOG_MISMATCH ("  vao: expected=%u actual=%d last_owner=%s\n", expected->vao, actual->vao, RB_DebugOwnerOrUnknown (rb_state_debug.last_vao_owner));
	if (actual->array_buffer != (GLint)expected->array_buffer)
		RB_LOG_MISMATCH ("  array_buffer: expected=%u actual=%d last_owner=%s\n", expected->array_buffer, actual->array_buffer, RB_DebugOwnerOrUnknown (rb_state_debug.last_array_buffer_owner));
	if (actual->element_array_buffer != (GLint)expected->element_array_buffer)
		RB_LOG_MISMATCH ("  element_array_buffer: expected=%u actual=%d last_owner=%s\n", expected->element_array_buffer, actual->element_array_buffer, RB_DebugOwnerOrUnknown (rb_state_debug.last_element_array_buffer_owner));
	if (actual->active_texture != (GLint)expected->active_texture)
		RB_LOG_MISMATCH ("  active_texture: expected=%d actual=%d\n", expected->active_texture - GL_TEXTURE0, actual->active_texture - GL_TEXTURE0);
	for (i = 0; i < RB_VALIDATE_TEXTURE_UNIT_LIMIT; ++i)
	{
		if (actual->tex2d[i] != 0)
			RB_LOG_MISMATCH ("  texture[%d]: expected=0 actual=%d last_owner=%s\n", i, actual->tex2d[i], RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[i]));
		if (actual->sampler_bindings[i] != (GLint)expected->sampler_bindings[i])
			RB_LOG_MISMATCH ("  sampler[%d]: expected=%u actual=%d last_owner=%s\n", i, expected->sampler_bindings[i], actual->sampler_bindings[i], RB_DebugOwnerOrUnknown (rb_state_debug.last_sampler_owner[i]));
	}
	if (actual->framebuffer_srgb != (GLboolean)expected->framebuffer_srgb)
		RB_LOG_MISMATCH ("  framebuffer_srgb: expected=%s actual=%s\n", RB_BoolName (expected->framebuffer_srgb), RB_BoolName (actual->framebuffer_srgb));
	if (actual->polygon_mode[0] != (GLint)expected->polygon_mode || actual->polygon_mode[1] != (GLint)expected->polygon_mode)
		RB_LOG_MISMATCH ("  polygon_mode: expected=%#x actual=%#x/%#x\n", expected->polygon_mode, actual->polygon_mode[0], actual->polygon_mode[1]);

	#undef RB_LOG_MISMATCH

	if (mismatch && r_rb_assert_state.value > 0.f)
		Sys_Error ("RB_BeginPass(%s): incoming state mismatch", rb_pass_info[pass].debug_name);

	return mismatch;
}

static void RB_ValidatePassExit (rb_pass_t pass)
{
	const rb_pass_info_t *policy;
	unsigned expected_state;
	unsigned state_allow_mask;
	GLint current_program = 0;
	GLint previous_active_tex = 0;
	GLint tex2d[RB_VALIDATE_TEXTURE_UNIT_LIMIT] = {0};
	GLint sampler[RB_VALIDATE_TEXTURE_UNIT_LIMIT] = {0};
	GLint draw_fbo = 0;
	GLint vao = 0;
	GLint array_buffer = 0;
	GLint element_array_buffer = 0;
	qboolean mismatch = false;
	int i;

	if (pass < 0 || pass >= PASS_COUNT)
		return;

	policy = &rb_pass_info[pass];
	expected_state = policy->baseline_state;
	state_allow_mask = policy->exit_allow_state_mask;

	glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &previous_active_tex);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vao);
	glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &array_buffer);
	glGetIntegerv (GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_array_buffer);

	for (i = 0; i < RB_VALIDATE_TEXTURE_UNIT_LIMIT; ++i)
	{
		GL_ActiveTextureFunc (GL_TEXTURE0 + i);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex2d[i]);
		GL_GetIntegeri_vFunc (GL_SAMPLER_BINDING, i, &sampler[i]);
	}
	GL_ActiveTextureFunc (previous_active_tex);

	if (((glstate ^ expected_state) & ~state_allow_mask & GLS_MASK_BLEND) != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  blend: expected=%s actual=%s last_owner=%s\n",
			RB_BlendName (expected_state),
			RB_BlendName (glstate),
			RB_DebugOwnerOrUnknown (rb_state_debug.last_blend_owner));
		mismatch = true;
	}

	if (((glstate ^ expected_state) & ~state_allow_mask & (GLS_NO_ZTEST | GLS_NO_ZWRITE)) != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  depth: expected=(ztest:%d zwrite:%d) actual=(ztest:%d zwrite:%d) last_owner=%s\n",
			(expected_state & GLS_NO_ZTEST) == 0,
			(expected_state & GLS_NO_ZWRITE) == 0,
			(glstate & GLS_NO_ZTEST) == 0,
			(glstate & GLS_NO_ZWRITE) == 0,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_depth_owner));
		mismatch = true;
	}

	if (((glstate ^ expected_state) & ~state_allow_mask & GLS_MASK_CULL) != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  cull: expected=%s actual=%s last_owner=%s\n",
			RB_CullName (expected_state),
			RB_CullName (glstate),
			RB_DebugOwnerOrUnknown (rb_state_debug.last_cull_owner));
		mismatch = true;
	}

	if (!policy->exit_allow_program && current_program != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  program: expected=0 actual=%d last_owner=%s\n",
			(int)current_program,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_program_owner));
		mismatch = true;
	}

	for (i = 0; i < RB_VALIDATE_TEXTURE_UNIT_LIMIT; ++i)
	{
		if (((policy->exit_allow_texture_mask >> i) & 1u) == 0 && tex2d[i] != 0)
		{
			if (!mismatch)
				Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
			Con_Warning ("  texture[%d]: expected=0 actual=%d last_owner=%s\n",
				i,
				(int)tex2d[i],
				RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[i]));
			mismatch = true;
		}

		if (((policy->exit_allow_sampler_mask >> i) & 1u) == 0 && sampler[i] != 0)
		{
			if (!mismatch)
				Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
			Con_Warning ("  sampler[%d]: expected=0 actual=%d last_owner=%s\n",
				i,
				(int)sampler[i],
				RB_DebugOwnerOrUnknown (rb_state_debug.last_sampler_owner[i]));
			mismatch = true;
		}
	}

	if (!policy->exit_allow_fbo && draw_fbo != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  draw_fbo: expected=0 actual=%d last_owner=%s\n",
			(int)draw_fbo,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_fbo_owner));
		mismatch = true;
	}

	if (!policy->exit_allow_vao && vao != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  vao: expected=0 actual=%d last_owner=%s\n",
			(int)vao,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_vao_owner));
		mismatch = true;
	}

	if (!policy->exit_allow_array_buffer && array_buffer != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  array_buffer: expected=0 actual=%d last_owner=%s\n",
			(int)array_buffer,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_array_buffer_owner));
		mismatch = true;
	}

	if (!policy->exit_allow_element_array_buffer && element_array_buffer != 0)
	{
		if (!mismatch)
			Con_Warning ("RB_EndPass(%s): exit state mismatch\n", policy->debug_name);
		Con_Warning ("  element_array_buffer: expected=0 actual=%d last_owner=%s\n",
			(int)element_array_buffer,
			RB_DebugOwnerOrUnknown (rb_state_debug.last_element_array_buffer_owner));
		mismatch = true;
	}

	if (mismatch && r_rb_assert_state.value > 0.f)
		Sys_Error ("RB_EndPass(%s): exit state mismatch", policy->debug_name);
}
#endif

static rb_pass_baseline_gl_t RB_BuildPassBaselineGL (rb_pass_t pass)
{
	rb_pass_baseline_gl_t baseline = rb_pass_info[pass].baseline_gl;

	baseline.viewport[0] = glx;
	baseline.viewport[1] = gly;
	baseline.viewport[2] = glwidth;
	baseline.viewport[3] = glheight;
	baseline.scissor_box[0] = glx;
	baseline.scissor_box[1] = gly;
	baseline.scissor_box[2] = glwidth;
	baseline.scissor_box[3] = glheight;
	baseline.depth_func = gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL;

	return baseline;
}

static void RB_ApplyPassBaselineGL (const rb_pass_baseline_gl_t *baseline)
{
	int i;

	RB_BindFramebufferWithOwner (GL_DRAW_FRAMEBUFFER, baseline->draw_fbo, "RB_BeginPass");
	RB_BindFramebufferWithOwner (GL_READ_FRAMEBUFFER, baseline->read_fbo, "RB_BeginPass");
	RB_Viewport (baseline->viewport[0], baseline->viewport[1], baseline->viewport[2], baseline->viewport[3]);
	if (baseline->scissor_enable)
		glEnable (GL_SCISSOR_TEST);
	else
		glDisable (GL_SCISSOR_TEST);
	RB_ScissorWithOwner (baseline->scissor_box[0], baseline->scissor_box[1], baseline->scissor_box[2], baseline->scissor_box[3], "RB_BeginPass");
	/* Transition helper: legacy paths still override depth func directly. */
	RB_DepthFunc (baseline->depth_func);
	/* Transition helper: legacy paths still use explicit blend tuples. */
	RB_BlendFunc (baseline->blend_src, baseline->blend_dst);
	GL_BlendEquationFunc (baseline->blend_equation);
	RB_ColorMaskWithOwner (baseline->color_mask[0], baseline->color_mask[1], baseline->color_mask[2], baseline->color_mask[3], "RB_BeginPass");
	RB_StencilTestWithOwner (baseline->stencil_enable, "RB_BeginPass");
	RB_StencilFuncWithOwner (baseline->stencil_func, baseline->stencil_ref, baseline->stencil_value_mask, "RB_BeginPass");
	RB_StencilOpWithOwner (baseline->stencil_sfail, baseline->stencil_dpfail, baseline->stencil_dppass, "RB_BeginPass");
	RB_StencilMaskWithOwner (baseline->stencil_writemask, "RB_BeginPass");
	RB_UseProgramWithOwner (baseline->program, "RB_BeginPass");
	RB_BindVertexArrayWithOwner (baseline->vao, "RB_BeginPass");
	RB_BindBufferWithOwner (GL_ARRAY_BUFFER, baseline->array_buffer, "RB_BeginPass");
	RB_BindBufferWithOwner (GL_ELEMENT_ARRAY_BUFFER, baseline->element_array_buffer, "RB_BeginPass");
	GL_ActiveTextureFunc (baseline->active_texture);
	for (i = 0; i < RB_PASS_BASELINE_TEXTURE_UNITS; ++i)
	{
		GL_ActiveTextureFunc (GL_TEXTURE0 + i);
		glBindTexture (GL_TEXTURE_2D, 0);
		RB_BindSamplerWithOwner ((GLuint)i, baseline->sampler_bindings[i], "RB_BeginPass");
	}
	GL_ActiveTextureFunc (baseline->active_texture);
	RB_EnableFramebufferSRGBWithOwner (baseline->framebuffer_srgb, "RB_BeginPass");
	RB_PolygonModeWithOwner (GL_FRONT_AND_BACK, baseline->polygon_mode, "RB_BeginPass");
}

void RB_SetState_Owner (unsigned mask, const char *owner)
{
	unsigned gl_mask = mask & ~RB_GLS_STENCIL_TEST;
	qboolean stencil_enable = (mask & RB_GLS_STENCIL_TEST) != 0;

	#if RB_DEBUG_STATE
	unsigned changed = glstate ^ gl_mask;

	if ((changed & GLS_MASK_BLEND) != 0)
		rb_state_debug.last_blend_owner = owner;
	if ((changed & (GLS_NO_ZTEST | GLS_NO_ZWRITE)) != 0)
		rb_state_debug.last_depth_owner = owner;
	if ((changed & GLS_MASK_CULL) != 0)
		rb_state_debug.last_cull_owner = owner;
	if ((glIsEnabled (GL_STENCIL_TEST) ? 1 : 0) != (stencil_enable ? 1 : 0))
		rb_state_debug.last_stencil_owner = owner;
	#endif

	GL_SetState (gl_mask);
	RB_StencilTestWithOwner (stencil_enable, owner);
}

void RB_UseProgram_Owner (GLuint program, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_program_owner = owner;
	#endif

	GL_UseProgram (program);
}

qboolean RB_BindTexture_Owner (GLenum texunit, gltexture_t *texture, const char *owner)
{
	#if RB_DEBUG_STATE
	int texindex = (int)(texunit - GL_TEXTURE0);
	if (texindex >= 0 && texindex < (int)countof (rb_state_debug.last_texture_owner))
		rb_state_debug.last_texture_owner[texindex] = owner;
	#endif

	return GL_Bind (texunit, texture);
}

void RB_BindFramebuffer_Owner (GLenum target, GLuint framebuffer, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_fbo_owner = owner;
	#endif

	GL_BindFramebufferFunc (target, framebuffer);
}

void RB_BindVertexArray_Owner (GLuint array, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_vao_owner = owner;
	#endif

	GL_BindVertexArrayFunc (array);
}

void RB_BindBuffer_Owner (GLenum target, GLuint buffer, const char *owner)
{
	#if RB_DEBUG_STATE
	if (target == GL_ARRAY_BUFFER)
		rb_state_debug.last_array_buffer_owner = owner;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
		rb_state_debug.last_element_array_buffer_owner = owner;
	#endif

	GL_BindBufferFunc (target, buffer);
}

void RB_BindSampler_Owner (GLuint unit, GLuint sampler, const char *owner)
{
	#if RB_DEBUG_STATE
	if (unit < countof (rb_state_debug.last_sampler_owner))
		rb_state_debug.last_sampler_owner[unit] = owner;
	#endif

	GL_BindSamplerFunc (unit, sampler);
}


void RB_ColorMask_Owner (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_color_mask_owner = owner;
	#endif

	glColorMask (red, green, blue, alpha);
}

void RB_Scissor_Owner (GLint x, GLint y, GLsizei width, GLsizei height, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_scissor_owner = owner;
	#endif

	glScissor (x, y, width, height);
}

void RB_EnableFramebufferSRGB_Owner (qboolean enable, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_framebuffer_srgb_owner = owner;
	#endif

	if (enable)
		glEnable (GL_FRAMEBUFFER_SRGB);
	else
		glDisable (GL_FRAMEBUFFER_SRGB);
}

void RB_PolygonMode_Owner (GLenum face, GLenum mode, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_polygon_mode_owner = owner;
	#endif

	glPolygonMode (face, mode);
}

void RB_StencilFunc_Owner (GLenum func, GLint ref, GLuint mask, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_stencil_owner = owner;
	#endif

	glStencilFunc (func, ref, mask);
}

void RB_StencilOp_Owner (GLenum sfail, GLenum dpfail, GLenum dppass, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_stencil_owner = owner;
	#endif

	glStencilOp (sfail, dpfail, dppass);
}

void RB_StencilMask_Owner (GLuint mask, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_stencil_owner = owner;
	#endif

	glStencilMask (mask);
}

void RB_StencilTest_Owner (qboolean enable, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_stencil_owner = owner;
	#endif

	if (enable)
		glEnable (GL_STENCIL_TEST);
	else
		glDisable (GL_STENCIL_TEST);
}

void RB_TexParameteri_Owner (GLenum target, GLenum pname, GLint param, const char *owner)
{
	(void)owner;
	glTexParameteri (target, pname, param);
}

void RB_DrawArrays (GLenum mode, GLint first, GLsizei count)
{
	glDrawArrays (mode, first, count);
}

void RB_DrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	glDrawElements (mode, count, type, indices);
}

void RB_Clear (GLbitfield mask)
{
	glClear (mask);
}

void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	glViewport (x, y, width, height);
}

void RB_Scissor (GLint x, GLint y, GLsizei width, GLsizei height)
{
	RB_ScissorWithOwner (x, y, width, height, "RB_Scissor");
}

/* Transition helper: temporarily allowed for legacy stage/material code. */
void RB_DepthFunc (GLenum func)
{
	glDepthFunc (func);
}

/* Transition helper: temporarily allowed for legacy stage/material code. */
void RB_BlendFunc (GLenum sfactor, GLenum dfactor)
{
	glBlendFunc (sfactor, dfactor);
}

void RB_DrawBuffer_Owner (GLenum buf, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_draw_buffer_owner = owner;
	#endif

	glDrawBuffer (buf);
}

void RB_ReadBuffer_Owner (GLenum src, const char *owner)
{
	#if RB_DEBUG_STATE
	rb_state_debug.last_read_buffer_owner = owner;
	#endif

	glReadBuffer (src);
}

void RB_FogVol_CaptureStateSnapshot (rb_fogvol_gl_snapshot_t *snapshot)
{
	int i;

	if (!snapshot)
		return;

	memset (snapshot, 0, sizeof (*snapshot));
	glGetIntegerv (GL_VIEWPORT, snapshot->viewport);
	snapshot->scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, snapshot->scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &snapshot->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &snapshot->read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &snapshot->draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &snapshot->read_buffer);
	snapshot->num_draw_buffers = 0;
	for (i = 0; i < RB_FOGVOL_MAX_DRAW_BUFFERS; ++i)
		snapshot->draw_buffers[i] = GL_NONE;
	{
		GLint max_draw_buffers = 0;
		glGetIntegerv (GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
		snapshot->num_draw_buffers = q_min (max_draw_buffers, RB_FOGVOL_MAX_DRAW_BUFFERS);
		for (i = 0; i < snapshot->num_draw_buffers; ++i)
			glGetIntegerv (GL_DRAW_BUFFER0 + i, &snapshot->draw_buffers[i]);
	}
	glGetIntegerv (GL_CURRENT_PROGRAM, &snapshot->program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &snapshot->vao);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &snapshot->active_texture);
	snapshot->blend = glIsEnabled (GL_BLEND);
	glGetIntegerv (GL_BLEND_SRC_RGB, &snapshot->blend_src_rgb);
	glGetIntegerv (GL_BLEND_DST_RGB, &snapshot->blend_dst_rgb);
	glGetIntegerv (GL_BLEND_EQUATION_RGB, &snapshot->blend_equation_rgb);
	snapshot->depth_test = glIsEnabled (GL_DEPTH_TEST);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &snapshot->depth_writemask);
	glGetBooleanv (GL_COLOR_WRITEMASK, snapshot->color_writemask);
	snapshot->cull_face = glIsEnabled (GL_CULL_FACE);
	glGetIntegerv (GL_POLYGON_MODE, snapshot->polygon_mode);
	glGetFloatv (GL_COLOR_CLEAR_VALUE, snapshot->color_clear_value);
	snapshot->framebuffer_srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	snapshot->dither = glIsEnabled (GL_DITHER);
	snapshot->multisample = glIsEnabled (GL_MULTISAMPLE);
}

void RB_FogVol_RestoreStateSnapshot (const rb_fogvol_gl_snapshot_t *snapshot)
{
	if (!snapshot)
		return;

	RB_BindFramebufferWithOwner (GL_DRAW_FRAMEBUFFER, (GLuint)snapshot->draw_fbo, "RB_FogVol_RestoreStateSnapshot");
	RB_BindFramebufferWithOwner (GL_READ_FRAMEBUFFER, (GLuint)snapshot->read_fbo, "RB_FogVol_RestoreStateSnapshot");
	RB_DrawBufferWithOwner ((GLenum)snapshot->draw_buffer, "RB_FogVol_RestoreStateSnapshot");
	RB_ReadBufferWithOwner ((GLenum)snapshot->read_buffer, "RB_FogVol_RestoreStateSnapshot");
	RB_Viewport (snapshot->viewport[0], snapshot->viewport[1], snapshot->viewport[2], snapshot->viewport[3]);
	if (snapshot->scissor_test)
		GL_SetScissorEnabled (true);
	else
		GL_SetScissorEnabled (false);
	RB_ScissorWithOwner (snapshot->scissor_box[0], snapshot->scissor_box[1], snapshot->scissor_box[2], snapshot->scissor_box[3], "RB_FogVol_RestoreStateSnapshot");
	RB_UseProgramWithOwner ((GLuint)snapshot->program, "RB_FogVol_RestoreStateSnapshot");
	RB_BindVertexArrayWithOwner ((GLuint)snapshot->vao, "RB_FogVol_RestoreStateSnapshot");
	GL_ActiveTextureFunc ((GLenum)snapshot->active_texture);
	RB_EnableFramebufferSRGBWithOwner (snapshot->framebuffer_srgb, "RB_FogVol_RestoreStateSnapshot");
}

void RB_SetPassSetupHook (rb_pass_setup_hook_t hook)
{
	rb_pass_setup_hook = hook;
}

void RB_BeginPass (rb_pass_t pass)
{
	rb_pass_baseline_gl_t baseline_gl;

	if (pass < 0 || pass >= PASS_COUNT)
		pass = PASS_WORLD_OPAQUE;

	if (rb_pass_active)
		RB_EndPass ();

	baseline_gl = RB_BuildPassBaselineGL (pass);

	#if RB_DEBUG_STATE
	if (r_gl_state_validate.value > 0.f)
	{
		rb_gl_state_snapshot_t incoming_state;
		RB_CaptureGLStateSnapshot (&incoming_state);
		RB_LogIncomingStateDiff (pass, &baseline_gl, &incoming_state);
	}
	#endif

	GL_BeginGroup (rb_pass_info[pass].debug_name);
	RB_SetState (rb_pass_info[pass].baseline_state);
	RB_ApplyPassBaselineGL (&baseline_gl);

	if (rb_pass_setup_hook)
		rb_pass_setup_hook (pass);

	rb_current_pass = pass;
	rb_pass_active = true;
}

const char *RB_DebugStateOwnersString (void)
{
#if RB_DEBUG_STATE
	static char info[3072];
	char segment[96];
	int i;

	q_snprintf (info, sizeof (info),
		"pass=%s owner(blend=%s depth=%s cull=%s prog=%s",
		rb_pass_info[rb_current_pass].debug_name,
		RB_DebugOwnerOrUnknown (rb_state_debug.last_blend_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_depth_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_cull_owner),
		RB_DebugOwnerOrUnknown (rb_state_debug.last_program_owner));

	for (i = 0; i < RB_VALIDATE_TEXTURE_UNIT_LIMIT; ++i)
	{
		q_snprintf (segment, sizeof (segment), " tex%d=%s", i, RB_DebugOwnerOrUnknown (rb_state_debug.last_texture_owner[i]));
		q_strlcat (info, segment, sizeof (info));
	}

	q_strlcat (info, " fbo=", sizeof (info));
	q_strlcat (info, RB_DebugOwnerOrUnknown (rb_state_debug.last_fbo_owner), sizeof (info));
	q_strlcat (info, " vao=", sizeof (info));
	q_strlcat (info, RB_DebugOwnerOrUnknown (rb_state_debug.last_vao_owner), sizeof (info));
	q_strlcat (info, " arrbuf=", sizeof (info));
	q_strlcat (info, RB_DebugOwnerOrUnknown (rb_state_debug.last_array_buffer_owner), sizeof (info));
	q_strlcat (info, " elembuf=", sizeof (info));
	q_strlcat (info, RB_DebugOwnerOrUnknown (rb_state_debug.last_element_array_buffer_owner), sizeof (info));

	for (i = 0; i < RB_VALIDATE_TEXTURE_UNIT_LIMIT; ++i)
	{
		q_snprintf (segment, sizeof (segment), " samp%d=%s", i, RB_DebugOwnerOrUnknown (rb_state_debug.last_sampler_owner[i]));
		q_strlcat (info, segment, sizeof (info));
	}

	q_snprintf (segment, sizeof (segment), " colormask=%s", RB_DebugOwnerOrUnknown (rb_state_debug.last_color_mask_owner));
	q_strlcat (info, segment, sizeof (info));
	q_snprintf (segment, sizeof (segment), " scissor=%s", RB_DebugOwnerOrUnknown (rb_state_debug.last_scissor_owner));
	q_strlcat (info, segment, sizeof (info));
	q_snprintf (segment, sizeof (segment), " stencil=%s", RB_DebugOwnerOrUnknown (rb_state_debug.last_stencil_owner));
	q_strlcat (info, segment, sizeof (info));
	q_snprintf (segment, sizeof (segment), " polygon=%s", RB_DebugOwnerOrUnknown (rb_state_debug.last_polygon_mode_owner));
	q_strlcat (info, segment, sizeof (info));
	q_snprintf (segment, sizeof (segment), " fbo_srgb=%s", RB_DebugOwnerOrUnknown (rb_state_debug.last_framebuffer_srgb_owner));
	q_strlcat (info, segment, sizeof (info));
	q_snprintf (segment, sizeof (segment), " drawbuf=%s", RB_DebugOwnerOrUnknown (rb_state_debug.last_draw_buffer_owner));
	q_strlcat (info, segment, sizeof (info));
	q_snprintf (segment, sizeof (segment), " readbuf=%s)", RB_DebugOwnerOrUnknown (rb_state_debug.last_read_buffer_owner));
	q_strlcat (info, segment, sizeof (info));

	return info;
#else
	return "pass=<tracker disabled>";
#endif
}

void RB_EndPass (void)
{
	if (!rb_pass_active)
		return;

	#if RB_DEBUG_STATE
	if (r_gl_state_validate.value > 0.f)
		RB_ValidatePassExit (rb_current_pass);
	#endif

	GL_EndGroup ();
	rb_pass_active = false;
}

qboolean RB_PassActive (void)
{
	return rb_pass_active;
}

rb_pass_t RB_CurrentPass (void)
{
	return rb_current_pass;
}
