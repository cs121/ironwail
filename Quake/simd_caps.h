#ifndef SIMD_CAPS_H
#define SIMD_CAPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
	int sse2;
	int ssse3;
	int sse41;
	int avx2;
} simd_caps_t;

enum
{
	SIMD_MODE_AUTO = 0,
	SIMD_MODE_SCALAR = 1,
	SIMD_MODE_SSE2 = 2,
	SIMD_MODE_SSSE3 = 3,
	SIMD_MODE_SSE41 = 4,
	SIMD_MODE_AVX2 = 5
};

simd_caps_t SIMD_GetCaps (void);
void SIMD_Init (void);
int SIMD_Mode (void);
qboolean SIMD_SupportsMode (int mode);

/* Shared CPU helpers for non-SIMD systems code. */
int CPU_GetCoreCount (void);
qboolean CPU_HasSSE2 (void);

/* RGBA/BGRA channel swap helpers used by upload/conversion loops. */
void swizzle_rgba_bgra_scalar (uint8_t *dst, const uint8_t *src, size_t n_pixels);
void swizzle_rgba_bgra_sse2 (uint8_t *dst, const uint8_t *src, size_t n_pixels);
void swizzle_rgba_bgra_ssse3 (uint8_t *dst, const uint8_t *src, size_t n_pixels);
void swizzle_rgba_bgra (uint8_t *dst, const uint8_t *src, size_t n_pixels);

#endif
