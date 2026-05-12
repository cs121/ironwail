/*
 * Renderer-neutral texture handle declarations for core-facing headers.
 * Native GL texture IDs remain ref_gl-private.
 * Phase 2 note: render_texture_handle_t is the intended destination for
 * frontend-visible texture references, but gltexture_t has not been fully
 * migrated and remains the public legacy bridge.
 */

#ifndef RENDER_TEXTURE_HANDLES_H
#define RENDER_TEXTURE_HANDLES_H

#include "q_stdinc.h"

typedef uint32_t render_texture_handle_t;
typedef uint64_t render_texture_bindless_handle_t;

#define RENDER_TEXTURE_HANDLE_INVALID ((render_texture_handle_t)0u)
#define RENDER_TEXTURE_BINDLESS_HANDLE_INVALID ((render_texture_bindless_handle_t)0ull)

/* Renderer-neutral source format enum used by image loading and upload paths. */
enum srcformat
{
	SRC_INDEXED,
	SRC_LIGHTMAP,
	SRC_RGBA
};

typedef uintptr_t src_offset_t;

/* Forward declarations only; full definitions stay in GL-private headers. */
struct gltexture_s;
typedef struct gltexture_s gltexture_t;

static inline qboolean R_TextureHandle_IsValid (render_texture_handle_t h)
{
	return h != RENDER_TEXTURE_HANDLE_INVALID;
}

static inline render_texture_handle_t R_TextureHandle_Invalid (void)
{
	return RENDER_TEXTURE_HANDLE_INVALID;
}

render_texture_handle_t R_TextureHandle_FromLegacyGLTexture (gltexture_t *tex);
gltexture_t *R_TextureHandle_ResolveLegacyGLTexture (render_texture_handle_t h);

#endif /* RENDER_TEXTURE_HANDLES_H */
