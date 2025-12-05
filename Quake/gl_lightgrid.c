/*
Copyright (C) 2024 Ironwail developers
*/

#include "quakedef.h"
#include "gl_lightgrid.h"
#include "glquake.h"

extern vec3_t lightcolor;

static lightgrid_t *current_lightgrid;

cvar_t  r_lightgrid = { "r_lightgrid", "1", CVAR_ARCHIVE };
cvar_t  r_lightgrid_debug = { "r_lightgrid_debug", "0", CVAR_NONE };

void Lightgrid_Init (void)
{
    Cvar_RegisterVariable (&r_lightgrid);
    Cvar_RegisterVariable (&r_lightgrid_debug);
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

        VectorLerp (c000->rgb, c100->rgb, fx, c00);
        VectorLerp (c010->rgb, c110->rgb, fx, c10);
        VectorLerp (c001->rgb, c101->rgb, fx, c01);
        VectorLerp (c011->rgb, c111->rgb, fx, c11);

        VectorLerp (c00, c10, fy, c0);
        VectorLerp (c01, c11, fy, c1);

        VectorLerp (c0, c1, fz, color);

        VectorLerp (c000->dir, c100->dir, fx, d00);
        VectorLerp (c010->dir, c110->dir, fx, d10);
        VectorLerp (c001->dir, c101->dir, fx, d01);
        VectorLerp (c011->dir, c111->dir, fx, d11);

        VectorLerp (d00, d10, fy, d0);
        VectorLerp (d01, d11, fy, d1);

        VectorLerp (d0, d1, fz, dir);
        VectorNormalize (dir);
    }

    VectorCopy (color, out_color);
    VectorCopy (dir, out_dir);
}

static float Lightgrid_CellsizeForBounds (const vec3_t mins, const vec3_t maxs)
{
    vec3_t size;
    float cellsize;

    VectorSubtract (maxs, mins, size);
    cellsize = q_max (size[0], q_max (size[1], size[2])) / 32.f;
    if (cellsize < 16.f)
        cellsize = 16.f;
    return cellsize;
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

void Lightgrid_BuildFallback (void)
{
    vec3_t mins, maxs, size;
    float cellsize;
    int nx, ny, nz;
    int x, y, z;

    if (!cl.worldmodel)
        return;

    Con_Printf ("Lightgrid fallback: building interpolated grid...\n");

    VectorCopy (cl.worldmodel->mins, mins);
    VectorCopy (cl.worldmodel->maxs, maxs);
    VectorSubtract (maxs, mins, size);

    cellsize = Lightgrid_CellsizeForBounds (mins, maxs);

    nx = q_max (1, q_min (32, (int)ceilf (size[0] / cellsize)));
    ny = q_max (1, q_min (32, (int)ceilf (size[1] / cellsize)));
    nz = q_max (1, q_min (32, (int)ceilf (size[2] / cellsize)));

    Lightgrid_Clear ();

    current_lightgrid = Lightgrid_Alloc (nx, ny, nz, cellsize, mins, maxs);
    if (!current_lightgrid)
    {
        Con_Printf ("Lightgrid fallback: failed to allocate grid\n");
        return;
    }

    for (z = 0; z < nz; z++)
    {
        for (y = 0; y < ny; y++)
        {
            for (x = 0; x < nx; x++)
            {
                vec3_t pos;
                lightgrid_probe_t *cell = Lightgrid_At (x, y, z);
                vec3_t dirsum = {0.f, 0.f, 0.f};
                float baseintensity;
                lightcache_t cache = {0};

                pos[0] = mins[0] + (x + 0.5f) * cellsize;
                pos[1] = mins[1] + (y + 0.5f) * cellsize;
                pos[2] = mins[2] + (z + 0.5f) * cellsize;

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

    Con_Printf ("Lightgrid fallback: done (%dx%dx%d)\n", nx, ny, nz);
}

const lightgrid_t *Lightgrid_Get (void)
{
    if (current_lightgrid)
        return current_lightgrid;

    return cl.lightgrid;
}

