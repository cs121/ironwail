/*
Copyright (C) 2024

This file is part of Ironwail.
*/

#include "quakedef.h"
#include "renderer_backend_gl.h"

#include <assert.h>
#include <SDL_stdinc.h>

typedef struct rb_gl_tex_s
{
	GLuint id;
	GLenum target;
	int width;
	int height;
	int mip_levels;
	rb_tex_format_t format;
} rb_gl_tex_t;

typedef struct rb_gl_buf_s
{
	GLuint id;
	GLenum target;
	size_t size;
	rb_buffer_type_t type;
	rb_buffer_usage_t usage;
} rb_gl_buf_t;

typedef struct rb_gl_fbo_s
{
	GLuint id;
	rb_tex_t color;
	rb_tex_t depth_stencil;
} rb_gl_fbo_t;

static rb_caps_t rb_gl_caps = {0, 0};
static cvar_t r_backend_gl_errors = {"r_backend_gl_errors", "0", CVAR_NONE};

/*
Buffer/mapping callsite inventory (outside renderer_backend_*):
Frontend files:
- gl_rmisc.c: GL_ClearBufferBindings (GL_BindBufferFunc)
- gl_rmisc.c: GL_AllocFrameResources (GL_BindBuffer, GL_BufferStorageFunc, GL_BufferDataFunc, RB_BufferMapRange -> RB_BufferUnmap)
- gl_rmisc.c: GL_DeleteFrameResources (RB_BufferUnmap)
- gl_rmisc.c: GL_Upload (GL_BindBuffer, GL_BufferSubDataFunc)
- gl_texmgr.c: TexMgr_DestroyLightmapUploadBuffer (RB_BufferUnmap)
- gl_texmgr.c: TexMgr_EnsureLightmapUploadBuffer (GL_BindBuffer, GL_BufferStorageFunc, RB_BufferMapRange)
- gl_texmgr.c: GLPalette_UpdateLookupTable (GL_BindBufferRange, GL_BufferSubDataFunc)
- r_sprite.c: R_FlushSpriteInstances (GL_BindBuffer)
- gl_draw.c: Draw_Flush (GL_BindBuffer)
- r_godrayvol.c: R_GodrayVolume_Render (GL_BindBuffer)
- r_decals.c: R_FlushDecalBatch (GL_BindBuffer)
- r_fogvol.c: R_FogVol_Render (GL_BindBufferRange)
- r_alias.c: R_DrawAliasModel_Real (GL_BindBuffer, GL_BindBuffersRange)
- gl_sky.c: Sky_DrawSkyBox (GL_BindBuffer)
- r_world.c: R_MarkVisSurfaces (GL_BindBufferRange)
- r_world.c: R_FlushBModelCalls (GL_BindBufferRange, GL_BindBuffer)
- r_world.c: R_DrawBrushModels_MaterialStages (GL_BindBufferRange)
- gl_rlight.c: R_PushDlights (GL_BindBufferRange)
- gl_rmain.c: GL_PostProcess (GL_BindBufferRange)
- gl_rmain.c: R_UploadFrameData (GL_BindBufferRange)
- gl_rmain.c: R_FlushDebugGeometry (GL_BindBuffer)
- gl_rmain.c: R_DrawDLightPass (GL_BindBufferRange)
- r_part.c: R_FlushParticleBatch (GL_BindBuffer)
Backend files:
- renderer_backend_gl.c: RB_GL_MapBufferRange/RB_GL_UnmapBuffer, RBGL_BindBuffer, RBGL_BufferData
- renderer_backend_null.c: RB_NULL_BindBuffer, RB_NULL_BufferData, RB_NULL_BufferMapRange
*/

#ifndef NDEBUG
typedef struct rb_gl_map_state_s
{
	GLuint id;
	int mapped;
} rb_gl_map_state_t;

#define RBGL_MAP_TRACK_MAX 256
static rb_gl_map_state_t rbgl_map_states[RBGL_MAP_TRACK_MAX];

