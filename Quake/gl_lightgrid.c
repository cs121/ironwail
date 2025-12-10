/*
Copyright (C) 2024 Ironwail developers
*/

#include "quakedef.h"
#include "gl_lightgrid.h"
#include "gl_model.h"
#include "glquake.h"

extern vec3_t lightcolor;

static lightgrid_t *current_lightgrid;
static char lightgrid_source[16] = "NONE";

cvar_t  r_lightgrid = { "r_lightgrid", "1", CVAR_ARCHIVE };
cvar_t  r_lightgrid_debug = { "r_lightgrid_debug", "0", CVAR_NONE };
cvar_t  r_generate_lightgrid_test = { "r_generate_lightgrid_test", "0", CVAR_NONE };

void Lightgrid_Init (void)
{
    Cvar_RegisterVariable (&r_lightgrid);
    Cvar_RegisterVariable (&r_lightgrid_debug);
    Cvar_RegisterVariable (&r_generate_lightgrid_test);
    Lightgrid_Clear ();
}

void Lightgrid_Shutdown (void)
{
    Lightgrid_Clear ();
}

void Lightgrid_Clear (void)
{
    Lightgrid_Free (current_lightgrid);
    current_lightgrid = NULL;
    q_strlcpy (lightgrid_source, "NONE", sizeof(lightgrid_source));
}

static qboolean Lightgrid_ValidateBSPX (int nx, int ny, int nz, int datasize)
{
    int count;
    size_t expect;

    if (nx <= 0 || ny <= 0 || nz <= 0)
        return false;

    count = nx * ny * nz;
    expect = (size_t)(3 * sizeof(int)) + sizeof(float) + sizeof(vec3_t) * 2 + (sizeof(vec3_t) * 2 + sizeof(float)) * (size_t)count;

    return expect <= (size_t)datasize;
}

qboolean Lightgrid_LoadFromBSPX (void *bspx_data, int bspx_len)
{
    byte *data = (byte *)bspx_data;
    int nx, ny, nz;
    float cellsize;
    vec3_t mins, maxs;
    int count, i;
    lightgrid_t *lg;

    Lightgrid_Clear ();

    if (!bspx_data || bspx_len <= 0)
        return false;

    if ((size_t)bspx_len < sizeof(int) * 3 + sizeof(float) + sizeof(vec3_t) * 2)
        return false;

    nx = LittleLong (((int *)data)[0]);
    ny = LittleLong (((int *)data)[1]);
    nz = LittleLong (((int *)data)[2]);

    if (!Lightgrid_ValidateBSPX (nx, ny, nz, bspx_len))
        return false;

    cellsize = LittleFloat (*(float *)(data + sizeof(int) * 3));
    memcpy (mins, data + sizeof(int) * 3 + sizeof(float), sizeof(vec3_t));
    memcpy (maxs, data + sizeof(int) * 3 + sizeof(float) + sizeof(vec3_t), sizeof(vec3_t));
    for (i = 0; i < 3; i++)
    {
        mins[i] = LittleFloat (mins[i]);
        maxs[i] = LittleFloat (maxs[i]);
    }

    lg = Lightgrid_Alloc (nx, ny, nz, cellsize, mins, maxs);
    if (!lg)
        return false;

    count = nx * ny * nz;

    data += sizeof(int) * 3 + sizeof(float) + sizeof(vec3_t) * 2;
    for (i = 0; i < count; i++)
    {
        vec3_t color, dir;
        float intensity;
        memcpy (color, data, sizeof(vec3_t));
        data += sizeof(vec3_t);
        memcpy (dir, data, sizeof(vec3_t));
        data += sizeof(vec3_t);
        memcpy (&intensity, data, sizeof(float));
        data += sizeof(float);

        color[0] = LittleFloat (color[0]);
        color[1] = LittleFloat (color[1]);
        color[2] = LittleFloat (color[2]);
        dir[0] = LittleFloat (dir[0]);
        dir[1] = LittleFloat (dir[1]);
        dir[2] = LittleFloat (dir[2]);
        intensity = LittleFloat (intensity);

        VectorCopy (color, lg->probes[i].rgb);
        VectorCopy (dir, lg->probes[i].dir);
        VectorNormalize (lg->probes[i].dir);
        lg->probes[i].intensity = intensity;
    }

    current_lightgrid = lg;
    q_strlcpy (lightgrid_source, "OCTREE", sizeof(lightgrid_source));

    return true;
}

