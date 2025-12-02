#include "quakedef.h"
#include "gl_local.h"
#include "gl_ktx2.h"

static const uint8_t KTX2_MAGIC[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

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

static uint32_t KTX2_ReadLE32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t KTX2_ReadLE64(const uint8_t *p)
{
    return ((uint64_t)p[0]) | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static qboolean KTX2_ParseHeader(ktx2_header_t *out, const uint8_t *data, size_t size)
{
    const size_t header_size = 100; /* size of ktx2_header_raw */
    size_t level_index_size;
    size_t level_table_size;
    size_t offset;
    int i;

    memset(out, 0, sizeof(*out));

    if (!data || size < header_size)
    {
        KTX2_LogError("buffer too small for KTX2 header");
        return false;
    }

    if (memcmp(data, KTX2_MAGIC, sizeof(KTX2_MAGIC)) != 0)
    {
        KTX2_LogError("invalid KTX2 magic");
        return false;
    }

    out->valid = false;
    out->width = KTX2_ReadLE32(data + 12 + 4 * 2); /* pixelWidth */
    out->height = KTX2_ReadLE32(data + 12 + 4 * 3); /* pixelHeight */
    out->depth = KTX2_ReadLE32(data + 12 + 4 * 4); /* pixelDepth */
    out->mip_count = KTX2_ReadLE32(data + 12 + 4 * 7); /* levelCount */
    out->supercompression = KTX2_ReadLE64(data + 12 + 4 * 8); /* supercompressionScheme */

    out->dfd_offset = KTX2_ReadLE64(data + 12 + 4 * 8 + 8 * 1);
    out->dfd_length = KTX2_ReadLE64(data + 12 + 4 * 8 + 8 * 2);

    out->kvd_offset = KTX2_ReadLE64(data + 12 + 4 * 8 + 8 * 3);
    out->kvd_length = KTX2_ReadLE64(data + 12 + 4 * 8 + 8 * 4);

    out->sgd_offset = KTX2_ReadLE64(data + 12 + 4 * 8 + 8 * 5);
    out->sgd_length = KTX2_ReadLE64(data + 12 + 4 * 8 + 8 * 6);

    if (out->width == 0)
    {
        KTX2_LogError("pixelWidth must be > 0");
        return false;
    }

    if (out->mip_count < 1)
    {
        KTX2_LogError("levelCount must be >= 1");
        return false;
    }

    if (out->mip_count > (int)(sizeof(out->levels) / sizeof(out->levels[0])))
    {
        KTX2_LogError("levelCount %u exceeds supported limit", (unsigned)out->mip_count);
        return false;
    }

    level_index_size = sizeof(uint64_t) * 3;
    if (size < header_size)
    {
        KTX2_LogError("buffer too small after header check");
        return false;
    }

    if (out->mip_count > SIZE_MAX / level_index_size)
    {
        KTX2_LogError("level count overflow");
        return false;
    }

    level_table_size = (size_t)out->mip_count * level_index_size;
    if (level_table_size > SIZE_MAX - header_size || header_size + level_table_size > size)
    {
        KTX2_LogError("buffer too small for level index table");
        return false;
    }

    offset = header_size;
    out->level_count = out->mip_count;
    for (i = 0; i < out->level_count; ++i)
    {
        const uint8_t *entry = data + offset + i * level_index_size;
        uint64_t byte_offset = KTX2_ReadLE64(entry + 0);
        uint64_t byte_length = KTX2_ReadLE64(entry + 8);
        uint64_t uncompressed_length = KTX2_ReadLE64(entry + 16);

        if (byte_offset > size || byte_length > size - byte_offset)
        {
            KTX2_LogError("level %d range out of bounds", i);
            return false;
        }

        out->levels[i].offset = byte_offset;
        out->levels[i].length = byte_length;
        out->levels[i].uncompressed_length = uncompressed_length;
    }

    if (out->dfd_length && (out->dfd_offset > size || out->dfd_length > size - out->dfd_offset))
    {
        KTX2_LogError("DFD range out of bounds");
        return false;
    }

    if (out->kvd_length && (out->kvd_offset > size || out->kvd_length > size - out->kvd_offset))
    {
        KTX2_LogError("KVD range out of bounds");
        return false;
    }

    if (out->sgd_length && (out->sgd_offset > size || out->sgd_length > size - out->sgd_offset))
    {
        KTX2_LogError("SGD range out of bounds");
        return false;
    }

    out->valid = true;
    KTX2_LogInfo("KTX2: %ux%u, mips=%u, supercompression=%llu",
                 out->width, out->height, out->mip_count, (unsigned long long)out->supercompression);
    return true;
}

qboolean KTX2_IsValid(const uint8_t *data, size_t size)
{
    ktx2_header_t hdr;
    return KTX2_ParseHeader(&hdr, data, size);
}

gltexture_t *R_LoadKTX2Texture(const char *name, const uint8_t *data, size_t size)
{
    (void)name;
    (void)data;
    (void)size;
    KTX2_LogInfo("R_LoadKTX2Texture: header parsed, no decoding yet");
    return NULL;
}

void KTX2_LogInfo(const char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    Con_Printf("KTX2-INFO: ");
    Con_VPrintf(fmt, argptr);
    Con_Printf("\n");
    va_end(argptr);
}

void KTX2_LogError(const char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    Con_Printf("KTX2-ERROR: ");
    Con_VPrintf(fmt, argptr);
    Con_Printf("\n");
    va_end(argptr);
}

void R_TestKTX2(void)
{
    KTX2_LogInfo("R_TestKTX2: stub");
}

// In r_textures.c oder gl_model.c einbauen:
//
// if (COM_HasExtension(name, ".ktx2")) {
//     return R_LoadKTX2Texture(name, rawbuf, filesize);
// }
// (derzeit nur Stub, lädt nichts)