static rb_gl_map_state_t *RBGL_MapStateLookup(GLuint id, qboolean create)
{
	int i;
	rb_gl_map_state_t *free_slot = NULL;

	for (i = 0; i < RBGL_MAP_TRACK_MAX; ++i)
	{
		if (rbgl_map_states[i].id == id)
			return &rbgl_map_states[i];
		if (!rbgl_map_states[i].id && !free_slot)
			free_slot = &rbgl_map_states[i];
	}

	if (create && free_slot)
	{
		free_slot->id = id;
		free_slot->mapped = 0;
		return free_slot;
	}

	return NULL;
}

static void RBGL_MapStateOnMap(GLuint id, const char *tag, const char *target_name)
{
	rb_gl_map_state_t *entry = RBGL_MapStateLookup(id, true);
	if (!entry)
	{
		Sys_Error("RB_GL_MapBufferRange: map state table full buffer=%u tag=%s target=%s",
			id,
			tag ? tag : "(null)",
			target_name);
	}
	if (entry->mapped)
	{
		Sys_Error("RB_GL_MapBufferRange: buffer already tracked as mapped buffer=%u tag=%s target=%s",
			id,
			tag ? tag : "(null)",
			target_name);
	}
	entry->mapped = 1;
}

static void RBGL_MapStateOnUnmap(GLuint id, const char *tag, const char *target_name)
{
	rb_gl_map_state_t *entry = RBGL_MapStateLookup(id, false);
	if (!entry || !entry->mapped)
	{
		Sys_Error("RB_GL_UnmapBuffer: buffer not tracked as mapped buffer=%u tag=%s target=%s",
			id,
			tag ? tag : "(null)",
			target_name);
	}
}

static void RBGL_MapStateSetUnmapped(GLuint id)
{
	rb_gl_map_state_t *entry = RBGL_MapStateLookup(id, false);
	if (!entry)
		return;
	entry->mapped = 0;
}

static void RBGL_MapStateClear(GLuint id)
{
	rb_gl_map_state_t *entry = RBGL_MapStateLookup(id, false);
	if (!entry)
		return;
	entry->id = 0;
	entry->mapped = 0;
}
#endif

static void RBGL_CheckError(const char *label)
{
	unsigned int err;

	if (!r_backend_gl_errors.value)
		return;

	err = glGetError();
	if (err != GL_NO_ERROR)
		Con_DPrintf("RBGL_CheckError: %s (0x%x)\n", label, err);
}

static void RBGL_Enable(int cap)
{
	glEnable (cap);
}

static void RBGL_Disable(int cap)
{
	glDisable (cap);
}

static void RBGL_BlendFunc(int sfactor, int dfactor)
{
	glBlendFunc (sfactor, dfactor);
}

static void RBGL_CullFace(int mode)
{
	glCullFace (mode);
}

static void RBGL_DepthMask(int flag)
{
	glDepthMask (flag);
}

static void RBGL_Viewport(int x, int y, int width, int height)
{
	glViewport (x, y, width, height);
}

static void RBGL_Scissor(int x, int y, int width, int height)
{
	glScissor (x, y, width, height);
}

static void RBGL_BindFramebuffer(int target, unsigned int framebuffer)
{
	GL_BindFramebufferFunc(target, framebuffer);
}

static void RBGL_BindTexture(int target, unsigned int texture)
{
	glBindTexture(target, texture);
}

static void RBGL_GenBuffers(int n, unsigned int *buffers)
{
	GL_GenBuffersFunc (n, buffers);
}

static void RBGL_DeleteBuffers(int n, const unsigned int *buffers)
{
#ifndef NDEBUG
	int i;
#endif

	GL_DeleteBuffersFunc (n, buffers);

#ifndef NDEBUG
	for (i = 0; i < n; ++i)
		RBGL_MapStateClear(buffers[i]);
#endif
}

static void RBGL_BindBuffer(int target, unsigned int buffer)
{
	GL_BindBufferFunc (target, buffer);
}

static void RBGL_BufferData(int target, size_t size, const void *data, int usage)
{
	GL_BufferDataFunc (target, size, data, usage);
}