static void Lightgrid_ClampPos (const vec3_t pos, vec3_t out)
{
    VectorCopy (pos, out);
    if (!current_lightgrid)
        return;

    out[0] = CLAMP (current_lightgrid->mins[0], out[0], current_lightgrid->maxs[0]);
    out[1] = CLAMP (current_lightgrid->mins[1], out[1], current_lightgrid->maxs[1]);
    out[2] = CLAMP (current_lightgrid->mins[2], out[2], current_lightgrid->maxs[2]);
}

static lightgrid_probe_t *Lightgrid_At (int x, int y, int z)
{
    return &current_lightgrid->probes[(z * current_lightgrid->ny + y) * current_lightgrid->nx + x];
}

static void Lightgrid_DefaultSample (vec3_t out_color, vec3_t out_dir)
{
    out_color[0] = out_color[1] = out_color[2] = 1.f;
    out_dir[0] = out_dir[1] = 0.f;
    out_dir[2] = 1.f;
}

void Lightgrid_Sample (const vec3_t pos, vec3_t out_color, vec3_t out_dir)
{
    vec3_t p;
    float fx, fy, fz;
    int x0, y0, z0, x1, y1, z1;
    lightgrid_probe_t *c000, *c100, *c010, *c110, *c001, *c101, *c011, *c111;
    vec3_t color = {1.f, 1.f, 1.f};
    vec3_t dir = {0.f, 0.f, 1.f};

    if (!current_lightgrid || !r_lightgrid.value)
    {
        Lightgrid_DefaultSample (out_color, out_dir);
        return;
    }

    Lightgrid_ClampPos (pos, p);

    fx = (p[0] - current_lightgrid->mins[0]) / current_lightgrid->cellsize;
    fy = (p[1] - current_lightgrid->mins[1]) / current_lightgrid->cellsize;
    fz = (p[2] - current_lightgrid->mins[2]) / current_lightgrid->cellsize;

    x0 = CLAMP (0, (int)floorf (fx), current_lightgrid->nx - 1);
    y0 = CLAMP (0, (int)floorf (fy), current_lightgrid->ny - 1);
    z0 = CLAMP (0, (int)floorf (fz), current_lightgrid->nz - 1);

    x1 = q_min (x0 + 1, current_lightgrid->nx - 1);
    y1 = q_min (y0 + 1, current_lightgrid->ny - 1);
    z1 = q_min (z0 + 1, current_lightgrid->nz - 1);

    fx -= floorf (fx);
    fy -= floorf (fy);
    fz -= floorf (fz);

    c000 = Lightgrid_At (x0, y0, z0);
    c100 = Lightgrid_At (x1, y0, z0);
    c010 = Lightgrid_At (x0, y1, z0);
    c110 = Lightgrid_At (x1, y1, z0);
    c001 = Lightgrid_At (x0, y0, z1);
    c101 = Lightgrid_At (x1, y0, z1);
    c011 = Lightgrid_At (x0, y1, z1);
    c111 = Lightgrid_At (x1, y1, z1);

    {
    vec3_t c00, c10, c01, c11, c0, c1;
    vec3_t d00, d10, d01, d11, d0, d1;

    vec3_t rgb000, rgb100, rgb010, rgb110, rgb001, rgb101, rgb011, rgb111;
    vec3_t dir000, dir100, dir010, dir110, dir001, dir101, dir011, dir111;

    // Pre-scale the RGB and direction values by probe intensity so that
    // brighter probes have a proportionally larger influence on the
    // trilinear result.
    VectorScale (c000->rgb, c000->intensity, rgb000);
    VectorScale (c100->rgb, c100->intensity, rgb100);
    VectorScale (c010->rgb, c010->intensity, rgb010);
    VectorScale (c110->rgb, c110->intensity, rgb110);
    VectorScale (c001->rgb, c001->intensity, rgb001);
    VectorScale (c101->rgb, c101->intensity, rgb101);
    VectorScale (c011->rgb, c011->intensity, rgb011);
    VectorScale (c111->rgb, c111->intensity, rgb111);

    VectorScale (c000->dir, c000->intensity, dir000);
    VectorScale (c100->dir, c100->intensity, dir100);
    VectorScale (c010->dir, c010->intensity, dir010);
    VectorScale (c110->dir, c110->intensity, dir110);
    VectorScale (c001->dir, c001->intensity, dir001);
    VectorScale (c101->dir, c101->intensity, dir101);
    VectorScale (c011->dir, c011->intensity, dir011);
    VectorScale (c111->dir, c111->intensity, dir111);

    VectorLerp (rgb000, rgb100, fx, c00);
    VectorLerp (rgb010, rgb110, fx, c10);
    VectorLerp (rgb001, rgb101, fx, c01);
    VectorLerp (rgb011, rgb111, fx, c11);

    VectorLerp (c00, c10, fy, c0);
    VectorLerp (c01, c11, fy, c1);

    VectorLerp (c0, c1, fz, color);

    VectorLerp (dir000, dir100, fx, d00);
    VectorLerp (dir010, dir110, fx, d10);
    VectorLerp (dir001, dir101, fx, d01);
    VectorLerp (dir011, dir111, fx, d11);

    VectorLerp (d00, d10, fy, d0);
    VectorLerp (d01, d11, fy, d1);

    VectorLerp (d0, d1, fz, dir);
    if (VectorNormalize (dir) == 0.f)
    {
        dir[0] = dir[1] = 0.f;
        dir[2] = 1.f;
    }
    }

    VectorCopy (color, out_color);
    VectorCopy (dir, out_dir);
}

