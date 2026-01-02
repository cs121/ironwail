#include "quakedef.h"
#include "gl_ktx2.h"
#include "gl_texmgr.h"
#include "basisu_transcoder.h"

static const uint8_t KTX2_MAGIC[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

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
    KTX2_LogInfo("KTX2: %ux%u, mips=%u, supercompression=%llu (header validated)",
                 out->width, out->height, out->mip_count, (unsigned long long)out->supercompression);
    return true;
}

qboolean KTX2_IsValid(const uint8_t *data, size_t size)
{
    ktx2_header_t hdr;
    return KTX2_ParseHeader(&hdr, data, size);
}

qboolean KTX2_ParseHeaderPublic(ktx2_header_t *out, const uint8_t *data, size_t size)
{
    return KTX2_ParseHeader(out, data, size);
}

qboolean KTX2_TranscodeToRGBA(const uint8_t *filedata, size_t filesize, const ktx2_header_t *hdr, ktx2_decoded_image_t *out)
{
    int i;

    if (!filedata || !hdr || !out)
    {
        KTX2_LogError("failed at step init: invalid arguments");
        return false;
    }

    if (!hdr->valid)
    {
        KTX2_LogError("failed at step init: header not validated");
        return false;
    }

    memset(out, 0, sizeof(*out));
    basisu_transcoder_init();

    if (hdr->supercompression != 0 && hdr->supercompression != 1)
    {
        KTX2_LogError("failed at step init: unsupported supercompression scheme %llu", (unsigned long long)hdr->supercompression);
        return false;
    }

    if (hdr->supercompression == 1 && hdr->sgd_length == 0)
    {
        KTX2_LogError("failed at step init: BasisLZ supercompression requires supercompression global data");
        return false;
    }

    if (hdr->sgd_length && (hdr->sgd_offset > filesize || hdr->sgd_length > filesize - hdr->sgd_offset))
    {
        KTX2_LogError("failed at step init: supercompression global data out of bounds");
        return false;
    }

    if (hdr->level_count <= 0)
    {
        KTX2_LogError("failed at step init: no mip levels");
        return false;
    }

    out->mip_count = hdr->level_count;

    /* BasisLZ uses the supercompression global data blob; UASTC does not. */
    const uint8_t *sgd_data = hdr->sgd_length ? filedata + hdr->sgd_offset : NULL;
    size_t sgd_size = hdr->sgd_length ? (size_t)hdr->sgd_length : 0;

    for (i = 0; i < hdr->level_count; ++i)
    {
        const uint64_t offset = hdr->levels[i].offset;
        const uint64_t length = hdr->levels[i].length;
        uint8_t *level_data = NULL;
        size_t level_size = 0;
        qboolean level_owned = false;
        qboolean is_uastc = (hdr->supercompression == 0);
        int width = (int)q_max(1U, hdr->width >> i);
        int height = (int)q_max(1U, hdr->height >> i);

        if (offset > filesize || length > filesize - offset)
        {
            KTX2_LogError("failed at step level bounds: level %d", i);
            KTX2_FreeDecodedImage(out);
            return false;
        }

        out->width[i] = width;
        out->height[i] = height;
        out->mip_size[i] = (size_t)width * (size_t)height * 4u;
        out->mip_data[i] = (uint8_t *)malloc(out->mip_size[i]);
        if (!out->mip_data[i])
        {
            KTX2_LogError("failed at step alloc mip: level %d", i);
            KTX2_FreeDecodedImage(out);
            return false;
        }

        if (hdr->supercompression == 0)
        {
            level_size = (size_t)length;
            level_data = (uint8_t *)malloc(level_size);
            if (!level_data)
            {
                KTX2_LogError("failed at step copy level: out of memory");
                KTX2_FreeDecodedImage(out);
                return false;
            }
            memcpy(level_data, filedata + offset, level_size);
            level_owned = true;
        }
        else
        {
            /* BASISLZ: hand the supercompressed payload and global tables to the transcoder. */
            level_size = (size_t)length;
            level_data = (uint8_t *)(filedata + offset);
        }

        if (!basisu_transcoder_transcode_image_level(level_data, level_size, is_uastc, sgd_data, sgd_size, (uint32_t)i, (uint32_t)width, (uint32_t)height, out->mip_data[i], out->mip_size[i]))
        {
            KTX2_LogError("failed at step transcode: level %d", i);
            if (level_owned)
                free(level_data);
            KTX2_FreeDecodedImage(out);
            return false;
        }

        if (level_owned)
            free(level_data);
    }

    KTX2_LogInfo("Transcoded %d mip levels to RGBA8", out->mip_count);
    return true;
}