static const char *RBGL_TargetName(int target)
{
	switch (target)
	{
	case GL_ARRAY_BUFFER:
		return "GL_ARRAY_BUFFER";
	case GL_ELEMENT_ARRAY_BUFFER:
		return "GL_ELEMENT_ARRAY_BUFFER";
	case GL_UNIFORM_BUFFER:
		return "GL_UNIFORM_BUFFER";
	case GL_SHADER_STORAGE_BUFFER:
		return "GL_SHADER_STORAGE_BUFFER";
	case GL_PIXEL_UNPACK_BUFFER:
		return "GL_PIXEL_UNPACK_BUFFER";
	case GL_DRAW_INDIRECT_BUFFER:
		return "GL_DRAW_INDIRECT_BUFFER";
	default:
		return "UNKNOWN_TARGET";
	}
}

static int RBGL_TargetBindingEnum(int target)
{
	switch (target)
	{
	case GL_ARRAY_BUFFER:
		return GL_ARRAY_BUFFER_BINDING;
	case GL_ELEMENT_ARRAY_BUFFER:
		return GL_ELEMENT_ARRAY_BUFFER_BINDING;
	case GL_UNIFORM_BUFFER:
		return GL_UNIFORM_BUFFER_BINDING;
	case GL_SHADER_STORAGE_BUFFER:
		return GL_SHADER_STORAGE_BUFFER_BINDING;
	case GL_PIXEL_UNPACK_BUFFER:
		return GL_PIXEL_UNPACK_BUFFER_BINDING;
	case GL_DRAW_INDIRECT_BUFFER:
		return GL_DRAW_INDIRECT_BUFFER_BINDING;
	default:
		return 0;
	}
}