static float Lightgrid_CellsizeForBounds (const vec3_t mins, const vec3_t maxs)
{
    (void)mins;
    (void)maxs;
    return LIGHTGRID_STANDARD_CELLSIZE;
}

lightgrid_t *Lightgrid_FromRaw (const lightgrid_raw_t *raw)
{
    lightgrid_t *lg;
    vec3_t mins, maxs;
    size_t count;

    if (!raw || !raw->cells || raw->cellSize <= 0.f)
        return NULL;

    if (raw->nx <= 0 || raw->ny <= 0 || raw->nz <= 0)
        return NULL;

    count = (size_t)raw->nx * (size_t)raw->ny * (size_t)raw->nz;
    if (!count || count > (SIZE_MAX / sizeof(lightgrid_probe_t)))
        return NULL;

    VectorCopy (raw->origin, mins);
    VectorCopy (raw->origin, maxs);
    maxs[0] += raw->cellSize * raw->nx;
    maxs[1] += raw->cellSize * raw->ny;
    maxs[2] += raw->cellSize * raw->nz;

    lg = Lightgrid_Alloc (raw->nx, raw->ny, raw->nz, raw->cellSize, mins, maxs);
    if (!lg)
        return NULL;

    for (size_t i = 0; i < count; i++)
    {
        const lightcell_t *cell = &raw->cells[i];
        vec3_t dir;

        VectorCopy (cell->rgb, lg->probes[i].rgb);
        VectorCopy (cell->dir, dir);
        if (VectorNormalize (dir) == 0.f)
        {
            dir[0] = dir[1] = 0.f;
            dir[2] = 1.f;
        }

        VectorCopy (dir, lg->probes[i].dir);
        lg->probes[i].intensity = cell->intensity;
    }

    q_strlcpy (lightgrid_source, "RAW", sizeof(lightgrid_source));

    return lg;
}

void Lightgrid_SetSource (const char *name)
{
    if (name && name[0])
        q_strlcpy (lightgrid_source, name, sizeof(lightgrid_source));
    else
        q_strlcpy (lightgrid_source, "UNKNOWN", sizeof(lightgrid_source));
}

const char *Lightgrid_GetSource (void)
{
    return lightgrid_source;
}

static void Lightgrid_AddDlights (const vec3_t pos, vec3_t color, vec3_t dirsum)
{
    int i;
    const dlight_t *l;

    for (i = 0, l = cl_dlights; i < MAX_DLIGHTS; i++, l++)
    {
        vec3_t delta;
        float dist2, add, scale;
        if (l->die <= cl.time || (!l->radius && !l->minlight))
            continue;

        VectorSubtract (pos, l->origin, delta);
        dist2 = DotProduct (delta, delta);
        if (dist2 >= l->radius * l->radius)
            continue;

        add = l->radius - sqrtf (dist2);
        if (add <= l->minlight)
            continue;

        scale = add * (1.f / 256.f);
        VectorMA (color, scale, l->color, color);

        if (VectorNormalize (delta) > 0.f)
            VectorMA (dirsum, add, delta, dirsum);
    }
}

static float Lightgrid_Random01 (void)
{
    return rand () * (1.f / (float)RAND_MAX);
}

