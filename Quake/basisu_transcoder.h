#ifndef BASISU_TRANSCODER_H
#define BASISU_TRANSCODER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal stub of the Basis Universal transcoder API.
 * This is **not** a complete transcoder; it only provides enough
 * functionality for the CPU-side KTX2 decoding path implemented in
 * gl_ktx2.c. The implementation simply expands the input data into
 * opaque RGBA8 pixels so the rest of the pipeline can be exercised
 * without GPU involvement or the full BasisU dependency chain.
 */

#ifdef __cplusplus
extern "C" {
#endif

static inline void basisu_transcoder_init(void)
{
    /* No-op in the stub implementation */
}

/*
 * Transcodes a single image level to RGBA8.
 *
 * Parameters:
 * - compressed: Pointer to compressed ETC1S or UASTC data.
 * - compressed_size: Size of compressed data in bytes.
 * - is_uastc: Non-zero if the payload is UASTC, zero for BasisLZ/ETC1S.
 * - global_data/global_size: Optional pointer to BasisLZ supercompression
 *   global data (endpoint/selector tables). May be NULL for UASTC payloads.
 * - level_index: Mip level index for informational purposes.
 * - width/height: Dimensions of the mip level.
 * - out_rgba: Destination buffer sized for width*height*4 bytes.
 *
 * Returns: true on success.
 */
static inline int basisu_transcoder_transcode_image_level(const uint8_t *compressed,
                                                          size_t compressed_size,
                                                          int is_uastc,
                                                          const uint8_t *global_data,
                                                          size_t global_size,
                                                          uint32_t level_index,
                                                          uint32_t width,
                                                          uint32_t height,
                                                          uint8_t *out_rgba,
                                                          size_t out_size)
{
    size_t expected = (size_t)width * (size_t)height * 4u;
    size_t i;

    (void)compressed;
    (void)compressed_size;
    (void)is_uastc;
    (void)level_index;
    (void)global_data;
    (void)global_size;

    if (!out_rgba || out_size < expected)
        return 0;

    /* Fill with a deterministic pattern so callers can observe output. */
    for (i = 0; i < expected; i += 4)
    {
        uint8_t x = (uint8_t)((i / 4) % 256);
        uint8_t y = (uint8_t)(((i / 4) / (width ? width : 1)) % 256);
        uint8_t g = (uint8_t)(is_uastc ? 255 : 128);
        uint8_t b = (uint8_t)((global_size ? global_data[global_size - 1] : 0) ^ (uint8_t)level_index);
        out_rgba[i + 0] = x;
        out_rgba[i + 1] = y;
        out_rgba[i + 2] = g ^ b;
        out_rgba[i + 3] = 255;
    }

    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* BASISU_TRANSCODER_H */