void *RB_GL_MapBufferRange(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield flags, GLsizeiptr expected_total_size)
{
	GLenum err;
	int binding_enum;
	GLint binding = 0;
	GLint mapped = 0;
	GLint64 buffer_size = 0;
	GLsizeiptr alloc_size = expected_total_size;
	void *ptr;
	const char *target_name = RBGL_TargetName(target);
	Uint32 thread_id = SDL_ThreadID();
	void *context = SDL_GL_GetCurrentContext();
	const char *map_func_kind = "core";

	if (!buffer)
	{
		Con_Printf("RB_GL_MapBufferRange: invalid buffer=0 for target=%s(0x%04X) expected_size=%lld\n",
			target_name,
			target,
			(long long)alloc_size);
		return NULL;
	}
	if (offset < 0 || length <= 0)
	{
		Con_Printf("RB_GL_MapBufferRange: invalid range offset=%lld length=%lld target=%s buffer=%u expected_size=%lld\n",
			(long long)offset,
			(long long)length,
			target_name,
			buffer,
			(long long)alloc_size);
		return NULL;
	}

	if (expected_total_size <= 0)
	{
		Sys_Error("RB_GL_MapBufferRange: invalid expected_total_size target=%s(0x%04X) buffer=%u offset=%lld length=%lld expected=%lld flags=0x%08X thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			(long long)offset,
			(long long)length,
			(long long)expected_total_size,
			flags,
			thread_id,
			context);
	}
	if ((uint64_t)expected_total_size < (uint64_t)offset + (uint64_t)length)
	{
		Sys_Error("RB_GL_MapBufferRange: expected_total_size too small target=%s(0x%04X) buffer=%u offset=%lld length=%lld expected=%lld flags=0x%08X thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			(long long)offset,
			(long long)length,
			(long long)expected_total_size,
			flags,
			thread_id,
			context);
	}

	assert(buffer != 0);

	while (glGetError() != GL_NO_ERROR)
	{
	}

	if (!GL_MapBufferRangeFunc)
	{
		Sys_Error("RB_GL_MapBufferRange: MapBufferRange function pointer NULL target=%s(0x%04X) buffer=%u offset=%lld length=%lld expected=%lld flags=0x%08X thread=%u ctx=%p func=%p kind=%s",
			target_name,
			target,
			buffer,
			(long long)offset,
			(long long)length,
			(long long)expected_total_size,
			flags,
			thread_id,
			context,
			(void *)GL_MapBufferRangeFunc,
			map_func_kind);
	}
	Con_Printf("RB_GL_MapBufferRange: MapBufferRangeFunc=%p kind=%s\n", (void *)GL_MapBufferRangeFunc, map_func_kind);

	GL_BindBuffer(target, buffer);

	binding_enum = RBGL_TargetBindingEnum(target);
	if (binding_enum)
		glGetIntegerv(binding_enum, &binding);
	Con_Printf("RB_GL_MapBufferRange: binding query target=%s(0x%04X) enum=0x%04X binding=%d buffer=%u\n",
		target_name,
		target,
		binding_enum,
		binding,
		buffer);
	GL_GetBufferParameteri64vFunc(target, GL_BUFFER_SIZE, &buffer_size);
	GL_GetBufferParameterivFunc(target, GL_BUFFER_MAPPED, &mapped);

	if (binding_enum && binding == 0)
	{
		Sys_Error("RB_GL_MapBufferRange: binding is zero target=%s(0x%04X) buffer=%u offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			(long long)offset,
			(long long)length,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			mapped,
			thread_id,
			context);
	}
	if (binding_enum && (GLuint)binding != buffer)
	{
		Sys_Error("RB_GL_MapBufferRange: binding mismatch target=%s(0x%04X) buffer=%u bound=%d offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			binding,
			(long long)offset,
			(long long)length,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			mapped,
			thread_id,
			context);
	}
	if (mapped)
	{
		Sys_Error("RB_GL_MapBufferRange: buffer already mapped target=%s(0x%04X) buffer=%u bound=%d offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			binding,
			(long long)offset,
			(long long)length,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			mapped,
			thread_id,
			context);
	}
	if (buffer_size <= 0)
	{
		if (alloc_size <= 0)
			alloc_size = length;

		Con_Printf("RB_GL_MapBufferRange: buffer has no storage target=%s(0x%04X) buffer=%u bound=%d size=%lld expected=%lld flags=0x%08X thread=%u ctx=%p\n",
			target_name,
			target,
			buffer,
			binding,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			thread_id,
			context);

		GL_BufferDataFunc(target, alloc_size, NULL, GL_STREAM_DRAW);
		err = glGetError();
		Con_Printf("RB_GL_MapBufferRange: glBufferData err=0x%04X target=%s(0x%04X) buffer=%u expected=%lld\n",
			err,
			target_name,
			target,
			buffer,
			(long long)alloc_size);
		GL_GetBufferParameteri64vFunc(target, GL_BUFFER_SIZE, &buffer_size);
		Con_Printf("RB_GL_MapBufferRange: buffer size after alloc target=%s(0x%04X) buffer=%u size=%lld expected=%lld\n",
			target_name,
			target,
			buffer,
			(long long)buffer_size,
			(long long)alloc_size);
		if (buffer_size <= 0)
		{
			Sys_Error("RB_GL_MapBufferRange: buffer storage allocation failed target=%s(0x%04X) buffer=%u bound=%d size=%lld expected=%lld flags=0x%08X err=0x%04X thread=%u ctx=%p",
				target_name,
				target,
				buffer,
				binding,
				(long long)buffer_size,
				(long long)alloc_size,
				flags,
				err,
				thread_id,
				context);
		}
	}
	if ((uint64_t)offset + (uint64_t)length > (uint64_t)buffer_size)
	{
		Sys_Error("RB_GL_MapBufferRange: range exceeds buffer target=%s(0x%04X) buffer=%u bound=%d offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			binding,
			(long long)offset,
			(long long)length,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			mapped,
			thread_id,
			context);
	}

	if (binding_enum)
		assert(binding != 0);
	assert((uint64_t)buffer_size >= (uint64_t)offset + (uint64_t)length);

	Con_Printf("RB_GL_MapBufferRange: target=%s(0x%04X) buffer=%u bound=%d size=%lld offset=%lld length=%lld expected=%lld flags=0x%08X\n",
		target_name,
		target,
		buffer,
		binding,
		(long long)buffer_size,
		(long long)offset,
		(long long)length,
		(long long)alloc_size,
		flags);

	while (glGetError() != GL_NO_ERROR)
	{
	}

	ptr = GL_MapBufferRangeFunc(target, (GLintptr)offset, (GLsizeiptr)length, flags);
	if (!ptr)
	{
		err = glGetError();
		Con_Printf("RB_GL_MapBufferRange: glMapBufferRange failed (err=0x%04X) target=%s(0x%04X) buffer=%u bound=%d offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p\n",
			err,
			target_name,
			target,
			buffer,
			binding,
			(long long)offset,
			(long long)length,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			mapped,
			thread_id,
			context);

		GL_BufferDataFunc(target, (GLsizeiptr)buffer_size, NULL, GL_STREAM_DRAW);
		while (glGetError() != GL_NO_ERROR)
		{
		}
		ptr = GL_MapBufferRangeFunc(target, (GLintptr)offset, (GLsizeiptr)length, flags);
		err = glGetError();
		Con_Printf("RB_GL_MapBufferRange: glMapBufferRange retry err=0x%04X target=%s(0x%04X) buffer=%u bound=%d offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p\n",
			err,
			target_name,
			target,
			buffer,
			binding,
			(long long)offset,
			(long long)length,
			(long long)buffer_size,
			(long long)alloc_size,
			flags,
			mapped,
			thread_id,
			context);
		if (!ptr)
		{
			Sys_Error("RB_GL_MapBufferRange: MapBufferRange failed after orphan (err=0x%04X) target=%s(0x%04X) buffer=%u bound=%d offset=%lld length=%lld size=%lld expected=%lld flags=0x%08X mapped=%d thread=%u ctx=%p",
				err,
				target_name,
				target,
				buffer,
				binding,
				(long long)offset,
				(long long)length,
				(long long)buffer_size,
				(long long)alloc_size,
				flags,
				mapped,
				thread_id,
				context);
		}
	}

#ifndef NDEBUG
	RBGL_MapStateOnMap(buffer, NULL, target_name);
#endif

	return ptr;
}

