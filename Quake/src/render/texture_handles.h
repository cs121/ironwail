/*
 * Renderer-neutral texture handle declarations for core-facing headers.
 * Native GL texture IDs remain ref_gl-private.
 */

#ifndef RENDER_TEXTURE_HANDLES_H
#define RENDER_TEXTURE_HANDLES_H

#include "q_stdinc.h"

typedef enum
{
	TEXPREF_NONE			= 0x0000,
	TEXPREF_MIPMAP			= 0x0001,	// generate mipmaps
	// TEXPREF_NEAREST and TEXPREF_LINEAR aren't supposed to be ORed with TEX_MIPMAP
	TEXPREF_LINEAR			= 0x0002,	// force linear
	TEXPREF_NEAREST			= 0x0004,	// force nearest
	TEXPREF_ALPHA			= 0x0008,	// allow alpha
	TEXPREF_PAD				= 0x0010,	// allow padding
	TEXPREF_PERSIST			= 0x0020,	// never free
	TEXPREF_OVERWRITE		= 0x0040,	// overwrite existing same-name texture
	TEXPREF_NOPICMIP		= 0x0080,	// always load full-sized
	TEXPREF_FULLBRIGHT		= 0x0100,	// use fullbright mask palette
	TEXPREF_NOBRIGHT		= 0x0200,	// use nobright mask palette
	TEXPREF_CONCHARS		= 0x0400,	// use conchars palette
	TEXPREF_ARRAY			= 0x0800,	// array texture
	TEXPREF_CUBEMAP			= 0x1000,	// cubemap texture
	TEXPREF_BINDLESS		= 0x2000,	// enable bindless usage
	TEXPREF_ALPHABRIGHT		= 0x4000,	// use palette with lighting mask in alpha channel (0=fullbright, 1=lit)
	TEXPREF_CLAMP			= 0x8000,	// clamp UVs

	TEXPREF_SRGB			= 0x10000,	// upload texture in sRGB format

	TEXPREF_HASALPHA		= (TEXPREF_ALPHA|TEXPREF_ALPHABRIGHT), // texture has alpha channel
} textureflags_t;

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

struct qmodel_s;
typedef struct qmodel_s qmodel_t;

typedef enum
{
	SOFTEMU_OFF,
	SOFTEMU_FINE,		// screen-space dither
	SOFTEMU_COARSE,		// world-space dither nearby, screen-space dither in the distance
	SOFTEMU_BANDED,		// no dithering

	SOFTEMU_NUMMODES,
} softemu_t;
extern softemu_t softemu;

typedef enum
{
	SOFTEMU_METRIC_NAIVE,
	SOFTEMU_METRIC_RIEMERSMA,
	SOFTEMU_METRIC_OKLAB,

	SOFTEMU_METRIC_COUNT,
	SOFTEMU_METRIC_INVALID = SOFTEMU_METRIC_COUNT,
} softemu_metric_t;

typedef struct
{
	int	magfilter;
	int	minfilter;
	const char  *name;
	const char  *uiname;
} glmode_t;
#define NUM_GLMODES 6
extern const glmode_t glmodes[NUM_GLMODES];

typedef struct
{
	int		mode;
	float	anisotropy;
	float	lodbias;
} texfilter_t;
extern texfilter_t gl_texfilter;

extern unsigned int d_8to24table[256];
extern unsigned int d_8to24table_fbright[256];
extern unsigned int d_8to24table_nobright[256];
extern unsigned int d_8to24table_conchars[256];

void TexMgr_Trace (const char *fmt, ...);
void TexMgr_FreeTexturesForOwner (qmodel_t *owner);
qboolean TexMgr_UsesFilterOverride (void);

#endif /* RENDER_TEXTURE_HANDLES_H */
