//
// Copyright (C) 
// Lightgrid system (improved version)
//
// Fully MSVC-compatible, with RAW + BSPX loading, fallback,
// and safe trilinear interpolation.
//

#include "quakedef.h"
#include "gl_lightgrid.h"
#include "gl_model.h"
#include "glquake.h"
#include <stdlib.h>
#include <float.h>
#include <math.h>

extern vec3_t lightcolor;

static lightgrid_t *current_lightgrid = NULL;
static char lightgrid_source[16] = "NONE";

cvar_t r_lightgrid            = { "r_lightgrid", "1", CVAR_ARCHIVE };
cvar_t r_lightgrid_debug      = { "r_lightgrid_debug", "0", CVAR_NONE };
cvar_t r_lightgrid_force      = { "r_lightgrid_force", "0", CVAR_ARCHIVE };
cvar_t r_generate_lightgrid_test = { "r_generate_lightgrid_test", "0", CVAR_NONE };
cvar_t r_lightgrid_weight_direct = { "r_lightgrid_weight_direct", "0.3", CVAR_ARCHIVE };
cvar_t r_lightgrid_weight_lightmap = { "r_lightgrid_weight_lightmap", "1", CVAR_ARCHIVE };
cvar_t r_lightgrid_surface_weight = { "r_lightgrid_surface_weight", "0", CVAR_ARCHIVE };
cvar_t r_lightgrid_bounce_factor = { "r_lightgrid_bounce_factor", "0.5", CVAR_ARCHIVE };
cvar_t r_lightgrid_bounce_rays = { "r_lightgrid_bounce_rays", "16", CVAR_ARCHIVE };
cvar_t r_lightgrid_sh9 = { "r_lightgrid_sh9", "0", CVAR_ARCHIVE };
#if USE_KTX2_LIGHTGRID
cvar_t r_lightgrid_ktx_enable = { "r_lightgrid_ktx_enable", "1", CVAR_ARCHIVE };
cvar_t r_lightgrid_ktx_export = { "r_lightgrid_ktx_export", "0", CVAR_ARCHIVE };
cvar_t r_lightgrid_ktx_prefer = { "r_lightgrid_ktx_prefer", "0", CVAR_ARCHIVE };
#endif

static qboolean R_IsFinite (float v)
{
#if defined(_MSC_VER)
    return _finite(v) != 0;
#else
    return isfinite(v);
#endif
}

/* =====================================================================
   Forward Declarations
   ===================================================================== */
static void  Lightgrid_AddDlights(const vec3_t pos, vec3_t color, vec3_t dirsum);
static float Lightgrid_Random01(void);
static void  Lightgrid_FillRandomCell(lightcell_t *cell);

/* =====================================================================
   Init / Shutdown
   ===================================================================== */

void Lightgrid_Init(void)
{
    Cvar_RegisterVariable(&r_lightgrid);
    Cvar_RegisterVariable(&r_lightgrid_debug);
    Cvar_RegisterVariable(&r_lightgrid_force);
    Cvar_RegisterVariable(&r_generate_lightgrid_test);
    Cvar_RegisterVariable(&r_lightgrid_weight_direct);
    Cvar_RegisterVariable(&r_lightgrid_weight_lightmap);
    Cvar_RegisterVariable(&r_lightgrid_surface_weight);
    Cvar_RegisterVariable(&r_lightgrid_bounce_factor);
    Cvar_RegisterVariable(&r_lightgrid_bounce_rays);
    Cvar_RegisterVariable(&r_lightgrid_sh9);
#if USE_KTX2_LIGHTGRID
    Cvar_RegisterVariable(&r_lightgrid_ktx_enable);
    Cvar_RegisterVariable(&r_lightgrid_ktx_export);
    Cvar_RegisterVariable(&r_lightgrid_ktx_prefer);
#endif
    Lightgrid_Clear();
}

void Lightgrid_Shutdown(void)
{
    Lightgrid_Clear();
}

void Lightgrid_Clear(void)
{
    if (current_lightgrid)
    {
#if USE_KTX2_LIGHTGRID
        if (current_lightgrid->tex_color3d || current_lightgrid->tex_dir3d)
        {
            GLuint tex[2] = { current_lightgrid->tex_color3d, current_lightgrid->tex_dir3d };
            glDeleteTextures(2, tex);
            current_lightgrid->tex_color3d = 0;
            current_lightgrid->tex_dir3d = 0;
        }
#endif

        Lightgrid_Free(current_lightgrid);
    }
    current_lightgrid = NULL;
    q_strlcpy(lightgrid_source, "NONE", sizeof(lightgrid_source));
}

/* =====================================================================
   Helpers
   ===================================================================== */

static float LG_FLittle(const void *p)
{
    float f;
    memcpy(&f, p, sizeof(float));
    return LittleFloat(f);
}

static size_t LightgridRAW_ComponentCount(unsigned int components)
{
    size_t count = 0;

    if (components & LIGHTGRID_RAW_COMPONENT_RGB)
        count += 3;
    if (components & LIGHTGRID_RAW_COMPONENT_DIR)
        count += 3;
    if (components & LIGHTGRID_RAW_COMPONENT_INTENSITY)
        count += 1;
    if (components & LIGHTGRID_RAW_COMPONENT_AO)
        count += 1;
    if (components & LIGHTGRID_RAW_COMPONENT_EMISSIVE)
        count += 1;

    if (components & LIGHTGRID_RAW_COMPONENT_SH9)
        return 0; // Not implemented yet

    return count;
}