static void *RBGL_BufferMapRange(int target, unsigned int buffer, size_t offset, size_t length, unsigned int flags, size_t full_size, const char *label)
{
	void *ptr;

	if (full_size && (offset + length > full_size))
	{
		Sys_Error("%s: range exceeds expected size target=%s buffer=%u offset=%llu length=%llu size=%llu",
			label ? label : "RBGL_BufferMapRange",
			RBGL_TargetName(target),
			buffer,
			(unsigned long long)offset,
			(unsigned long long)length,
			(unsigned long long)full_size);
	}

	ptr = RB_GL_MapBufferRange((GLenum)target, (GLuint)buffer, (GLintptr)offset, (GLsizeiptr)length, (GLbitfield)flags, (GLsizeiptr)full_size);
	if (!ptr && label)
	{
		Con_Printf("%s: RB_GL_MapBufferRange returned NULL target=%s buffer=%u offset=%llu length=%llu\n",
			label,
			RBGL_TargetName(target),
			buffer,
			(unsigned long long)offset,
			(unsigned long long)length);
	}

	return ptr;
}

void RB_GL_UnmapBuffer(GLenum target, GLuint buffer, const char *tag)
{
	GLenum err;
	int binding_enum;
	GLint binding = 0;
	GLint mapped = 0;
	GLint64 buffer_size = 0;
	const char *target_name = RBGL_TargetName(target);
	Uint32 thread_id = SDL_ThreadID();
	void *context = SDL_GL_GetCurrentContext();

	if (!buffer)
	{
		Con_Printf("RB_GL_UnmapBuffer: invalid buffer=0 for target=%s(0x%04X) tag=%s\n",
			target_name,
			target,
			tag ? tag : "(null)");
		return;
	}

	while (glGetError() != GL_NO_ERROR)
	{
	}

	GL_BindBuffer(target, buffer);

	binding_enum = RBGL_TargetBindingEnum(target);
	if (binding_enum)
		glGetIntegerv(binding_enum, &binding);
	GL_GetBufferParameteri64vFunc(target, GL_BUFFER_SIZE, &buffer_size);
	GL_GetBufferParameterivFunc(target, GL_BUFFER_MAPPED, &mapped);

	if (binding_enum && binding == 0)
	{
		Sys_Error("RB_GL_UnmapBuffer: binding is zero target=%s(0x%04X) buffer=%u bound=%d size=%llu tag=%s thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			binding,
			(unsigned long long)buffer_size,
			tag ? tag : "(null)",
			thread_id,
			context);
	}
	if (binding_enum && (GLuint)binding != buffer)
	{
		Sys_Error("RB_GL_UnmapBuffer: binding mismatch target=%s(0x%04X) buffer=%u bound=%d size=%llu tag=%s thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			binding,
			(unsigned long long)buffer_size,
			tag ? tag : "(null)",
			thread_id,
			context);
	}
	if (!mapped)
	{
		Sys_Error("RB_GL_UnmapBuffer: buffer not mapped target=%s(0x%04X) buffer=%u bound=%d size=%llu tag=%s thread=%u ctx=%p",
			target_name,
			target,
			buffer,
			binding,
			(unsigned long long)buffer_size,
			tag ? tag : "(null)",
			thread_id,
			context);
	}

#ifndef NDEBUG
	RBGL_MapStateOnUnmap(buffer, tag, target_name);
#endif

	if (!GL_UnmapBufferFunc(target))
	{
		err = glGetError();
		Sys_Error("RB_GL_UnmapBuffer: glUnmapBuffer failed (err=0x%04X) target=%s(0x%04X) buffer=%u bound=%d size=%llu tag=%s thread=%u ctx=%p",
			err,
			target_name,
			target,
			buffer,
			binding,
			(unsigned long long)buffer_size,
			tag ? tag : "(null)",
			thread_id,
			context);
	}

#ifndef NDEBUG
	RBGL_MapStateSetUnmapped(buffer);
#endif
}

