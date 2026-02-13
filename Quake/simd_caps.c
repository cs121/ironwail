#include "quakedef.h"
#include "simd_caps.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SIMD_X86_FAMILY 1
#else
#define SIMD_X86_FAMILY 0
#endif

#if SIMD_X86_FAMILY
#if defined(USE_SSE2)
#include <emmintrin.h>
#endif
#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif
#endif

cvar_t simd_enable = {"simd_enable", "1", CVAR_ARCHIVE};
cvar_t simd_force = {"simd_force", "0", CVAR_ARCHIVE};

static simd_caps_t simd_caps;
static qboolean simd_caps_initialized;

static int simd_mode_cache = -1;
static int simd_force_cache = -999;
static int simd_enable_cache = -999;

#if SIMD_X86_FAMILY && defined(_MSC_VER)
static void SIMD_CPUID (int out[4], int leaf, int subleaf)
{
	__cpuidex (out, leaf, subleaf);
}

static uint64_t SIMD_XGetBV0 (void)
{
	return _xgetbv (0);
}
#endif

simd_caps_t SIMD_GetCaps (void)
{
	simd_caps_t caps = {0, 0, 0, 0};

#if SIMD_X86_FAMILY
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(_M_X64)
	caps.sse2 = 1;
#else
	caps.sse2 = __builtin_cpu_supports ("sse2") != 0;
#endif
	caps.ssse3 = __builtin_cpu_supports ("ssse3") != 0;
	caps.sse41 = __builtin_cpu_supports ("sse4.1") != 0;
	caps.avx2 = __builtin_cpu_supports ("avx2") != 0;
#elif defined(_MSC_VER)
	int regs[4];
	int max_basic;
	qboolean osxsave;
	qboolean avx_os;

	SIMD_CPUID (regs, 0, 0);
	max_basic = regs[0];
	if (max_basic < 1)
		return caps;

#if defined(__x86_64__) || defined(_M_X64)
	caps.sse2 = 1;
#else
	SIMD_CPUID (regs, 1, 0);
	caps.sse2 = (regs[3] & (1 << 26)) != 0;
#endif

	SIMD_CPUID (regs, 1, 0);
	caps.ssse3 = (regs[2] & (1 << 9)) != 0;
	caps.sse41 = (regs[2] & (1 << 19)) != 0;

	osxsave = (regs[2] & (1 << 27)) != 0;
	avx_os = false;
	if (osxsave)
	{
		uint64_t xcr0 = SIMD_XGetBV0 ();
		avx_os = ((xcr0 & 0x6) == 0x6);
	}

	if (max_basic >= 7 && avx_os)
	{
		SIMD_CPUID (regs, 7, 0);
		caps.avx2 = (regs[1] & (1 << 5)) != 0;
	}
#endif
#endif

	return caps;
}

void SIMD_Init (void)
{
	if (simd_caps_initialized)
		return;

	Cvar_RegisterVariable (&simd_enable);
	Cvar_RegisterVariable (&simd_force);

	simd_caps = SIMD_GetCaps ();
	simd_caps_initialized = true;
}

int SIMD_Mode (void)
{
	int force;
	int enable;
	int mode;

	if (!simd_caps_initialized)
		SIMD_Init ();

	force = (int)simd_force.value;
	enable = simd_enable.value > 0.f;
	if (force == simd_force_cache && enable == simd_enable_cache && simd_mode_cache >= 0)
		return simd_mode_cache;

	simd_force_cache = force;
	simd_enable_cache = enable;

	if (!enable)
	{
		simd_mode_cache = SIMD_MODE_SCALAR;
		return simd_mode_cache;
	}

	if (force >= SIMD_MODE_SCALAR && force <= SIMD_MODE_AVX2)
	{
		mode = force;
		if (mode == SIMD_MODE_AVX2 && !simd_caps.avx2)
			mode = SIMD_MODE_SCALAR;
		else if (mode == SIMD_MODE_SSE41 && !simd_caps.sse41)
			mode = SIMD_MODE_SCALAR;
		else if (mode == SIMD_MODE_SSSE3 && !simd_caps.ssse3)
			mode = SIMD_MODE_SCALAR;
		else if (mode == SIMD_MODE_SSE2 && !simd_caps.sse2)
			mode = SIMD_MODE_SCALAR;
		simd_mode_cache = mode;
		return simd_mode_cache;
	}

	if (simd_caps.avx2)
		simd_mode_cache = SIMD_MODE_AVX2;
	else if (simd_caps.sse41)
		simd_mode_cache = SIMD_MODE_SSE41;
	else if (simd_caps.ssse3)
		simd_mode_cache = SIMD_MODE_SSSE3;
	else if (simd_caps.sse2)
		simd_mode_cache = SIMD_MODE_SSE2;
	else
		simd_mode_cache = SIMD_MODE_SCALAR;

	return simd_mode_cache;
}