static size_t LightgridRAW_CellSizeBytes(unsigned int components, unsigned int encoding)
{
    size_t component_count = LightgridRAW_ComponentCount(components);
    size_t component_size;

    if (!component_count)
        return 0;

    switch (encoding)
    {
    case LIGHTGRID_RAW_ENCODING_FLOAT32:
        component_size = sizeof(float);
        break;
    case LIGHTGRID_RAW_ENCODING_FLOAT16:
        component_size = sizeof(unsigned short);
        break;
    default:
        return 0;
    }

    return component_count * component_size;
}

static float LightgridRAW_HalfToFloat(unsigned short h)
{
    unsigned int sign = (unsigned int)(h & 0x8000) << 16;
    unsigned int exp = (h >> 10) & 0x1f;
    unsigned int mant = h & 0x3ff;
    unsigned int f;

    if (exp == 0)
    {
        if (mant == 0)
        {
            f = sign;
        }
        else
        {
            exp = 1;
            while ((mant & 0x400) == 0)
            {
                mant <<= 1;
                exp--;
            }
            mant &= ~0x400;
            exp += (127 - 15);
            mant <<= 13;
            f = sign | (exp << 23) | mant;
        }
    }
    else if (exp == 31)
    {
        f = sign | 0x7f800000 | (mant << 13);
    }
    else
    {
        exp = exp + (127 - 15);
        mant = mant << 13;
        f = sign | (exp << 23) | mant;
    }

    return *((float *)&f);
}

/* =====================================================================
   BSPX Loader
   ===================================================================== */

qboolean Lightgrid_LoadFromBSPX(void *bspx_data, int bspx_len)
{
    if (!bspx_data || bspx_len <= 0)
        return false;

    Lightgrid_Clear();

    const byte *payload = (const byte *)bspx_data;
    size_t payload_size = (size_t)bspx_len;
    size_t header_size = sizeof(int) * 3 + sizeof(float) + sizeof(vec3_t);
    unsigned int version = LIGHTGRID_RAW_VERSION_1;
    unsigned int components = LIGHTGRID_RAW_COMPONENTS_BASE;
    unsigned int encoding = LIGHTGRID_RAW_ENCODING_FLOAT32;
    int nx = 0, ny = 0, nz = 0;
    float cellSize = 0.f;
    vec3_t origin = {0.f, 0.f, 0.f};

    if (payload_size >= sizeof(lightgrid_raw_header_t))
    {
        const lightgrid_raw_header_t *hdr = (const lightgrid_raw_header_t *)payload;
        if (LittleLong(hdr->magic) == LIGHTGRID_RAW_MAGIC)
        {
            version = LittleLong(hdr->version);
            if (version != LIGHTGRID_RAW_VERSION_2)
                return false;

            components = LittleLong(hdr->components);
            encoding = LittleLong(hdr->encoding);
            nx = LittleLong(hdr->nx);
            ny = LittleLong(hdr->ny);
            nz = LittleLong(hdr->nz);
            cellSize = LittleFloat(hdr->cellSize);
            VectorCopy(hdr->origin, origin);
            origin[0] = LittleFloat(origin[0]);
            origin[1] = LittleFloat(origin[1]);
            origin[2] = LittleFloat(origin[2]);
            header_size = sizeof(*hdr);
        }
    }

    if (version == LIGHTGRID_RAW_VERSION_1)
    {
        if (payload_size < header_size)
            return false;

        nx = LittleLong(((int *)payload)[0]);
        ny = LittleLong(((int *)payload)[1]);
        nz = LittleLong(((int *)payload)[2]);

        cellSize = LG_FLittle(payload + sizeof(int) * 3);
        memcpy(origin, payload + sizeof(int) * 3 + sizeof(float), sizeof(vec3_t));
        origin[0] = LittleFloat(origin[0]);
        origin[1] = LittleFloat(origin[1]);
        origin[2] = LittleFloat(origin[2]);
    }

    if (nx <= 0 || ny <= 0 || nz <= 0)
        return false;

    if (cellSize <= 0.f)
        return false;

    if (version == LIGHTGRID_RAW_VERSION_2 &&
        (components & LIGHTGRID_RAW_COMPONENTS_BASE) != LIGHTGRID_RAW_COMPONENTS_BASE)
        return false;

    size_t count = (size_t)nx * (size_t)ny * (size_t)nz;
    if (!count || count > LIGHTGRID_MAX_CELLS || count > SIZE_MAX / sizeof(lightcell_t))
        return false;

    if (payload_size < header_size)
        return false;
    payload_size -= header_size;

    const size_t cell_stride = (version == LIGHTGRID_RAW_VERSION_1)
        ? sizeof(vec3_t) * 2 + sizeof(float)
        : LightgridRAW_CellSizeBytes(components, encoding);
    if (!cell_stride || payload_size < cell_stride * count)
        return false;

    lightgrid_raw_t *raw = (lightgrid_raw_t *)Z_Malloc(sizeof(*raw));
    if (!raw)
        return false;
    memset(raw, 0, sizeof(*raw));

    raw->cells = (lightcell_t *)Z_Malloc(count * sizeof(lightcell_t));
    if (!raw->cells)
    {
        Z_Free(raw);
        return false;
    }

    raw->nx = nx;
    raw->ny = ny;
    raw->nz = nz;
    raw->cellSize = cellSize;
    VectorCopy(origin, raw->origin);

    const byte *cell_bytes = payload + header_size;
    for (size_t i = 0; i < count; i++)
    {
        lightcell_t *cell = &raw->cells[i];
        const byte *component_ptr = cell_bytes;

        cell->ao = 1.f;
        cell->emissive = 0.f;
        cell->sh_valid = false;

#define READ_COMPONENT_FLOAT(out)                     \
        do {                                          \
            if (encoding == LIGHTGRID_RAW_ENCODING_FLOAT16) \
            {                                         \
                unsigned short half;                  \
                memcpy(&half, component_ptr, sizeof(half)); \
                half = LittleShort(half);             \
                out = LightgridRAW_HalfToFloat(half); \
                component_ptr += sizeof(unsigned short); \
            }                                         \
            else                                      \
            {                                         \
                memcpy(&out, component_ptr, sizeof(float)); \
                out = LittleFloat(out);               \
                component_ptr += sizeof(float);       \
            }                                         \
        } while (0)

        READ_COMPONENT_FLOAT(cell->rgb[0]);
        READ_COMPONENT_FLOAT(cell->rgb[1]);
        READ_COMPONENT_FLOAT(cell->rgb[2]);
        READ_COMPONENT_FLOAT(cell->dir[0]);
        READ_COMPONENT_FLOAT(cell->dir[1]);
        READ_COMPONENT_FLOAT(cell->dir[2]);
        READ_COMPONENT_FLOAT(cell->intensity);

        if (components & LIGHTGRID_RAW_COMPONENT_AO)
            READ_COMPONENT_FLOAT(cell->ao);

        if (components & LIGHTGRID_RAW_COMPONENT_EMISSIVE)
            READ_COMPONENT_FLOAT(cell->emissive);

        cell_bytes += cell_stride;
#undef READ_COMPONENT_FLOAT
    }

    lightgrid_t *lg = Lightgrid_FromRaw(raw);

    Z_Free(raw->cells);
    Z_Free(raw);

    if (!lg)
        return false;

    current_lightgrid = lg;
    cl.lightgrid      = lg;
    lg->source        = LIGHTGRID_SRC_OCTREE;
    q_strlcpy(lightgrid_source, "OCTREE", sizeof(lightgrid_source));

    return true;
}