static void RBGL_BufferUnmap(int target, unsigned int buffer, const char *label)
{
	RB_GL_UnmapBuffer((GLenum)target, (GLuint)buffer, label);
}

static void RBGL_GenVertexArrays(int n, unsigned int *arrays)
{
	GL_GenVertexArraysFunc (n, arrays);
}

static void RBGL_DeleteVertexArrays(int n, const unsigned int *arrays)
{
	GL_DeleteVertexArraysFunc (n, arrays);
}

static void RBGL_BindVertexArray(unsigned int array)
{
	GL_BindVertexArrayFunc (array);
}

static void RBGL_Init(void)
{
	Cvar_RegisterVariable(&r_backend_gl_errors);
	Con_DPrintf("RBGL_Init: backend initialized.\n");
}

static void RBGL_Shutdown(void)
{
	Con_DPrintf("RBGL_Shutdown: backend shutdown.\n");
}

static void RBGL_BeginFrame(void)
{
	RBGL_CheckError("begin_frame");
}

static void RBGL_EndFrame(void)
{
	RBGL_CheckError("end_frame");
}

static void RBGL_Resize(int width, int height)
{
	(void)width;
	(void)height;
}

static void RBGL_NewMap(void)
{
}

static GLenum RBGL_TextureInternalFormat(rb_tex_format_t format)
{
	switch (format)
	{
	case RB_TEXFMT_RGBA8:
		return GL_RGBA8;
	case RB_TEXFMT_DEPTH24_STENCIL8:
		return GL_DEPTH24_STENCIL8;
	}
	return GL_RGBA8;
}

static GLenum RBGL_TextureFormat(rb_tex_format_t format)
{
	switch (format)
	{
	case RB_TEXFMT_RGBA8:
		return GL_RGBA;
	case RB_TEXFMT_DEPTH24_STENCIL8:
		return GL_DEPTH_STENCIL;
	}
	return GL_RGBA;
}

static GLenum RBGL_TextureType(rb_tex_format_t format)
{
	switch (format)
	{
	case RB_TEXFMT_RGBA8:
		return GL_UNSIGNED_BYTE;
	case RB_TEXFMT_DEPTH24_STENCIL8:
		return GL_UNSIGNED_INT_24_8;
	}
	return GL_UNSIGNED_BYTE;
}

static GLenum RBGL_BufferTarget(rb_buffer_type_t type)
{
	switch (type)
	{
	case RB_BUFFER_VERTEX:
		return GL_ARRAY_BUFFER;
	case RB_BUFFER_INDEX:
		return GL_ELEMENT_ARRAY_BUFFER;
	case RB_BUFFER_UNIFORM:
		return GL_UNIFORM_BUFFER;
	}
	return GL_ARRAY_BUFFER;
}

