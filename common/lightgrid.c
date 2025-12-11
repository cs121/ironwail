#include "quakedef.h"
#include "lightgrid.h"

#define LIGHTGRID_MAGIC 0x4c475244u /* 'LGRD' */
#define LIGHTGRID_VERSION_V2 2u

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

static qboolean Lightgrid_ReadV2Header(const lightgrid_v2_header_t *hdr, int *nx, int *ny, int *nz, float *cellsize, vec3_t origin)
{
    if (LittleLong(hdr->magic) != LIGHTGRID_MAGIC)
        return false;

    if (LittleLong(hdr->version) != LIGHTGRID_VERSION_V2)
        return false;

    *nx = LittleLong(hdr->nx);
    *ny = LittleLong(hdr->ny);
    *nz = LittleLong(hdr->nz);
    *cellsize = LittleFloat(hdr->cellsize);

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

    if (!Lightgrid_ReadV2Header(hdr, &nx, &ny, &nz, &cellsize, origin))
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

    const qboolean has_ao = (floats_per_probe == 8);
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

    lg->source = LIGHTGRID_SRC_RAW;

    free(buffer);
    return lg;
}

lightgrid_t *Lightgrid_LoadExternal(const char *path)
{
    return Lightgrid_LoadV2(path);
}