/* =====================================================================
   KTX2 loader/export helpers
   ===================================================================== */

#if USE_KTX2_LIGHTGRID

typedef struct lightgrid_meta_s {
    int32_t nx, ny, nz;
    float cellsize;
    vec3_t mins;
    vec3_t maxs;
} lightgrid_meta_t;

typedef struct {
    uint32_t width, height, depth;
    uint32_t level_count;
    uint64_t dfd_offset, dfd_length;
    uint64_t kvd_offset, kvd_length;
    uint64_t sgd_offset, sgd_length;
    uint32_t vkformat;
    struct {
        uint64_t offset;
        uint64_t length;
        uint64_t uncompressed;
    } levels[8];
} lightgrid_ktx2_header_t;

static uint32_t LG_ReadU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t LG_ReadU64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static void LG_WriteU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void LG_WriteU64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
    p[4] = (uint8_t)((v >> 32) & 0xff);
    p[5] = (uint8_t)((v >> 40) & 0xff);
    p[6] = (uint8_t)((v >> 48) & 0xff);
    p[7] = (uint8_t)((v >> 56) & 0xff);
}

static qboolean Lightgrid_ParseKTX2(const uint8_t *data, size_t size, lightgrid_ktx2_header_t *out)
{
    static const uint8_t magic[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    const size_t header_size = 100;
    const size_t level_index_size = sizeof(uint64_t) * 3;
    size_t level_table_size;

    if (!data || size < header_size)
        return false;

    if (memcmp(data, magic, sizeof(magic)) != 0)
        return false;

    memset(out, 0, sizeof(*out));

    out->vkformat = LG_ReadU32(data + 12);
    /* typeSize is ignored (data assumed tightly packed float32) */
    out->width = LG_ReadU32(data + 12 + 4 * 2);
    out->height = LG_ReadU32(data + 12 + 4 * 3);
    out->depth = LG_ReadU32(data + 12 + 4 * 4);
    out->level_count = LG_ReadU32(data + 12 + 4 * 7);

    out->dfd_offset = LG_ReadU64(data + 12 + 4 * 8);
    out->dfd_length = LG_ReadU64(data + 12 + 4 * 8 + 8 * 1);
    out->kvd_offset = LG_ReadU64(data + 12 + 4 * 8 + 8 * 2);
    out->kvd_length = LG_ReadU64(data + 12 + 4 * 8 + 8 * 3);
    out->sgd_offset = LG_ReadU64(data + 12 + 4 * 8 + 8 * 4);
    out->sgd_length = LG_ReadU64(data + 12 + 4 * 8 + 8 * 5);

    if (out->level_count == 0 || out->level_count > (int)(sizeof(out->levels) / sizeof(out->levels[0])))
        return false;

    level_table_size = out->level_count * level_index_size;
    if (header_size + level_table_size > size)
        return false;

    for (uint32_t i = 0; i < out->level_count; i++)
    {
        const uint8_t *entry = data + header_size + i * level_index_size;
        uint64_t off = LG_ReadU64(entry + 0);
        uint64_t len = LG_ReadU64(entry + 8);
        uint64_t uncompressed = LG_ReadU64(entry + 16);

        if (off > size || len > size - off)
            return false;

        out->levels[i].offset = off;
        out->levels[i].length = len;
        out->levels[i].uncompressed = uncompressed;
    }

    return true;
}

static qboolean Lightgrid_WriteKTX2(const char *path, const float *payload, size_t payload_floats, int nx, int ny, int nz)
{
    const size_t header_size = 100;
    const size_t level_index_size = sizeof(uint64_t) * 3;
    const size_t level_count = 1;
    const size_t level_table_size = level_count * level_index_size;
    size_t data_offset = header_size + level_table_size;
    const size_t payload_bytes = payload_floats * sizeof(float);
    size_t total_size;
    uint8_t *buffer;

    data_offset = (data_offset + 7) & ~((size_t)7); /* align payload to 8 bytes */
    total_size = data_offset + payload_bytes;

    buffer = (uint8_t *)malloc(total_size);
    if (!buffer)
        return false;

    memset(buffer, 0, total_size);

    /* Magic */
    {
        static const uint8_t magic[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
        memcpy(buffer, magic, sizeof(magic));
    }

    LG_WriteU32(buffer + 12, 109); /* vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT */
    LG_WriteU32(buffer + 16, 4);   /* typeSize */
    LG_WriteU32(buffer + 20, (uint32_t)nx);
    LG_WriteU32(buffer + 24, (uint32_t)ny);
    LG_WriteU32(buffer + 28, (uint32_t)nz);
    LG_WriteU32(buffer + 32, 1); /* layerCount */
    LG_WriteU32(buffer + 36, 1); /* faceCount */
    LG_WriteU32(buffer + 40, 1); /* levelCount */

    /* supercompression + data/metadata offsets are zero */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 0, 0); /* supercompressionScheme */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 1, 0); /* dfdByteOffset */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 2, 0); /* dfdByteLength */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 3, 0); /* kvdByteOffset */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 4, 0); /* kvdByteLength */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 5, 0); /* sgdByteOffset */
    LG_WriteU64(buffer + 12 + 4 * 8 + 8 * 6, 0); /* sgdByteLength */

    /* Level index */
    LG_WriteU64(buffer + header_size + 0, data_offset);
    LG_WriteU64(buffer + header_size + 8, payload_bytes);
    LG_WriteU64(buffer + header_size + 16, payload_bytes);

    memcpy(buffer + data_offset, payload, payload_bytes);

    if (!COM_WriteFile_OSPath(path, buffer, total_size))
    {
        free(buffer);
        return false;
    }

    free(buffer);
    return true;
}