static GLenum RBGL_BufferUsage(rb_buffer_usage_t usage)
{
	switch (usage)
	{
	case RB_BUFFER_USAGE_STATIC:
		return GL_STATIC_DRAW;
	case RB_BUFFER_USAGE_DYNAMIC:
		return GL_DYNAMIC_DRAW;
	}
	return GL_STATIC_DRAW;
}

static rb_tex_t RBGL_CreateTexture(const rb_tex_desc_t *desc)
{
	rb_gl_tex_t *tex;
	GLenum internal_format;
	GLenum format;
	GLenum type;
	GLenum filter;
	int levels;

	assert(desc);
	tex = (rb_gl_tex_t *)Z_Malloc(sizeof(*tex));
	tex->target = GL_TEXTURE_2D;
	tex->width = desc->width;
	tex->height = desc->height;
	tex->mip_levels = desc->mip_levels > 0 ? desc->mip_levels : 1;
	tex->format = desc->format;

	internal_format = RBGL_TextureInternalFormat(desc->format);
	format = RBGL_TextureFormat(desc->format);
	type = RBGL_TextureType(desc->format);
	filter = desc->format == RB_TEXFMT_DEPTH24_STENCIL8 ? GL_NEAREST : GL_LINEAR;
	levels = tex->mip_levels - 1;

	glGenTextures(1, &tex->id);
	GL_BindNative(GL_TEXTURE0, tex->target, tex->id);
	glTexImage2D(tex->target, 0, internal_format, desc->width, desc->height, 0, format, type, desc->data);
	glTexParameteri(tex->target, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(tex->target, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(tex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex->target, GL_TEXTURE_MAX_LEVEL, levels);
	if (desc->format == RB_TEXFMT_DEPTH24_STENCIL8)
		glTexParameteri(tex->target, GL_TEXTURE_COMPARE_MODE, GL_NONE);

	return (rb_tex_t)tex;
}

static void RBGL_DestroyTexture(rb_tex_t tex)
{
	rb_gl_tex_t *gl_tex = (rb_gl_tex_t *)tex;

	assert(gl_tex);
	if (gl_tex->id)
		GL_DeleteNativeTexture(gl_tex->id);
	Z_Free(gl_tex);
}

static rb_buf_t RBGL_CreateBuffer(const rb_buf_desc_t *desc)
{
	rb_gl_buf_t *buf;
	GLenum target;
	GLenum usage;
	size_t size;

	assert(desc);
	buf = (rb_gl_buf_t *)Z_Malloc(sizeof(*buf));
	buf->type = desc->type;
	buf->usage = desc->usage;
	target = RBGL_BufferTarget(desc->type);
	usage = RBGL_BufferUsage(desc->usage);
	size = desc->size ? desc->size : desc->data_size;

	buf->target = target;
	buf->size = size;

	GL_GenBuffersFunc(1, &buf->id);
	GL_BindBufferFunc(target, buf->id);
	GL_BufferDataFunc(target, size, desc->data, usage);

	return (rb_buf_t)buf;
}

static void RBGL_DestroyBuffer(rb_buf_t buf)
{
	rb_gl_buf_t *gl_buf = (rb_gl_buf_t *)buf;

	assert(gl_buf);
	if (gl_buf->id)
	{
		GL_DeleteBuffersFunc(1, &gl_buf->id);
#ifndef NDEBUG
		RBGL_MapStateClear(gl_buf->id);
#endif
		gl_buf->id = 0;
	}
	Z_Free(gl_buf);
}

static rb_fbo_t RBGL_CreateFBO(const rb_fbo_desc_t *desc)
{
	rb_gl_fbo_t *fbo;
	GLenum draw_buffers[1];

	assert(desc);
	fbo = (rb_gl_fbo_t *)Z_Malloc(sizeof(*fbo));
	fbo->color = desc->color;
	fbo->depth_stencil = desc->depth_stencil;

	GL_GenFramebuffersFunc(1, &fbo->id);
	GL_BindFramebufferFunc(GL_FRAMEBUFFER, fbo->id);

	if (desc->color)
	{
		const rb_gl_tex_t *color_tex = (const rb_gl_tex_t *)desc->color;
		GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color_tex->target, color_tex->id, 0);
		draw_buffers[0] = GL_COLOR_ATTACHMENT0;
		GL_DrawBuffersFunc(1, draw_buffers);
	}
	else
	{
		glDrawBuffer(GL_NONE);
		glReadBuffer(GL_NONE);
	}

	if (desc->depth_stencil)
	{
		const rb_gl_tex_t *depth_tex = (const rb_gl_tex_t *)desc->depth_stencil;
		if (depth_tex->format == RB_TEXFMT_DEPTH24_STENCIL8)
			GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth_tex->target, depth_tex->id, 0);
		else
			GL_FramebufferTexture2DFunc(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depth_tex->target, depth_tex->id, 0);
	}

	GL_BindFramebufferFunc(GL_FRAMEBUFFER, 0);

	return (rb_fbo_t)fbo;
}

