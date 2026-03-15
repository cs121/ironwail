#include "quakedef.h"
#include "ktx2_write.h"

#define KTX2_HEADER_SIZE 80u
#define KTX2_LEVEL_INDEX_SIZE 24u

static const uint8_t ktx2_magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

/* KHR_DF DFD block for VK_FORMAT_R8G8B8A8_SRGB (taken from KTX-Software rgba_mipmap.ktx2). */
static const uint8_t ktx2_dfd_rgba8_srgb[] = {
        0x5c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x58, 0x00, 0x01, 0x01, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x08, 0x00, 0x07, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x10, 0x00, 0x07, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x18, 0x00, 0x07, 0x1f, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00
};

static size_t KTX2_Align4(size_t value)
{
        return (value + 3u) & ~3u;
}

static void KTX2_WriteLE32(uint8_t *dst, uint32_t value)
{
        dst[0] = (uint8_t)(value & 0xffu);
        dst[1] = (uint8_t)((value >> 8) & 0xffu);
        dst[2] = (uint8_t)((value >> 16) & 0xffu);
        dst[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void KTX2_WriteLE64(uint8_t *dst, uint64_t value)
{
        dst[0] = (uint8_t)(value & 0xffu);
        dst[1] = (uint8_t)((value >> 8) & 0xffu);
        dst[2] = (uint8_t)((value >> 16) & 0xffu);
        dst[3] = (uint8_t)((value >> 24) & 0xffu);
        dst[4] = (uint8_t)((value >> 32) & 0xffu);
        dst[5] = (uint8_t)((value >> 40) & 0xffu);
        dst[6] = (uint8_t)((value >> 48) & 0xffu);
        dst[7] = (uint8_t)((value >> 56) & 0xffu);
}

qboolean KTX2_WriteRGBA8File(const char *path, int width, int height, int mip_count,
                             const uint8_t **mip_data, const size_t *mip_sizes)
{
        uint8_t *buffer = NULL;
        size_t header_size = KTX2_HEADER_SIZE;
        size_t level_index_size;
        size_t dfd_offset;
        size_t dfd_length = sizeof(ktx2_dfd_rgba8_srgb);
        size_t kvd_offset = 0;
        size_t kvd_length = 0;
        size_t sgd_offset = 0;
        size_t sgd_length = 0;
        size_t data_offset;
        size_t total_size;
        size_t i;

        if (!path || width <= 0 || height <= 0 || mip_count <= 0 || !mip_data || !mip_sizes)
                return false;

        level_index_size = (size_t)mip_count * KTX2_LEVEL_INDEX_SIZE;
        dfd_offset = KTX2_Align4(header_size + level_index_size);
        data_offset = KTX2_Align4(dfd_offset + dfd_length);

        total_size = data_offset;
        for (i = 0; i < (size_t)mip_count; ++i)
                total_size = KTX2_Align4(total_size + mip_sizes[i]);

        buffer = (uint8_t *)q_calloc(1, total_size);
        if (!buffer)
                return false;

        memcpy(buffer, ktx2_magic, sizeof(ktx2_magic));
        KTX2_WriteLE32(buffer + 12, 43u); /* VK_FORMAT_R8G8B8A8_SRGB */
        KTX2_WriteLE32(buffer + 16, 1u);
        KTX2_WriteLE32(buffer + 20, (uint32_t)width);
        KTX2_WriteLE32(buffer + 24, (uint32_t)height);
        KTX2_WriteLE32(buffer + 28, 0u);
        KTX2_WriteLE32(buffer + 32, 0u);
        KTX2_WriteLE32(buffer + 36, 1u);
        KTX2_WriteLE32(buffer + 40, (uint32_t)mip_count);
        KTX2_WriteLE32(buffer + 44, 0u);
        KTX2_WriteLE32(buffer + 48, (uint32_t)dfd_offset);
        KTX2_WriteLE32(buffer + 52, (uint32_t)dfd_length);
        KTX2_WriteLE32(buffer + 56, (uint32_t)kvd_offset);
        KTX2_WriteLE32(buffer + 60, (uint32_t)kvd_length);
        KTX2_WriteLE64(buffer + 64, (uint64_t)sgd_offset);
        KTX2_WriteLE64(buffer + 72, (uint64_t)sgd_length);

        {
                size_t level_table_offset = header_size;
                size_t running_offset = data_offset;
                for (i = 0; i < (size_t)mip_count; ++i)
                {
                        size_t entry_offset = level_table_offset + i * KTX2_LEVEL_INDEX_SIZE;
                        KTX2_WriteLE64(buffer + entry_offset, (uint64_t)running_offset);
                        KTX2_WriteLE64(buffer + entry_offset + 8, (uint64_t)mip_sizes[i]);
                        KTX2_WriteLE64(buffer + entry_offset + 16, (uint64_t)mip_sizes[i]);

                        memcpy(buffer + running_offset, mip_data[i], mip_sizes[i]);
                        running_offset = KTX2_Align4(running_offset + mip_sizes[i]);
                }
        }

        memcpy(buffer + dfd_offset, ktx2_dfd_rgba8_srgb, dfd_length);

        if (!COM_WriteFile_OSPath(path, buffer, total_size))
        {
                q_free(buffer);
                return false;
        }

        q_free(buffer);
        return true;
}
