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
    Lightgrid_Clear();
}

void Lightgrid_Shutdown(void)
{
    Lightgrid_Clear();
}

void Lightgrid_Clear(void)
{
    Lightgrid_Free(current_lightgrid);
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

static qboolean Lightgrid_ValidateBSPX(int nx, int ny, int nz, int datasize)
{
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return false;

    size_t count = (size_t)nx * ny * nz;

    size_t expect =
        sizeof(int)*3 +
        sizeof(float) +
        sizeof(vec3_t)*2 +
        count * (sizeof(vec3_t)*2 + sizeof(float));

    return (size_t)datasize == expect;
}

/* =====================================================================
   BSPX Loader
   ===================================================================== */

qboolean Lightgrid_LoadFromBSPX(void *bspx_data, int bspx_len)
{
    if (!bspx_data || bspx_len <= 0)
        return false;

    Lightgrid_Clear();

    if ((size_t)bspx_len < sizeof(int)*3 + sizeof(float) + sizeof(vec3_t)*2)
        return false;

    byte *data = (byte*)bspx_data;

    int nx = LittleLong(((int*)data)[0]);
    int ny = LittleLong(((int*)data)[1]);
    int nz = LittleLong(((int*)data)[2]);

    if (!Lightgrid_ValidateBSPX(nx, ny, nz, bspx_len))
        return false;

    float cellSize = LG_FLittle(data + sizeof(int)*3);

    vec3_t mins, maxs;
    float *fmins = (float*)(data + sizeof(int)*3 + sizeof(float));
    float *fmaxs = (float*)(data + sizeof(int)*3 + sizeof(float) + sizeof(vec3_t));

    for (int i = 0; i < 3; i++)
    {
        mins[i] = LittleFloat(fmins[i]);
        maxs[i] = LittleFloat(fmaxs[i]);
    }

    lightgrid_t *lg = Lightgrid_Alloc(nx, ny, nz, cellSize, mins, maxs);
    if (!lg)
        return false;

    int count = nx * ny * nz;
    data += sizeof(int)*3 + sizeof(float) + sizeof(vec3_t)*2;

    for (int i = 0; i < count; i++)
    {
        float col[3], dir[3], intensity;

        col[0] = LG_FLittle(data + 0);
        col[1] = LG_FLittle(data + 4);
        col[2] = LG_FLittle(data + 8);
        data += sizeof(vec3_t);

        dir[0] = LG_FLittle(data + 0);
        dir[1] = LG_FLittle(data + 4);
        dir[2] = LG_FLittle(data + 8);
        data += sizeof(vec3_t);

        memcpy(&intensity, data, sizeof(float));
        intensity = LittleFloat(intensity);
        data += sizeof(float);

        VectorCopy(col, lg->probes[i].rgb);
        VectorCopy(dir, lg->probes[i].dir);
        lg->probes[i].intensity = intensity;

        if (VectorNormalize(lg->probes[i].dir) == 0)
            VectorSet(lg->probes[i].dir, 0,0,1);
    }

    current_lightgrid = lg;
    cl.lightgrid      = lg;
    q_strlcpy(lightgrid_source, "OCTREE", sizeof(lightgrid_source));

    return true;
}

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
    }

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
    memset(&cell->sh, 0, sizeof(cell->sh));
    cell->sh_valid = false;
}

/* =====================================================================
   RAW Lightgrid generator (fallback)
   ===================================================================== */

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

    const float cellSize = LIGHTGRID_STANDARD_CELLSIZE;

    int nx = q_max(1, q_min(64, (int)ceilf(size[0] / cellSize)));
    int ny = q_max(1, q_min(64, (int)ceilf(size[1] / cellSize)));
    int nz = q_max(1, q_min(64, (int)ceilf(size[2] / cellSize)));

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

            float dist = VectorDistance(pos, center);
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
}

const char *Lightgrid_GetSource(void)
{
    return lightgrid_source;
}
