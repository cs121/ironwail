#ifndef RB_GL_H
#define RB_GL_H

#include "glquake.h"
#include "gl_texmgr.h"
#include "rb_pass.h"

/*
 * Build guard: wrappers are pass-through only during the initial migration.
 * Keep this set to 1 until backend behavior intentionally diverges.
 *
 * A2 hotpath rule: state ownership stays in RB_* wrappers. Direct GL state calls
 * are not allowed in A2 paths, except for explicit transition helpers listed in
 * render_backend.h (currently RB_DepthFunc/RB_BlendFunc).
 *
 * Debug guard: RB_DEBUG_STATE enables state-owner tracking + incoming state-diff
 * diagnostics. Default maps to debug builds (!defined(NDEBUG) || defined(_DEBUG)
 * || defined(DEBUG)); override at build-time if needed.
 */
#define RB_GL_PASSTHROUGH_ONLY 1

/* RB-local extension bit for RB_SetState: toggles GL_STENCIL_TEST. */
#define RB_GLS_STENCIL_TEST (1u << 31)

void RB_SetState_Owner (unsigned mask, const char *owner);
void RB_UseProgram_Owner (GLuint program, const char *owner);
qboolean RB_BindTexture_Owner (GLenum texunit, gltexture_t *texture, const char *owner);
void RB_BindFramebuffer_Owner (GLenum target, GLuint framebuffer, const char *owner);
void RB_BindVertexArray_Owner (GLuint array, const char *owner);
void RB_BindBuffer_Owner (GLenum target, GLuint buffer, const char *owner);
void RB_BindSampler_Owner (GLuint unit, GLuint sampler, const char *owner);
void RB_ColorMask_Owner (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha, const char *owner);
void RB_Scissor_Owner (GLint x, GLint y, GLsizei width, GLsizei height, const char *owner);
void RB_EnableFramebufferSRGB_Owner (qboolean enable, const char *owner);
void RB_PolygonMode_Owner (GLenum face, GLenum mode, const char *owner);
void RB_StencilFunc_Owner (GLenum func, GLint ref, GLuint mask, const char *owner);
void RB_StencilOp_Owner (GLenum sfail, GLenum dpfail, GLenum dppass, const char *owner);
void RB_StencilMask_Owner (GLuint mask, const char *owner);
void RB_StencilTest_Owner (qboolean enable, const char *owner);
void RB_TexParameteri_Owner (GLenum target, GLenum pname, GLint param, const char *owner);
void RB_DrawBuffer_Owner (GLenum buf, const char *owner);
void RB_ReadBuffer_Owner (GLenum src, const char *owner);
#define RB_SetState(mask) RB_SetState_Owner ((mask), __func__)
#define RB_UseProgram(program) RB_UseProgram_Owner ((program), __func__)
#define RB_BindTexture(texunit, texture) RB_BindTexture_Owner ((texunit), (texture), __func__)
#define RB_BindFramebuffer(target, framebuffer) RB_BindFramebuffer_Owner ((target), (framebuffer), __func__)
#define RB_BindVertexArray(array) RB_BindVertexArray_Owner ((array), __func__)
#define RB_BindBuffer(target, buffer) RB_BindBuffer_Owner ((target), (buffer), __func__)
#define RB_BindSampler(unit, sampler) RB_BindSampler_Owner ((unit), (sampler), __func__)
#define RB_ColorMask(red, green, blue, alpha) RB_ColorMask_Owner ((red), (green), (blue), (alpha), __func__)
#define RB_EnableFramebufferSRGB(enable) RB_EnableFramebufferSRGB_Owner ((enable), __func__)
#define RB_PolygonMode(face, mode) RB_PolygonMode_Owner ((face), (mode), __func__)
#define RB_StencilFunc(func, ref, mask) RB_StencilFunc_Owner ((func), (ref), (mask), __func__)
#define RB_StencilOp(sfail, dpfail, dppass) RB_StencilOp_Owner ((sfail), (dpfail), (dppass), __func__)
#define RB_StencilMask(mask) RB_StencilMask_Owner ((mask), __func__)
#define RB_StencilTest(enable) RB_StencilTest_Owner ((enable), __func__)
#define RB_TexParameteri(target, pname, param) RB_TexParameteri_Owner ((target), (pname), (param), __func__)
#define RB_DrawBuffer(buf) RB_DrawBuffer_Owner ((buf), __func__)
#define RB_ReadBuffer(src) RB_ReadBuffer_Owner ((src), __func__)
#define RB_SetStateWithOwner(mask, owner) RB_SetState_Owner ((mask), (owner))
#define RB_UseProgramWithOwner(program, owner) RB_UseProgram_Owner ((program), (owner))
#define RB_BindTextureWithOwner(texunit, texture, owner) RB_BindTexture_Owner ((texunit), (texture), (owner))
#define RB_BindFramebufferWithOwner(target, framebuffer, owner) RB_BindFramebuffer_Owner ((target), (framebuffer), (owner))
#define RB_BindVertexArrayWithOwner(array, owner) RB_BindVertexArray_Owner ((array), (owner))
#define RB_BindBufferWithOwner(target, buffer, owner) RB_BindBuffer_Owner ((target), (buffer), (owner))
#define RB_BindSamplerWithOwner(unit, sampler, owner) RB_BindSampler_Owner ((unit), (sampler), (owner))
#define RB_ColorMaskWithOwner(red, green, blue, alpha, owner) RB_ColorMask_Owner ((red), (green), (blue), (alpha), (owner))
#define RB_ScissorWithOwner(x, y, width, height, owner) RB_Scissor_Owner ((x), (y), (width), (height), (owner))
#define RB_EnableFramebufferSRGBWithOwner(enable, owner) RB_EnableFramebufferSRGB_Owner ((enable), (owner))
#define RB_PolygonModeWithOwner(face, mode, owner) RB_PolygonMode_Owner ((face), (mode), (owner))
#define RB_StencilFuncWithOwner(func, ref, mask, owner) RB_StencilFunc_Owner ((func), (ref), (mask), (owner))
#define RB_StencilOpWithOwner(sfail, dpfail, dppass, owner) RB_StencilOp_Owner ((sfail), (dpfail), (dppass), (owner))
#define RB_StencilMaskWithOwner(mask, owner) RB_StencilMask_Owner ((mask), (owner))
#define RB_StencilTestWithOwner(enable, owner) RB_StencilTest_Owner ((enable), (owner))
#define RB_TexParameteriWithOwner(target, pname, param, owner) RB_TexParameteri_Owner ((target), (pname), (param), (owner))
#define RB_DrawBufferWithOwner(buf, owner) RB_DrawBuffer_Owner ((buf), (owner))
#define RB_ReadBufferWithOwner(src, owner) RB_ReadBuffer_Owner ((src), (owner))
void RB_DrawArrays (GLenum mode, GLint first, GLsizei count);
void RB_DrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void RB_Clear (GLbitfield mask);
void RB_Viewport (GLint x, GLint y, GLsizei width, GLsizei height);
void RB_Scissor (GLint x, GLint y, GLsizei width, GLsizei height);
/* Transition helper: temporarily allowed for legacy stage/material code. */
void RB_DepthFunc (GLenum func);
/* Transition helper: temporarily allowed for legacy stage/material code. */
void RB_BlendFunc (GLenum sfactor, GLenum dfactor);