void swizzle_rgba_bgra_scalar (uint8_t *dst, const uint8_t *src, size_t n_pixels)
{
	size_t i;
	for (i = 0; i < n_pixels; ++i)
	{
		const uint8_t *s = src + i * 4;
		uint8_t *d = dst + i * 4;
		d[0] = s[2];
		d[1] = s[1];
		d[2] = s[0];
		d[3] = s[3];
	}
}

void swizzle_rgba_bgra_sse2 (uint8_t *dst, const uint8_t *src, size_t n_pixels)
{
#if SIMD_X86_FAMILY && defined(USE_SSE2)
	/*
	 * RGBA->BGRA using SSE2 only (no pshufb): swap byte 0/2 inside each dword.
	 * Uses unaligned loads/stores and falls back to scalar for the tail.
	 */
	size_t i = 0;
	const __m128i rb_mask = _mm_set1_epi32 (0x00FF00FF);
	for (; i + 4 <= n_pixels; i += 4)
	{
		__m128i v = _mm_loadu_si128 ((const __m128i *)(src + i * 4));
		__m128i rb = _mm_and_si128 (v, rb_mask);
		__m128i ga = _mm_andnot_si128 (rb_mask, v);
		__m128i rb_swapped = _mm_or_si128 (_mm_slli_epi32 (rb, 16), _mm_srli_epi32 (rb, 16));
		_mm_storeu_si128 ((__m128i *)(dst + i * 4), _mm_or_si128 (ga, rb_swapped));
	}
	if (i < n_pixels)
		swizzle_rgba_bgra_scalar (dst + i * 4, src + i * 4, n_pixels - i);
#else
	swizzle_rgba_bgra_scalar (dst, src, n_pixels);
#endif
}

void swizzle_rgba_bgra_ssse3 (uint8_t *dst, const uint8_t *src, size_t n_pixels)
{
#if SIMD_X86_FAMILY && defined(__SSSE3__)
	/*
	 * RGBA->BGRA using SSSE3 pshufb.
	 * Shuffle mask reorders each pixel from [R G B A] to [B G R A].
	 */
	size_t i = 0;
	const __m128i shuf = _mm_setr_epi8 (
		2, 1, 0, 3,
		6, 5, 4, 7,
		10, 9, 8, 11,
		14, 13, 12, 15);
	for (; i + 4 <= n_pixels; i += 4)
	{
		__m128i v = _mm_loadu_si128 ((const __m128i *)(src + i * 4));
		_mm_storeu_si128 ((__m128i *)(dst + i * 4), _mm_shuffle_epi8 (v, shuf));
	}
	if (i < n_pixels)
		swizzle_rgba_bgra_scalar (dst + i * 4, src + i * 4, n_pixels - i);
#else
	swizzle_rgba_bgra_sse2 (dst, src, n_pixels);
#endif
}

void swizzle_rgba_bgra (uint8_t *dst, const uint8_t *src, size_t n_pixels)
{
	int mode = SIMD_Mode ();
	if (mode >= SIMD_MODE_SSSE3)
		swizzle_rgba_bgra_ssse3 (dst, src, n_pixels);
	else if (mode >= SIMD_MODE_SSE2)
		swizzle_rgba_bgra_sse2 (dst, src, n_pixels);
	else
		swizzle_rgba_bgra_scalar (dst, src, n_pixels);
}