static qboolean Lightgrid_LoadMetadata(const char *mapname, lightgrid_meta_t *meta)
{
    char path[MAX_OSPATH];
    byte *buf;

    q_snprintf(path, sizeof(path), "maps/%s_lightgrid_meta.bin", mapname);
    buf = COM_LoadMallocFile(path, NULL);
    if (!buf)
        return false;

    if (com_filesize < (int)sizeof(*meta))
    {
        free(buf);
        return false;
    }

    memcpy(meta, buf, sizeof(*meta));
    free(buf);
    return true;
}

static qboolean Lightgrid_LoadKTXPayload(const char *path, int nx, int ny, int nz, float *out, size_t float_count)
{
    byte *buf = COM_LoadMallocFile(path, NULL);
    lightgrid_ktx2_header_t hdr;
    size_t expected_bytes = float_count * sizeof(float);

    if (!buf)
        return false;

    if (!Lightgrid_ParseKTX2(buf, (size_t)com_filesize, &hdr))
    {
        free(buf);
        return false;
    }

    if (hdr.vkformat != 109 /* VK_FORMAT_R32G32B32A32_SFLOAT */)
    {
        free(buf);
        return false;
    }

    if (hdr.width != (uint32_t)nx || hdr.height != (uint32_t)ny || hdr.depth != (uint32_t)nz || hdr.level_count != 1)
    {
        free(buf);
        return false;
    }

    if (hdr.levels[0].length < expected_bytes)
    {
        free(buf);
        return false;
    }

    memcpy(out, buf + hdr.levels[0].offset, expected_bytes);
    free(buf);
    return true;
}