typedef void (*rb_pass_setup_hook_t) (rb_pass_t pass);
void RB_SetPassSetupHook (rb_pass_setup_hook_t hook);
void RB_BeginPass (rb_pass_t pass);
void RB_EndPass (void);
qboolean RB_PassActive (void);
rb_pass_t RB_CurrentPass (void);
const char *RB_DebugStateOwnersString (void);

#define RB_FOGVOL_MAX_DRAW_BUFFERS 8

typedef struct rb_fogvol_gl_snapshot_s
{
	GLint viewport[4];
	GLint scissor_box[4];
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint draw_buffers[RB_FOGVOL_MAX_DRAW_BUFFERS];
	GLint num_draw_buffers;
	GLint program;
	GLint vao;
	GLint active_texture;
	GLint blend_src_rgb;
	GLint blend_dst_rgb;
	GLint blend_equation_rgb;
	GLint polygon_mode[2];
	GLboolean scissor_test;
	GLboolean blend;
	GLboolean depth_test;
	GLboolean depth_writemask;
	GLboolean color_writemask[4];
	GLboolean cull_face;
	GLfloat color_clear_value[4];
	GLboolean framebuffer_srgb;
	GLboolean dither;
	GLboolean multisample;
} rb_fogvol_gl_snapshot_t;

void RB_FogVol_CaptureStateSnapshot (rb_fogvol_gl_snapshot_t *snapshot);
void RB_FogVol_RestoreStateSnapshot (const rb_fogvol_gl_snapshot_t *snapshot);

#endif
