#include "quakedef.h"
#include "lightgrid.h"

#define LIGHTGRID_MAGIC 0x4c475244u /* 'LGRD' */
#define LIGHTGRID_VERSION_V2 2u
#define KTX2_HEADER_SIZE 100

static const uint8_t KTX2_MAGIC[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

typedef struct lightgrid_v2_header_s
{
    uint32_t magic;
    uint32_t version;
    uint32_t components;
    uint32_t reserved[5];
    int32_t nx, ny, nz;
    float cellsize;
    vec3_t origin;
} lightgrid_v2_header_t;

lightgrid_t *Lightgrid_Alloc(int nx, int ny, int nz, float cellsize, const vec3_t mins, const vec3_t maxs)
{
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return NULL;

    const size_t total = (size_t)nx * ny * nz;
    if (total > SIZE_MAX / sizeof(lightcell_t))
        return NULL;

    lightgrid_t *lg = (lightgrid_t *)Hunk_AllocName(sizeof(lightgrid_t), "lightgrid");
    if (!lg)
        return NULL;

    memset(lg, 0, sizeof(*lg));
    lg->probes = (lightcell_t *)Hunk_AllocName(total * sizeof(lightcell_t), "lightgrid_probes");
    if (!lg->probes)
        return NULL;

    lg->nx = nx;
    lg->ny = ny;
    lg->nz = nz;
    lg->cellsize = cellsize;
    VectorCopy(mins, lg->mins);
    VectorCopy(maxs, lg->maxs);

    lg->source = LIGHTGRID_SRC_NONE;
    lg->tex_color3d = 0;
    lg->tex_dir3d = 0;
    lg->tex_intensity = 0;
    lg->tex_ao = 0;

    return lg;
}

void Lightgrid_Free(lightgrid_t *lg)
{
    if (!lg)
        return;
}

static qboolean Lightgrid_ReadV2Header(const lightgrid_v2_header_t *hdr, int *nx, int *ny, int *nz, float *cellsize, vec3_t origin, uint32_t *components)
{
    if (LittleLong(hdr->magic) != LIGHTGRID_MAGIC)
        return false;

    if (LittleLong(hdr->version) != LIGHTGRID_VERSION_V2)
        return false;

    *nx = LittleLong(hdr->nx);
    *ny = LittleLong(hdr->ny);
    *nz = LittleLong(hdr->nz);
    *cellsize = LittleFloat(hdr->cellsize);
    *components = LittleLong(hdr->components);

    origin[0] = LittleFloat(hdr->origin[0]);
    origin[1] = LittleFloat(hdr->origin[1]);
    origin[2] = LittleFloat(hdr->origin[2]);

    return true;
}

lightgrid_t *Lightgrid_LoadV2(const char *path)
{
    if (!path)
        return NULL;

    byte *buffer = COM_LoadMallocFile(path, NULL);
    if (!buffer)
        return NULL;

    const size_t buffer_size = (size_t)com_filesize;
    if (buffer_size < sizeof(lightgrid_v2_header_t))
    {
        free(buffer);
        return NULL;
    }

    const lightgrid_v2_header_t *hdr = (const lightgrid_v2_header_t *)buffer;
    int nx, ny, nz;
    float cellsize;
    vec3_t origin;

    uint32_t components = 0;

    if (!Lightgrid_ReadV2Header(hdr, &nx, &ny, &nz, &cellsize, origin, &components))
    {
        free(buffer);
        return NULL;
    }

    if (cellsize <= 0.f)
    {
        free(buffer);
        return NULL;
    }

    vec3_t mins, maxs;
    VectorCopy(origin, mins);
    maxs[0] = origin[0] + cellsize * nx;
    maxs[1] = origin[1] + cellsize * ny;
    maxs[2] = origin[2] + cellsize * nz;

    if (nx <= 0 || ny <= 0 || nz <= 0)
    {
        free(buffer);
        return NULL;
    }

    const size_t probe_count = (size_t)nx * ny * nz;
    const size_t header_size = sizeof(lightgrid_v2_header_t);
    const size_t payload_bytes = buffer_size - header_size;

    if (probe_count == 0 || payload_bytes == 0)
    {
        free(buffer);
        return NULL;
    }

    const size_t floats_per_probe = payload_bytes / (sizeof(float) * probe_count);
    if (payload_bytes % (sizeof(float) * probe_count) != 0 || (floats_per_probe != 7 && floats_per_probe != 8))
    {
        free(buffer);
        return NULL;
    }

    const uint32_t required_components = LIGHTGRID_COMPONENT_RGB | LIGHTGRID_COMPONENT_DIR | LIGHTGRID_COMPONENT_INTENSITY;
    const uint32_t known_components = required_components | LIGHTGRID_COMPONENT_AO;
    const uint32_t unknown_components = components & ~(known_components | LIGHTGRID_COMPONENT_SH9_FLAG);

    if (components && (components & required_components) != required_components)
    {
        free(buffer);
        return NULL;
    }

    if (unknown_components)
        Con_DPrintf("Lightgrid V2: ignoring unknown component flags 0x%x\n", unknown_components);

    const qboolean has_ao = components ? ((components & LIGHTGRID_COMPONENT_AO) != 0) : (floats_per_probe == 8);

    if (components)
    {
        const size_t expected_floats = 7 + (has_ao ? 1 : 0);
        if (expected_floats != floats_per_probe)
        {
            free(buffer);
            return NULL;
        }
    }
    const float *probe_data = (const float *)(buffer + header_size);

    lightgrid_t *lg = Lightgrid_Alloc(nx, ny, nz, cellsize, mins, maxs);
    if (!lg)
    {
        free(buffer);
        return NULL;
    }

    for (size_t i = 0; i < probe_count; i++)
    {
        const float *p = probe_data + floats_per_probe * i;
        lightcell_t *cell = &lg->probes[i];

        cell->rgb[0] = CLAMP(0.f, LittleFloat(p[0]), 1.f);
        cell->rgb[1] = CLAMP(0.f, LittleFloat(p[1]), 1.f);
        cell->rgb[2] = CLAMP(0.f, LittleFloat(p[2]), 1.f);

        cell->dir[0] = LittleFloat(p[3]);
        cell->dir[1] = LittleFloat(p[4]);
        cell->dir[2] = LittleFloat(p[5]);
        if (VectorNormalize(cell->dir) == 0.f)
        {
            cell->dir[0] = 0.f;
            cell->dir[1] = 0.f;
            cell->dir[2] = 1.f;
        }

        cell->intensity = LittleFloat(p[6]);
        cell->ao = has_ao ? LittleFloat(p[7]) : 0.f;
    }

    lg->source = LIGHTGRID_SRC_V2;

    free(buffer);
    return lg;
}

typedef struct lightgrid_ktx2_level_s
{
    uint64_t offset;
    uint64_t length;
} lightgrid_ktx2_level_t;

typedef struct lightgrid_ktx2_header_s
{
    uint32_t vkformat;
    uint32_t width, height, depth;
    uint32_t level_count;
    uint64_t kvd_offset, kvd_length;
    lightgrid_ktx2_level_t levels[16];
} lightgrid_ktx2_header_t;

enum
{
    VK_FORMAT_R16G16B16_SFLOAT = 96,
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109
};

static uint32_t Lightgrid_KTX2ReadU32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t Lightgrid_KTX2ReadU64(const uint8_t *p)
{
    return ((uint64_t)p[0]) | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static qboolean Lightgrid_KTX2ParseHeader(const uint8_t *data, size_t size, lightgrid_ktx2_header_t *out)
{
    const size_t level_index_size = sizeof(uint64_t) * 3;
    size_t level_table_size;

    if (!data || size < KTX2_HEADER_SIZE)
        return false;

    if (memcmp(data, KTX2_MAGIC, sizeof(KTX2_MAGIC)) != 0)
        return false;

    memset(out, 0, sizeof(*out));

    out->vkformat = Lightgrid_KTX2ReadU32(data + 12);
    out->width = Lightgrid_KTX2ReadU32(data + 12 + 4 * 2);
    out->height = Lightgrid_KTX2ReadU32(data + 12 + 4 * 3);
    out->depth = Lightgrid_KTX2ReadU32(data + 12 + 4 * 4);
    out->level_count = Lightgrid_KTX2ReadU32(data + 12 + 4 * 7);

    out->kvd_offset = Lightgrid_KTX2ReadU64(data + 12 + 4 * 8 + 8 * 2);
    out->kvd_length = Lightgrid_KTX2ReadU64(data + 12 + 4 * 8 + 8 * 3);

    if (out->level_count == 0 || out->level_count > (int)(sizeof(out->levels) / sizeof(out->levels[0])))
        return false;

    level_table_size = out->level_count * level_index_size;
    if (KTX2_HEADER_SIZE + level_table_size > size)
        return false;

    for (uint32_t i = 0; i < out->level_count; i++)
    {
        const uint8_t *entry = data + KTX2_HEADER_SIZE + i * level_index_size;
        uint64_t offset = Lightgrid_KTX2ReadU64(entry + 0);
        uint64_t length = Lightgrid_KTX2ReadU64(entry + 8);

        if (offset > size || length > size - offset)
            return false;

        out->levels[i].offset = offset;
        out->levels[i].length = length;
    }

    if (out->kvd_length && (out->kvd_offset > size || out->kvd_length > size - out->kvd_offset))
        return false;

    return true;
}

static qboolean Lightgrid_KTX2ReadMetadata(const uint8_t *data, const lightgrid_ktx2_header_t *hdr, float *out_cellsize, vec3_t origin)
{
    qboolean have_cellsize = false;
    qboolean have_origin = false;
    qboolean have_sh9 = false;
    size_t offset = 0;

    if (!hdr->kvd_length)
        return false;

    while (offset + 4 <= hdr->kvd_length)
    {
        uint32_t kv_size = Lightgrid_KTX2ReadU32(data + offset);
        size_t padded = (kv_size + 3u) & ~3u;
        const uint8_t *kv = data + offset + 4;

        if (kv_size == 0 || offset + 4 + padded > hdr->kvd_length)
            break;

        const char *key = (const char *)kv;
        size_t key_len = strlen(key);
        if (key_len + 1 > kv_size)
            break;

        const uint8_t *value = kv + key_len + 1;
        size_t value_len = kv_size - (key_len + 1);

        if (!have_cellsize && !q_strcasecmp(key, "LGRID_CELLSIZE") && value_len >= sizeof(float))
        {
            float v;
            memcpy(&v, value, sizeof(float));
            *out_cellsize = LittleFloat(v);
            have_cellsize = true;
        }
        else if (!have_origin && !q_strcasecmp(key, "LGRID_ORIGIN") && value_len >= sizeof(vec3_t))
        {
            float v[3];
            memcpy(v, value, sizeof(vec3_t));
            origin[0] = LittleFloat(v[0]);
            origin[1] = LittleFloat(v[1]);
            origin[2] = LittleFloat(v[2]);
            have_origin = true;
        }
        else if (!have_sh9 && !q_strcasecmp(key, "LGRID_SH9"))
        {
            have_sh9 = true;
        }

        offset += 4 + padded;
    }

    if (have_sh9)
        Con_DPrintf("Lightgrid KTX2: detected LGRID_SH9 metadata (ignored for now)\n");

    return have_cellsize && have_origin;
}

lightgrid_t *Lightgrid_LoadKTX2(const char *path)
{
    byte *buffer;
    lightgrid_ktx2_header_t hdr;
    int nx, ny, nz;
    vec3_t origin, mins, maxs;
    float cellsize = 0.f;
    size_t component_count;
    size_t component_size;
    const uint8_t *payload;
    size_t payload_len;
    size_t probe_count;
    lightgrid_t *lg;

    if (!path)
        return NULL;

    buffer = COM_LoadMallocFile(path, NULL);
    if (!buffer)
        return NULL;

    if (!Lightgrid_KTX2ParseHeader(buffer, (size_t)com_filesize, &hdr))
    {
        free(buffer);
        return NULL;
    }

    if (hdr.depth == 0 || hdr.level_count == 0)
    {
        free(buffer);
        return NULL;
    }

    switch (hdr.vkformat)
    {
    case VK_FORMAT_R16G16B16_SFLOAT:
        component_count = 3;
        component_size = sizeof(uint16_t);
        break;
    case VK_FORMAT_R32G32B32_SFLOAT:
        component_count = 3;
        component_size = sizeof(float);
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        component_count = 4;
        component_size = sizeof(uint16_t);
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        component_count = 4;
        component_size = sizeof(float);
        break;
    default:
        free(buffer);
        return NULL;
    }

    nx = (int)hdr.width;
    ny = (int)hdr.height;
    nz = (int)hdr.depth;
    if (nx <= 0 || ny <= 0 || nz <= 0)
    {
        free(buffer);
        return NULL;
    }

    if (!Lightgrid_KTX2ReadMetadata(buffer + hdr.kvd_offset, &hdr, &cellsize, origin))
    {
        free(buffer);
        return NULL;
    }

    if (cellsize <= 0.f)
    {
        free(buffer);
        return NULL;
    }

    probe_count = (size_t)nx * (size_t)ny * (size_t)nz;
    if (probe_count == 0 || probe_count > LIGHTGRID_MAX_CELLS)
    {
        free(buffer);
        return NULL;
    }

    payload = buffer + hdr.levels[0].offset;
    payload_len = (size_t)hdr.levels[0].length;

    if ((size_t)component_size > SIZE_MAX / component_count ||
        component_count * component_size > SIZE_MAX / probe_count)
    {
        free(buffer);
        return NULL;
    }

    if (payload_len < probe_count * component_count * component_size)
    {
        free(buffer);
        return NULL;
    }

    VectorCopy(origin, mins);
    maxs[0] = origin[0] + cellsize * nx;
    maxs[1] = origin[1] + cellsize * ny;
    maxs[2] = origin[2] + cellsize * nz;

    lg = Lightgrid_Alloc(nx, ny, nz, cellsize, mins, maxs);
    if (!lg)
    {
        free(buffer);
        return NULL;
    }

    for (size_t i = 0; i < probe_count; i++)
    {
        const uint8_t *texel = payload + i * component_count * component_size;
        lightcell_t *cell = &lg->probes[i];
        float rgb[3];
        float alpha = 0.f;

        if (component_size == sizeof(uint16_t))
        {
            rgb[0] = LightgridRAW_HalfToFloat(((const uint16_t *)texel)[0]);
            rgb[1] = LightgridRAW_HalfToFloat(((const uint16_t *)texel)[1]);
            rgb[2] = LightgridRAW_HalfToFloat(((const uint16_t *)texel)[2]);
            if (component_count == 4)
                alpha = LightgridRAW_HalfToFloat(((const uint16_t *)texel)[3]);
        }
        else
        {
            rgb[0] = LittleFloat(((const float *)texel)[0]);
            rgb[1] = LittleFloat(((const float *)texel)[1]);
            rgb[2] = LittleFloat(((const float *)texel)[2]);
            if (component_count == 4)
                alpha = LittleFloat(((const float *)texel)[3]);
        }

        VectorCopy(rgb, cell->rgb);
        cell->dir[0] = 0.f;
        cell->dir[1] = 0.f;
        cell->dir[2] = 1.f;
        cell->ao = 0.f;

        if (component_count == 3)
            cell->intensity = sqrtf(rgb[0] * rgb[0] + rgb[1] * rgb[1] + rgb[2] * rgb[2]);
        else
            cell->intensity = alpha;
    }

    lg->source = LIGHTGRID_SRC_KTX2;

    free(buffer);
    return lg;
}

// Placeholder for future Lightgrid V3 loader.
lightgrid_t *Lightgrid_LoadV3(const char *path)
{
    (void)path;
    return NULL;
}

lightgrid_t *Lightgrid_LoadExternal(const char *path)
{
    if (!path)
        return NULL;

    typedef lightgrid_t *(*lightgrid_loader_fn)(const char *path);
    static lightgrid_loader_fn loaders[] = {
        Lightgrid_LoadV2,
        Lightgrid_LoadKTX2,
        Lightgrid_LoadV3,
        NULL
    };

    for (int i = 0; loaders[i]; ++i)
    {
        lightgrid_t *lg = loaders[i](path);
        if (lg)
            return lg;
    }

    return NULL;
}
