#ifndef GL_KTX2_H
#define GL_KTX2_H

#include <stddef.h>
#include <stdint.h>

#include "q_stdinc.h"
#include "asset_decode_async.h"

typedef struct {
    int mip_count;
    int width[32];
    int height[32];
    uint8_t *mip_data[32];   // RGBA8 decoded data
    size_t mip_size[32];     // size in bytes
} ktx2_decoded_image_t;

/* Forward declaration */
typedef struct gltexture_s gltexture_t;

typedef struct {
    qboolean valid;

    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_count;

    uint64_t supercompression;

    uint64_t dfd_offset;
    uint64_t dfd_length;

    uint64_t kvd_offset;
    uint64_t kvd_length;

    uint64_t sgd_offset;
    uint64_t sgd_length;

    int level_count;
    struct {
        uint64_t offset;
        uint64_t length;
        uint64_t uncompressed_length;
    } levels[32];
} ktx2_header_t;

qboolean KTX2_IsValid(const uint8_t *data, size_t size);
qboolean KTX2_ParseHeaderPublic(ktx2_header_t *out, const uint8_t *data, size_t size);
gltexture_t *R_LoadKTX2Texture(const char *name, const uint8_t *data, size_t size);
void KTX2_LogInfo(const char *fmt, ...);
void KTX2_LogError(const char *fmt, ...);
void R_TestKTX2(void);
asset_backend_format_t KTX2_SelectBackendTargetFormat(void);
qboolean KTX2_TranscodeToRGBA(const uint8_t *filedata, size_t filesize, const ktx2_header_t *hdr, ktx2_decoded_image_t *out);
void KTX2_FreeDecodedImage(ktx2_decoded_image_t *img);

#endif /* GL_KTX2_H */