gltexture_t *R_LoadKTX2Texture(const char *name, const uint8_t *data, size_t size)
{
    ktx2_header_t hdr;
    ktx2_decoded_image_t decoded;
    gltexture_t *tex;
    GLenum gl_internal_format = GL_RGBA8;
    qboolean use_compressed = false;
    qboolean srgb = false;
    int i;

    memset(&decoded, 0, sizeof(decoded));

    if (!KTX2_ParseHeader(&hdr, data, size))
        return NULL;

    if (!KTX2_TranscodeToRGBA(data, size, &hdr, &decoded))
    {
        KTX2_FreeDecodedImage(&decoded);
        return NULL;
    }

    tex = TexMgr_NewTexture();
    if (!tex)
    {
        KTX2_FreeDecodedImage(&decoded);
        return NULL;
    }

    srgb = TexMgr_ShouldUseSRGB(name, SRC_RGBA, TEXPREF_MIPMAP, "");
#ifdef GL_SRGB8
    if (srgb)
        gl_internal_format = GL_SRGB8_ALPHA8;
#endif

    /* KTX2_TranscodeToRGBA() currently produces RGBA8 payloads, so upload uncompressed. */
    use_compressed = false;

    if (use_compressed)
    {
        for (i = 0; i < decoded.mip_count; i++)
        {
            size_t expected_rgba = (size_t)decoded.width[i] * (size_t)decoded.height[i] * 4u;
            if (decoded.mip_size[i] == expected_rgba)
            {
                KTX2_LogError("expected compressed payload for mip %d but got %zu bytes (RGBA8), falling back to uncompressed upload", i, decoded.mip_size[i]);
                gl_internal_format = GL_RGBA8;
                use_compressed = false;
                break;
            }
        }
    }

    tex->owner = NULL;
    tex->target = GL_TEXTURE_2D;
    q_strlcpy(tex->name, name, sizeof(tex->name));
    tex->width = (unsigned short)decoded.width[0];
    tex->height = (unsigned short)decoded.height[0];
    tex->depth = 1;
    tex->compression = use_compressed ? 4 : 1;
    tex->flags = TEXPREF_MIPMAP | (srgb ? TEXPREF_SRGB : 0);
    tex->source_file[0] = '\0';
    tex->source_offset = 0;
    tex->source_format = SRC_RGBA;
    tex->source_width = decoded.width[0];
    tex->source_height = decoded.height[0];
    tex->source_crc = 0;
    tex->shirt = -1;
    tex->pants = -1;
    tex->visframe = 0;
    tex->mipmap = decoded.mip_count;
    tex->internal_format = gl_internal_format;

    glBindTexture(GL_TEXTURE_2D, tex->texnum);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    for (i = 0; i < decoded.mip_count; i++)
    {
        int w = decoded.width[i];
        int h = decoded.height[i];

        KTX2_LogInfo("Upload mip %d: %dx%d (%zu bytes)", i, w, h, decoded.mip_size[i]);

        glTexImage2D(GL_TEXTURE_2D, i, gl_internal_format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded.mip_data[i]);
    }

    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR)
            KTX2_LogError("GL upload failed: 0x%x", err);
    }

    KTX2_FreeDecodedImage(&decoded);

    Con_DPrintf("Texture %s: %s, %s\n", name, srgb ? "sRGB" : "linear", srgb ? "GL_SRGB8_ALPHA8" : "GL_RGBA8");
    KTX2_LogInfo("Finished uploading KTX2 texture '%s' (%dx%d, mips=%d)", name, tex->width, tex->height, tex->mipmap);
    return tex;
}

void KTX2_LogInfo(const char *fmt, ...)
{
    va_list argptr;
    char msg[1024];

    va_start(argptr, fmt);
    q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    Con_DPrintf("KTX2-INFO: %s\n", msg);
    va_end(argptr);
}

void KTX2_LogError(const char *fmt, ...)
{
    va_list argptr;
    char msg[1024];

    va_start(argptr, fmt);
    q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    Con_DPrintf("KTX2-ERROR: %s\n", msg);
    va_end(argptr);
}

void R_TestKTX2(void)
{
    KTX2_LogInfo("R_TestKTX2: stub");
}

void KTX2_FreeDecodedImage(ktx2_decoded_image_t *img)
{
    int i;

    if (!img)
        return;

    for (i = 0; i < img->mip_count; i++)
    {
        free(img->mip_data[i]);
        img->mip_data[i] = NULL;
    }
    memset(img, 0, sizeof(*img));
}

// In r_textures.c oder gl_model.c einbauen:
//
// if (COM_HasExtension(name, ".ktx2")) {
//     return R_LoadKTX2Texture(name, rawbuf, filesize);
// }
// (derzeit nur Stub, lädt nichts)