static void RBGL_DestroyFBO(rb_fbo_t fbo)
{
	rb_gl_fbo_t *gl_fbo = (rb_gl_fbo_t *)fbo;

	assert(gl_fbo);
	if (gl_fbo->id)
		GL_DeleteFramebuffersFunc(1, &gl_fbo->id);
	Z_Free(gl_fbo);
}

static void RBGL_Submit(const rb_draw_desc_t *desc)
{
	const rb_gl_buf_t *vertex_buf;
	const rb_gl_buf_t *index_buf;
	GLsizei index_count;
	GLsizei vertex_count;

	assert(desc);
	vertex_buf = (const rb_gl_buf_t *)desc->vertex_buffer;
	index_buf = (const rb_gl_buf_t *)desc->index_buffer;
	index_count = desc->index_count;
	vertex_count = desc->vertex_count;

	if (vertex_buf)
		GL_BindBufferFunc(GL_ARRAY_BUFFER, vertex_buf->id);
	if (index_buf)
		GL_BindBufferFunc(GL_ELEMENT_ARRAY_BUFFER, index_buf->id);

	if (index_buf && index_count > 0)
	{
		const size_t index_offset = (size_t)desc->first_index * sizeof(unsigned short);
		glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, (const void *)index_offset);
	}
	else if (vertex_count > 0)
	{
		glDrawArrays(GL_TRIANGLES, desc->first_vertex, vertex_count);
	}
}

static const rb_caps_t *RBGL_GetCaps(void)
{
	return &rb_gl_caps;
}

static void RBGL_DebugMarkerBegin(const char *label)
{
	if (label && *label)
		Con_DPrintf("RBGL_DebugMarkerBegin: %s\n", label);
}

static void RBGL_DebugMarkerEnd(void)
{
}

static const rb_backend_api_t rb_gl_api = {
	"OpenGL",
	RBGL_Init,
	RBGL_Shutdown,
	RBGL_BeginFrame,
	RBGL_EndFrame,
	RBGL_Resize,
	RBGL_NewMap,
	RBGL_CreateTexture,
	RBGL_DestroyTexture,
	RBGL_CreateBuffer,
	RBGL_DestroyBuffer,
	RBGL_CreateFBO,
	RBGL_DestroyFBO,
	RBGL_Submit,
	RBGL_Enable,
	RBGL_Disable,
	RBGL_BlendFunc,
	RBGL_CullFace,
	RBGL_DepthMask,
	RBGL_Viewport,
	RBGL_Scissor,
	RBGL_BindFramebuffer,
	RBGL_BindTexture,
	RBGL_GenBuffers,
	RBGL_DeleteBuffers,
	RBGL_BindBuffer,
	RBGL_BufferData,
	RBGL_BufferMapRange,
	RBGL_BufferUnmap,
	RBGL_GenVertexArrays,
	RBGL_DeleteVertexArrays,
	RBGL_BindVertexArray,
	RBGL_GetCaps,
	RBGL_DebugMarkerBegin,
	RBGL_DebugMarkerEnd
};

const rb_backend_api_t *RBGL_GetApi(void)
{
	return &rb_gl_api;
}