static void Lightgrid_FillRandomCell (lightcell_t *cell)
{
    cell->rgb[0] = Lightgrid_Random01 ();
    cell->rgb[1] = Lightgrid_Random01 ();
    cell->rgb[2] = Lightgrid_Random01 ();

    cell->dir[0] = cell->dir[1] = 0.f;
    cell->dir[2] = 1.f;

    cell->intensity = (cell->rgb[0] + cell->rgb[1] + cell->rgb[2]) * (1.f / 3.f);
}

lightgrid_raw_t *Lightgrid_GenerateRaw (const qmodel_t *model)
{
    vec3_t mins, maxs, size;
    float cellsize;
    int nx, ny, nz;
    int x, y, z;

    if (!model)
        return NULL;

    VectorCopy (model->mins, mins);
    VectorCopy (model->maxs, maxs);
    VectorSubtract (maxs, mins, size);

    cellsize = Lightgrid_CellsizeForBounds (mins, maxs);

    nx = q_max (1, q_min (32, (int)ceilf (size[0] / cellsize)));
    ny = q_max (1, q_min (32, (int)ceilf (size[1] / cellsize)));
    nz = q_max (1, q_min (32, (int)ceilf (size[2] / cellsize)));

    lightgrid_raw_t *raw = (lightgrid_raw_t *)Hunk_AllocName (sizeof(*raw), "lightgrid_raw");
    if (!raw)
        return NULL;

    memset (raw, 0, sizeof(*raw));

    const size_t count = (size_t)nx * (size_t)ny * (size_t)nz;
    raw->cells = (lightcell_t *)Hunk_AllocName (count * sizeof(lightcell_t), "lightgrid_cells");
    if (!raw->cells)
        return NULL;

    raw->nx = nx;
    raw->ny = ny;
    raw->nz = nz;
    raw->cellSize = cellsize;
    VectorCopy (mins, raw->origin);

    for (z = 0; z < nz; z++)
    {
        for (y = 0; y < ny; y++)
        {
            for (x = 0; x < nx; x++)
            {
                vec3_t pos;
                lightcell_t *cell = &raw->cells[(z * ny + y) * nx + x];
                vec3_t dirsum = {0.f, 0.f, 0.f};
                float baseintensity;
                lightcache_t cache = {0};

                pos[0] = mins[0] + (x + 0.5f) * cellsize;
                pos[1] = mins[1] + (y + 0.5f) * cellsize;
                pos[2] = mins[2] + (z + 0.5f) * cellsize;

                if (r_generate_lightgrid_test.value > 0.f)
                {
                    Lightgrid_FillRandomCell (cell);
                    continue;
                }

                R_LightPoint (pos, 0.f, &cache);
                cell->rgb[0] = lightcolor[0] * (1.f / 255.f);
                cell->rgb[1] = lightcolor[1] * (1.f / 255.f);
                cell->rgb[2] = lightcolor[2] * (1.f / 255.f);

                baseintensity = (cell->rgb[0] + cell->rgb[1] + cell->rgb[2]) * (1.f / 3.f);

                Lightgrid_AddDlights (pos, cell->rgb, dirsum);

                {
                    vec3_t up = {0.f, 0.f, 1.f};
                    VectorMA (dirsum, baseintensity, up, dirsum);
                }

                cell->intensity = baseintensity;
                if (VectorNormalize (dirsum) == 0.f)
                {
                    cell->dir[0] = cell->dir[1] = 0.f;
                    cell->dir[2] = 1.f;
                }
                else
                {
                    VectorCopy (dirsum, cell->dir);
                }
            }
        }
    }

    return raw;
}

void Lightgrid_BuildFallback (void)
{
    lightgrid_t *lg;
    lightgrid_raw_t *raw;

    if (!cl.worldmodel)
        return;

    Con_Printf ("Lightgrid fallback: building interpolated grid...\n");

    raw = Lightgrid_GenerateRaw (cl.worldmodel);
    if (!raw)
    {
        Con_Printf ("Lightgrid fallback: failed to allocate grid\n");
        return;
    }

    cl.worldmodel->lightgrid_raw = raw;

    Lightgrid_Clear ();
    lg = Lightgrid_FromRaw (raw);
    if (!lg)
    {
        Con_Printf ("Lightgrid fallback: failed to upload grid\n");
        return;
    }

    current_lightgrid = lg;
    cl.lightgrid = lg;

    Con_Printf ("Lightgrid fallback: done (%dx%dx%d)\n", raw->nx, raw->ny, raw->nz);
}

const lightgrid_t *Lightgrid_Get (void)
{
    if (current_lightgrid)
        return current_lightgrid;

    return cl.lightgrid;
}