static void Lightgrid_Upload3D(GLuint *tex, const float *payload, int nx, int ny, int nz)
{
    if (!*tex)
        glGenTextures(1, tex);

    glBindTexture(GL_TEXTURE_3D, *tex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    GL_TexImage3DFunc(GL_TEXTURE_3D, 0, GL_RGBA32F, nx, ny, nz, 0, GL_RGBA, GL_FLOAT, payload);
}

qboolean Lightgrid_LoadFromKTX2(const char *mapname)
{
    lightgrid_meta_t meta;
    lightgrid_t *lg;
    size_t count;
    size_t payload_floats;
    float *color_payload = NULL;
    float *dir_payload = NULL;
    char color_path[MAX_OSPATH];
    char dir_path[MAX_OSPATH];

    if (!mapname || !mapname[0])
        return false;

    if (!Lightgrid_LoadMetadata(mapname, &meta))
        return false;

    count = (size_t)meta.nx * (size_t)meta.ny * (size_t)meta.nz;
    if (count == 0 || count > LIGHTGRID_MAX_CELLS)
        return false;

    payload_floats = count * 4;
    color_payload = (float *)malloc(payload_floats * sizeof(float));
    dir_payload = (float *)malloc(payload_floats * sizeof(float));
    if (!color_payload || !dir_payload)
        goto fail;

    q_snprintf(color_path, sizeof(color_path), "maps/%s_lightgrid_color.ktx2", mapname);
    q_snprintf(dir_path, sizeof(dir_path), "maps/%s_lightgrid_dir.ktx2", mapname);

    if (!Lightgrid_LoadKTXPayload(color_path, meta.nx, meta.ny, meta.nz, color_payload, payload_floats))
        goto fail;
    if (!Lightgrid_LoadKTXPayload(dir_path, meta.nx, meta.ny, meta.nz, dir_payload, payload_floats))
        goto fail;

    lg = Lightgrid_Alloc(meta.nx, meta.ny, meta.nz, meta.cellsize, meta.mins, meta.maxs);
    if (!lg)
        goto fail;

    for (size_t i = 0; i < count; i++)
    {
        const float *c = &color_payload[i * 4];
        const float *d = &dir_payload[i * 4];
        lightgrid_probe_t *p = &lg->probes[i];

        VectorCopy(c, p->rgb);
        p->intensity = c[3];
        VectorCopy(d, p->dir);
        if (VectorNormalize(p->dir) == 0)
            VectorSet(p->dir, 0, 0, 1);
    }

    Lightgrid_Upload3D(&lg->tex_color3d, color_payload, meta.nx, meta.ny, meta.nz);
    Lightgrid_Upload3D(&lg->tex_dir3d, dir_payload, meta.nx, meta.ny, meta.nz);

    current_lightgrid = lg;
    cl.lightgrid = lg;
    lg->source = LIGHTGRID_SRC_KTX2;
    q_strlcpy(lightgrid_source, "KTX2", sizeof(lightgrid_source));

    free(color_payload);
    free(dir_payload);
    Con_Printf("Loaded KTX2 lightgrid for %s (%dx%dx%d)\n", mapname, meta.nx, meta.ny, meta.nz);
    return true;

fail:
    if (color_payload)
        free(color_payload);
    if (dir_payload)
        free(dir_payload);
    return false;
}

qboolean Lightgrid_ExportToKTX2(const lightgrid_t *grid, const char *mapname)
{
    lightgrid_meta_t meta;
    size_t count, payload_floats;
    float *color_payload = NULL;
    float *dir_payload = NULL;
    char color_path[MAX_OSPATH];
    char dir_path[MAX_OSPATH];
    char meta_path[MAX_OSPATH];
    qboolean ok = false;

    if (!grid || !mapname || !mapname[0])
        return false;

    count = (size_t)grid->nx * (size_t)grid->ny * (size_t)grid->nz;
    if (count == 0)
        return false;

    payload_floats = count * 4;
    color_payload = (float *)malloc(payload_floats * sizeof(float));
    dir_payload = (float *)malloc(payload_floats * sizeof(float));
    if (!color_payload || !dir_payload)
        goto cleanup;

    for (size_t i = 0; i < count; i++)
    {
        const lightgrid_probe_t *p = &grid->probes[i];
        float *c = &color_payload[i * 4];
        float *d = &dir_payload[i * 4];

        VectorCopy(p->rgb, c);
        c[3] = p->intensity;
        VectorCopy(p->dir, d);
        d[3] = 0.0f;
    }

    q_snprintf(color_path, sizeof(color_path), "%s/maps/%s_lightgrid_color.ktx2", com_gamedir, mapname);
    q_snprintf(dir_path, sizeof(dir_path), "%s/maps/%s_lightgrid_dir.ktx2", com_gamedir, mapname);
    q_snprintf(meta_path, sizeof(meta_path), "%s/maps/%s_lightgrid_meta.bin", com_gamedir, mapname);

    if (!Lightgrid_WriteKTX2(color_path, color_payload, payload_floats, grid->nx, grid->ny, grid->nz))
        goto cleanup;
    if (!Lightgrid_WriteKTX2(dir_path, dir_payload, payload_floats, grid->nx, grid->ny, grid->nz))
        goto cleanup;

    meta.nx = grid->nx;
    meta.ny = grid->ny;
    meta.nz = grid->nz;
    meta.cellsize = grid->cellsize;
    VectorCopy(grid->mins, meta.mins);
    VectorCopy(grid->maxs, meta.maxs);

    if (!COM_WriteFile_OSPath(meta_path, &meta, sizeof(meta)))
        goto cleanup;

    Con_Printf("Exported KTX2 lightgrid for %s (%dx%dx%d)\n", mapname, grid->nx, grid->ny, grid->nz);
    ok = true;

cleanup:
    if (color_payload)
        free(color_payload);
    if (dir_payload)
        free(dir_payload);
    return ok;
}
#endif

/* =====================================================================
   RAW -> Lightgrid Converter (used by fallback)
   ===================================================================== */

lightgrid_t *Lightgrid_FromRaw(const lightgrid_raw_t *raw)
{
    if (!raw || !raw->cells)
        return NULL;

    if (raw->nx <= 0 || raw->ny <= 0 || raw->nz <= 0)
        return NULL;

    size_t count = (size_t)raw->nx * raw->ny * raw->nz;

    vec3_t mins, maxs;
    VectorCopy(raw->origin, mins);
    VectorCopy(raw->origin, maxs);

    maxs[0] += raw->cellSize * raw->nx;
    maxs[1] += raw->cellSize * raw->ny;
    maxs[2] += raw->cellSize * raw->nz;

    lightgrid_t *lg =
        Lightgrid_Alloc(raw->nx, raw->ny, raw->nz, raw->cellSize, mins, maxs);

    if (!lg)
        return NULL;

    for (size_t i = 0; i < count; i++)
    {
        const lightcell_t *cell = &raw->cells[i];
        vec3_t dir;

        VectorCopy(cell->rgb, lg->probes[i].rgb);

        VectorCopy(cell->dir, dir);
        if (VectorNormalize(dir) == 0)
            VectorSet(dir, 0,0,1);

        VectorCopy(dir, lg->probes[i].dir);
        lg->probes[i].intensity = cell->intensity;
        lg->probes[i].emissive = cell->emissive;
    }

    lg->source = LIGHTGRID_SRC_RAW;
    q_strlcpy(lightgrid_source, "RAW", sizeof(lightgrid_source));
    return lg;
}

/* =====================================================================
   Sampling
   ===================================================================== */

static void Lightgrid_DefaultSample(vec3_t out_color, vec3_t out_dir)
{
    VectorSet(out_color, 1,1,1);
    VectorSet(out_dir, 0,0,1);
}

static const lightgrid_probe_t *Lightgrid_At(const lightgrid_t *lg, int x, int y, int z)
{
    return &lg->probes[
        (size_t)z * lg->ny * lg->nx +
        (size_t)y * lg->nx +
        x
    ];
}

void Lightgrid_Sample(const vec3_t pos, vec3_t out_color, vec3_t out_dir)
{
    const lightgrid_t *lg = Lightgrid_Get();

    if (!lg || !lg->probes || lg->cellsize <= 0.f || !r_lightgrid.value)
    {
        Lightgrid_DefaultSample(out_color, out_dir);
        return;
    }

    vec3_t p;
    VectorCopy(pos, p);

    for (int i = 0; i < 3; i++)
        p[i] = CLAMP(lg->mins[i], p[i], lg->maxs[i]);

    float fx = (p[0] - lg->mins[0]) / lg->cellsize;
    float fy = (p[1] - lg->mins[1]) / lg->cellsize;
    float fz = (p[2] - lg->mins[2]) / lg->cellsize;

    int x0 = CLAMP(0, (int)floorf(fx), lg->nx - 1);
    int y0 = CLAMP(0, (int)floorf(fy), lg->ny - 1);
    int z0 = CLAMP(0, (int)floorf(fz), lg->nz - 1);

    int x1 = q_min(x0 + 1, lg->nx - 1);
    int y1 = q_min(y0 + 1, lg->ny - 1);
    int z1 = q_min(z0 + 1, lg->nz - 1);

    fx -= floorf(fx);
    fy -= floorf(fy);
    fz -= floorf(fz);

    const lightgrid_probe_t *c000 = Lightgrid_At(lg, x0, y0, z0);
    const lightgrid_probe_t *c100 = Lightgrid_At(lg, x1, y0, z0);
    const lightgrid_probe_t *c010 = Lightgrid_At(lg, x0, y1, z0);
    const lightgrid_probe_t *c110 = Lightgrid_At(lg, x1, y1, z0);
    const lightgrid_probe_t *c001 = Lightgrid_At(lg, x0, y0, z1);
    const lightgrid_probe_t *c101 = Lightgrid_At(lg, x1, y0, z1);
    const lightgrid_probe_t *c011 = Lightgrid_At(lg, x0, y1, z1);
    const lightgrid_probe_t *c111 = Lightgrid_At(lg, x1, y1, z1);

    vec3_t rgb000,rgb100,rgb010,rgb110,rgb001,rgb101,rgb011,rgb111;
    vec3_t dir000,dir100,dir010,dir110,dir001,dir101,dir011,dir111;
    vec3_t c00,c10,c01,c11,c0,c1,c;
    vec3_t d00,d10,d01,d11,d0,d1,d;

    #define PREMUL(dst, p) \
        VectorScale(p->rgb, p->intensity, dst)

    PREMUL(rgb000, c000);
    PREMUL(rgb100, c100);
    PREMUL(rgb010, c010);
    PREMUL(rgb110, c110);
    PREMUL(rgb001, c001);
    PREMUL(rgb101, c101);
    PREMUL(rgb011, c011);
    PREMUL(rgb111, c111);

    #undef PREMUL

    #define PREMUL_DIR(dst,p) VectorScale(p->dir, p->intensity, dst)

    PREMUL_DIR(dir000, c000);
    PREMUL_DIR(dir100, c100);
    PREMUL_DIR(dir010, c010);
    PREMUL_DIR(dir110, c110);
    PREMUL_DIR(dir001, c001);
    PREMUL_DIR(dir101, c101);
    PREMUL_DIR(dir011, c011);
    PREMUL_DIR(dir111, c111);

    #undef PREMUL_DIR

    VectorLerp(rgb000, rgb100, fx, c00);
    VectorLerp(rgb010, rgb110, fx, c10);
    VectorLerp(rgb001, rgb101, fx, c01);
    VectorLerp(rgb011, rgb111, fx, c11);

    VectorLerp(c00, c10, fy, c0);
    VectorLerp(c01, c11, fy, c1);

    VectorLerp(c0, c1, fz, c);

    VectorLerp(dir000, dir100, fx, d00);
    VectorLerp(dir010, dir110, fx, d10);
    VectorLerp(dir001, dir101, fx, d01);
    VectorLerp(dir011, dir111, fx, d11);

    VectorLerp(d00, d10, fy, d0);
    VectorLerp(d01, d11, fy, d1);

    VectorLerp(d0, d1, fz, d);

    if (VectorNormalize(d) == 0)
        VectorSet(d,0,0,1);

    VectorCopy(c, out_color);
    VectorCopy(d, out_dir);
}

/* =====================================================================
   Random & Fill
   ===================================================================== */

static float Lightgrid_Random01(void)
{
    return (float)rand() / (float)RAND_MAX;
}

static void Lightgrid_RandomHemisphereDir(vec3_t out)
{
    const float u = Lightgrid_Random01();
    const float v = Lightgrid_Random01();
    const float phi = 2.f * (float)M_PI * u;
    const float cos_theta = v;
    const float sin_theta = sqrtf(q_max(0.f, 1.f - cos_theta * cos_theta));

    out[0] = cosf(phi) * sin_theta;
    out[1] = sinf(phi) * sin_theta;
    out[2] = fabsf(cos_theta);
}

static qboolean Lightgrid_TraceLine(const qmodel_t *mod, const vec3_t start, const vec3_t end, vec3_t impact)
{
    trace_t trace;

    if (!mod || !mod->hulls || !mod->hulls[0].planes)
    {
        if (impact)
            VectorCopy(end, impact);
        return true;
    }

    if (impact)
        VectorCopy(end, impact);

    memset(&trace, 0, sizeof(trace));

    vec3_t mutable_start, mutable_end;
    VectorCopy(start, mutable_start);
    VectorCopy(end, mutable_end);

    SV_RecursiveHullCheck(mod->hulls, 0, 0, 1, mutable_start, mutable_end, &trace);

    if (impact)
        VectorCopy(trace.endpos, impact);

    vec3_t delta;
    VectorSubtract(end, trace.endpos, delta);
    return VectorLength(delta) < 1.f;
}

void SH9_EncodeDirectional(vec3_t dir, vec3_t rgb, sh9_color_t *out)
{
    // Stub for future SH9 encoding implementation.
    (void)dir;
    (void)rgb;
    (void)out;
}

static void Lightgrid_FillRandomCell(lightcell_t *cell)
{
    cell->rgb[0] = Lightgrid_Random01();
    cell->rgb[1] = Lightgrid_Random01();
    cell->rgb[2] = Lightgrid_Random01();

    VectorSet(cell->dir,0,0,1);
    cell->intensity = (cell->rgb[0] + cell->rgb[1] + cell->rgb[2]) / 3.f;
    cell->ao = 1.f;
    cell->emissive = 0.f;
    memset(&cell->sh, 0, sizeof(cell->sh));
    cell->sh_valid = false;
}

/* =====================================================================
   RAW Lightgrid generator (fallback)
   ===================================================================== */

static void Lightgrid_ComputeDims(const vec3_t size, float cellSize, int *nx, int *ny, int *nz)
{
    *nx = q_max(1, q_min(LIGHTGRID_MAX_NX, (int)ceilf(size[0] / cellSize)));
    *ny = q_max(1, q_min(LIGHTGRID_MAX_NY, (int)ceilf(size[1] / cellSize)));
    *nz = q_max(1, q_min(LIGHTGRID_MAX_NZ, (int)ceilf(size[2] / cellSize)));
}

lightgrid_raw_t *Lightgrid_GenerateRaw(const struct qmodel_s *model)
{
    if (!model)
        return NULL;

    /*
     * Disable any currently loaded lightgrid while we bake a new one so that
     * sampling uses the actual BSP light data instead of feeding back the
     * existing (and possibly empty) grid. Otherwise R_LightPoint() would hit
     * the active lightgrid path and return the default white sample, producing
     * blank probes.
     */
    lightgrid_t *saved_current = current_lightgrid;
    lightgrid_t *saved_cl      = cl.lightgrid;
    float saved_r_lightgrid    = r_lightgrid.value;
    float saved_r_lightgrid_force = r_lightgrid_force.value;

    current_lightgrid = NULL;
    cl.lightgrid      = NULL;
    r_lightgrid.value = 0.f;
    r_lightgrid_force.value = 0.f;

    vec3_t mins, maxs, size;
    VectorCopy(model->mins, mins);
    VectorCopy(model->maxs, maxs);
    VectorSubtract(maxs, mins, size);

    float cellSize = LIGHTGRID_STANDARD_CELLSIZE;

    int nx, ny, nz;
    Lightgrid_ComputeDims(size, cellSize, &nx, &ny, &nz);

    while ((size_t)nx * ny * nz > LIGHTGRID_MAX_CELLS)
    {
        cellSize *= 2.f;
        Lightgrid_ComputeDims(size, cellSize, &nx, &ny, &nz);
    }

        lightgrid_raw_t *raw =
        (lightgrid_raw_t*)Hunk_AllocName(sizeof(*raw), "lightgrid_raw");
    memset(raw, 0, sizeof(*raw));

    raw->nx = nx;
    raw->ny = ny;
    raw->nz = nz;
    raw->cellSize = cellSize;
    raw->has_sh9 = (r_lightgrid_sh9.value != 0.f);
    VectorCopy(mins, raw->origin);

    size_t count = (size_t)nx * ny * nz;
    raw->cells =
        (lightcell_t*)Hunk_AllocName(count * sizeof(lightcell_t), "lightgrid_cells");

    for (int z = 0; z < nz; z++)
    for (int y = 0; y < ny; y++)
    for (int x = 0; x < nx; x++)
    {
        lightcell_t *cell = &raw->cells[(z * ny + y) * nx + x];

        vec3_t pos = {
            mins[0] + (x + 0.5f) * cellSize,
            mins[1] + (y + 0.5f) * cellSize,
            mins[2] + (z + 0.5f) * cellSize
        };

        vec3_t jitter = {
            (Lightgrid_Random01() * 4.f) - 2.f,
            (Lightgrid_Random01() * 4.f) - 2.f,
            (Lightgrid_Random01() * 4.f) - 2.f
        };
        VectorAdd(pos, jitter, pos);

        static const vec3_t luminance_weights = {0.299f, 0.587f, 0.114f};
        const qboolean sh_enabled = r_lightgrid_sh9.value != 0.f;

        memset(&cell->sh, 0, sizeof(cell->sh));
        cell->sh_valid = sh_enabled;

        if (r_generate_lightgrid_test.value > 0.f)
        {
            Lightgrid_FillRandomCell(cell);
            continue;
        }

        lightcache_t cache = {0};
        vec3_t direct = {0, 0, 0};
        vec3_t lightmap = {0, 0, 0};
        vec3_t dirsum = {0,0,0};

        R_LightPoint((qmodel_t *)model, pos, 0.f, &cache);

        direct[0] = lightcolor[0] * (1.f/255.f);
        direct[1] = lightcolor[1] * (1.f/255.f);
        direct[2] = lightcolor[2] * (1.f/255.f);

        R_SampleLightmapAtPoint(pos, lightmap);

        if (r_lightgrid_surface_weight.value != 0.f && cache.surfidx > 0 && cache.surfidx <= model->numsurfaces)
        {
                const msurface_t *surf = &model->surfaces[cache.surfidx - 1];
                vec3_t center;
                for (int i = 0; i < 3; i++)
                        center[i] = 0.5f * (surf->mins[i] + surf->maxs[i]);

                vec3_t delta;
                VectorSubtract(pos, center, delta);
                float dist = VectorLength(delta);
                float weight = 1.f / (1.f + dist);
                VectorScale(lightmap, weight, lightmap);
        }

        VectorClear(cell->rgb);
        VectorMA(cell->rgb, r_lightgrid_weight_direct.value, direct, cell->rgb);
        VectorMA(cell->rgb, r_lightgrid_weight_lightmap.value, lightmap, cell->rgb);

        Lightgrid_AddDlights(pos, cell->rgb, dirsum);

        /* Second bounce approximation */
        const int bounce_rays = q_max(0, (int)r_lightgrid_bounce_rays.value);
        if (bounce_rays > 0 && r_lightgrid_bounce_factor.value > 0.f)
        {
            vec3_t bounce_accum = {0, 0, 0};
            const float bounce_scale = r_lightgrid_bounce_factor.value / (float)bounce_rays;
            for (int i = 0; i < bounce_rays; i++)
            {
                vec3_t raydir;
                Lightgrid_RandomHemisphereDir(raydir);

                vec3_t end;
                VectorMA(pos, 512.f, raydir, end);

                vec3_t impact;
                qboolean unobstructed = Lightgrid_TraceLine((qmodel_t *)model, pos, end, impact);
                if (unobstructed)
                    continue;

                vec3_t hit_color;
                if (R_SampleLightmapAtPoint(impact, hit_color))
                {
                    VectorMA(bounce_accum, bounce_scale, hit_color, bounce_accum);

                    if (sh_enabled)
                    {
                        sh9_color_t coeff = {{{0}}};
                        SH9_EncodeDirectional(raydir, hit_color, &coeff);
                        for (int c = 0; c < 9; c++)
                            VectorAdd(cell->sh.c[c], coeff.c[c], cell->sh.c[c]);
                    }
                }
            }

            VectorAdd(cell->rgb, bounce_accum, cell->rgb);
        }

        vec3_t up = {0,0,1};
        float base = DotProduct(cell->rgb, luminance_weights);
        VectorMA(dirsum, base, up, dirsum);

        float dirlen = VectorNormalize(dirsum);
        if (dirlen < 1e-6f || !R_IsFinite(dirlen))
            VectorSet(cell->dir,0,0,1);
        else
            VectorCopy(dirsum, cell->dir);

        cell->intensity = base;
    }

    /* Restore previous lightgrid state */
    r_lightgrid.value = saved_r_lightgrid;
    r_lightgrid_force.value = saved_r_lightgrid_force;
    current_lightgrid = saved_current;
    cl.lightgrid      = saved_cl;

    return raw;
}

/* =====================================================================
   Fallback builder
   ===================================================================== */

void Lightgrid_BuildFallback(void)
{
    if (!cl.worldmodel)
        return;

    Con_Printf("Lightgrid fallback: building grid...\n");

    lightgrid_raw_t *raw = Lightgrid_GenerateRaw(cl.worldmodel);
    if (!raw)
    {
        Con_Printf("fallback failed: allocation error\n");
        return;
    }

    cl.worldmodel->lightgrid_raw = raw;

    Lightgrid_Clear();

    lightgrid_t *lg = Lightgrid_FromRaw(raw);
    if (!lg)
    {
        Con_Printf("fallback failed: conversion error\n");
        return;
    }

    current_lightgrid = lg;
    cl.lightgrid      = lg;

#if USE_KTX2_LIGHTGRID
    if (r_lightgrid_ktx_enable.value && r_lightgrid_ktx_export.value)
    {
        char mapname[MAX_QPATH];
        COM_StripExtension(cl.worldmodel->name, mapname, sizeof(mapname));
        Lightgrid_ExportToKTX2(lg, COM_SkipPath(mapname));
    }
#endif

    Con_Printf("fallback done (%dx%dx%d)\n", raw->nx, raw->ny, raw->nz);
}

/* =====================================================================
   Dlight integration
   ===================================================================== */

static void Lightgrid_AddDlights(const vec3_t pos, vec3_t color, vec3_t dirsum)
{
    for (int i = 0; i < MAX_DLIGHTS; i++)
    {
        const dlight_t *l = &cl_dlights[i];

        if (l->die <= cl.time || (!l->radius && !l->minlight))
            continue;

        vec3_t delta;
        VectorSubtract(pos, l->origin, delta);

        float dist2 = DotProduct(delta, delta);
        if (dist2 >= l->radius * l->radius)
            continue;

        float add = l->radius - sqrtf(dist2);
        if (add <= l->minlight)
            continue;

        float scale = add * (1.f / 256.f);
        VectorMA(color, scale, l->color, color);

        if (VectorNormalize(delta) > 0)
            VectorMA(dirsum, add, delta, dirsum);
    }
}

/* =====================================================================
   Public API
   ===================================================================== */

const lightgrid_t *Lightgrid_Get(void)
{
    return current_lightgrid ? current_lightgrid : cl.lightgrid;
}

void Lightgrid_SetSource(const char *name)
{
    if (name && name[0])
         q_strlcpy(lightgrid_source, name, sizeof(lightgrid_source));
    else q_strlcpy(lightgrid_source, "UNKNOWN", sizeof(lightgrid_source));

    if (current_lightgrid)
    {
        if (name && !q_strcasecmp(name, "KTX2"))
            current_lightgrid->source = LIGHTGRID_SRC_KTX2;
        else if (name && !q_strcasecmp(name, "RAW"))
            current_lightgrid->source = LIGHTGRID_SRC_RAW;
        else if (name && !q_strcasecmp(name, "OCTREE"))
            current_lightgrid->source = LIGHTGRID_SRC_OCTREE;
        else
            current_lightgrid->source = LIGHTGRID_SRC_NONE;
    }
}

const char *Lightgrid_GetSource(void)
{
    return lightgrid_source;
}
