/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
// r_decals.c -- runtime decal/mark surface rendering

#include "quakedef.h"
#include <ctype.h>
#include <string.h>

#define MAX_DECALS                      256
#define DECAL_TEXTURE_SIZE      64
#define DECAL_OFFSET            0.25f

#define DECAL_MAX_DISTANCE      64.f
#define BULLET_DECAL_DISTANCE   12.f
#define WALL_BLOOD_DISTANCE     48.f
#define FLOOR_BLOOD_DISTANCE    72.f
#define BLOOD_DECAL_SPREAD      32.f
#define BLOOD_DECAL_VERTICAL_SPREAD     16.f

#define BULLET_DECAL_MIN_RADIUS 7.0f
#define BULLET_DECAL_MAX_RADIUS 10.0f
#define BLOOD_DECAL_MIN_RADIUS  6.0f
#define BLOOD_DECAL_MAX_RADIUS  12.0f

#define BLOOD_POOL_RADIUS       22.0f
#define BLOOD_POOL_JITTER       12.0f
#define BLOOD_POOL_SMALL_MIN    6.0f
#define BLOOD_POOL_SMALL_MAX    10.0f

#define DECAL_LIGHT_REFRESH     0.1

#define BLOOD_WALL_THRESHOLD    0.65f

typedef enum decaltype_e
{
        DECAL_BULLET,
        DECAL_BLOOD,
        DECAL_COUNT
} decaltype_t;

typedef struct decalvertex_s
{
        vec3_t          pos;
        float           uv[2];
        float           color[4];
} decalvertex_t;

typedef struct decal_s
{
        qboolean        active;
        double          die;
        gltexture_t     *texture;
        gltexture_t     *base_texture;
        decaltype_t     type;
        float           radius;
        vec3_t          origin;
        vec3_t          normal;
        float           tint[4];
        vec3_t          light_color;
        lightcache_t    lightcache;
        double          last_light_update;
        decalvertex_t   verts[4];
} decal_t;

typedef struct decalgeom_s
{
        vec3_t  origin;
        vec3_t  normal;
        vec3_t  sdir;
        vec3_t  tdir;
        vec3_t  mins;
        vec3_t  maxs;
} decalgeom_t;

static decal_t                 r_decals[MAX_DECALS];
static gltexture_t     *r_decal_textures[DECAL_COUNT];

static GLushort          decal_indices[6 * MAX_DECALS];
static qboolean          decal_indices_init = false;

static decalvertex_t     decal_batch[4 * MAX_DECALS];
static int                       decal_batch_count = 0;
static gltexture_t       *decal_batch_texture = NULL;
static qboolean          decal_batch_showtris = false;

static cvar_t    r_decals_cvar = {"r_decals", "1", CVAR_ARCHIVE};
static cvar_t    r_decals_blood_cvar = {"r_decals_blood", "1", CVAR_ARCHIVE};
static cvar_t    r_decals_bullet_cvar = {"r_decals_bullet", "1", CVAR_ARCHIVE};
static cvar_t    r_decals_lifetime_cvar = {"r_decals_lifetime", "20", CVAR_ARCHIVE};
static cvar_t    r_decals_max_cvar = {"r_decals_max", "128", CVAR_ARCHIVE};
static cvar_t    r_decals_debug_cvar = {"r_decals_debug", "0", 0};

extern vec3_t lightcolor;

static byte r_decal_death_spawned[MAX_EDICTS];

static int R_GetDecalLimit (void)
{
        return CLAMP (0, (int) r_decals_max_cvar.value, MAX_DECALS);
}

static void R_InitDecalIndices (void)
{
        int i;
        if (decal_indices_init)
                return;

        for (i = 0; i < MAX_DECALS; i++)
        {
                decal_indices[i*6 + 0] = i*4 + 0;
                decal_indices[i*6 + 1] = i*4 + 1;
                decal_indices[i*6 + 2] = i*4 + 2;
                decal_indices[i*6 + 3] = i*4 + 0;
                decal_indices[i*6 + 4] = i*4 + 2;
                decal_indices[i*6 + 5] = i*4 + 3;
        }

        decal_indices_init = true;
}

static void R_ClearDecalBatch (void)
{
        decal_batch_count = 0;
        decal_batch_texture = NULL;
        decal_batch_showtris = false;
}

static void R_FlushDecalBatch (void)
{
        GLuint buf;
        GLbyte *ofs;

        if (!decal_batch_count)
                return;

        R_InitDecalIndices ();

        GL_Bind (GL_TEXTURE0, decal_batch_showtris ? whitetexture : decal_batch_texture);

        GL_Upload (GL_ARRAY_BUFFER, decal_batch, sizeof(decal_batch[0]) * decal_batch_count * 4, &buf, &ofs);
        GL_BindBuffer (GL_ARRAY_BUFFER, buf);
        GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, pos));
        GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, uv));
        GL_VertexAttribPointerFunc (2, 4, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, color));

        GL_Upload (GL_ELEMENT_ARRAY_BUFFER, decal_indices, sizeof(decal_indices[0]) * decal_batch_count * 6, &buf, &ofs);
        GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
        glDrawElements (GL_TRIANGLES, decal_batch_count * 6, GL_UNSIGNED_SHORT, ofs);

        R_ClearDecalBatch ();
}

static void R_AppendDecalToBatch (decal_t *dec, qboolean showtris)
{
        if (!dec->texture)
                return;

        if (!decal_batch_count)
        {
                decal_batch_texture = dec->texture;
                decal_batch_showtris = showtris;
        }

        if (decal_batch_count == MAX_DECALS || decal_batch_texture != dec->texture || decal_batch_showtris != showtris)
        {
                R_FlushDecalBatch ();
                decal_batch_texture = dec->texture;
                decal_batch_showtris = showtris;
        }

        if (decal_batch_count == MAX_DECALS)
                return;

        memcpy (&decal_batch[decal_batch_count * 4], dec->verts, sizeof(dec->verts));
        decal_batch_count++;
}

static void R_GenerateDecalTexture (decaltype_t type)
{
        byte data[DECAL_TEXTURE_SIZE * DECAL_TEXTURE_SIZE * 4];
        int x, y;
        const float inv = 1.0f / (DECAL_TEXTURE_SIZE - 1);

        for (y = 0; y < DECAL_TEXTURE_SIZE; y++)
        {
                for (x = 0; x < DECAL_TEXTURE_SIZE; x++)
                {
                        float fx = x * inv * 2.0f - 1.0f;
                        float fy = y * inv * 2.0f - 1.0f;
                        float dist = sqrtf (fx * fx + fy * fy);
                        float alpha = 0.f;
                        float r = 0.f, g = 0.f, b = 0.f;

                        switch (type)
                        {
                        case DECAL_BULLET:
                        {
                                const float radius = 0.82f;
                                if (dist <= radius)
                                {
                                        float falloff = 1.0f - (dist / radius);
                                        float hole = q_max (0.f, 1.0f - dist * 3.0f);
                                        alpha = powf (falloff, 1.8f) * 0.8f;
                                        float shade = 0.05f + falloff * 0.18f;
                                        shade = q_min (shade + hole * 0.1f, 0.35f);
                                        r = g = b = shade;
                                }
                                break;
                        }

                        case DECAL_BLOOD:
                        {
                                const float radius = 0.95f;
                                if (dist <= radius)
                                {
                                        float falloff = 1.0f - (dist / radius);
                                        float ring = 0.25f * sinf ((fx + fy) * 9.0f) * sinf ((fx - fy) * 7.0f);
                                        float noisefall = CLAMP (0.f, falloff + ring * 0.2f, 1.f);
                                        alpha = powf (noisefall, 1.5f);
                                        float base = 0.35f + noisefall * 0.45f;
                                        r = base * 0.8f + 0.2f;
                                        g = base * 0.18f;
                                        b = base * 0.14f;
                                }
                                break;
                        }
                        default:
                                break;
                        }

                        alpha = CLAMP (0.f, alpha, 1.f);
                        r = CLAMP (0.f, r, 1.f);
                        g = CLAMP (0.f, g, 1.f);
                        b = CLAMP (0.f, b, 1.f);

                        data[(y * DECAL_TEXTURE_SIZE + x) * 4 + 0] = (byte) (r * 255.0f);
                        data[(y * DECAL_TEXTURE_SIZE + x) * 4 + 1] = (byte) (g * 255.0f);
                        data[(y * DECAL_TEXTURE_SIZE + x) * 4 + 2] = (byte) (b * 255.0f);
                        data[(y * DECAL_TEXTURE_SIZE + x) * 4 + 3] = (byte) (alpha * 255.0f);
                }
        }

        r_decal_textures[type] = TexMgr_LoadImage (NULL,
                (type == DECAL_BULLET) ? "decal_bullet" : "decal_blood",
                DECAL_TEXTURE_SIZE, DECAL_TEXTURE_SIZE, SRC_RGBA, data, "", 0,
                TEXPREF_ALPHA | TEXPREF_PERSIST | TEXPREF_CLAMP | TEXPREF_NOPICMIP);
}

void R_InitDecals (void)
{
        int i;

        Cvar_RegisterVariable (&r_decals_cvar);
        Cvar_RegisterVariable (&r_decals_blood_cvar);
        Cvar_RegisterVariable (&r_decals_bullet_cvar);
        Cvar_RegisterVariable (&r_decals_lifetime_cvar);
        Cvar_RegisterVariable (&r_decals_max_cvar);
        Cvar_RegisterVariable (&r_decals_debug_cvar);

        for (i = 0; i < DECAL_COUNT; i++)
                R_GenerateDecalTexture ((decaltype_t)i);

        R_ClearDecals ();
}

void R_ClearDecals (void)
{
        memset (r_decals, 0, sizeof (r_decals));
        memset (r_decal_death_spawned, 0, sizeof (r_decal_death_spawned));
        R_ClearDecalBatch ();
}

void R_UpdateDecals (void)
{
        int i, limit = R_GetDecalLimit ();
        double t = cl.time;
        qboolean debug = r_decals_debug_cvar.value != 0.f;

        for (i = 0; i < MAX_DECALS; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active)
                        continue;
                if (i >= limit || dec->die <= t || !dec->texture)
                {
                        dec->active = false;
                        continue;
                }

                {
                        gltexture_t *desired = debug ? whitetexture : dec->base_texture;
                        if (dec->texture != desired)
                                dec->texture = desired;
                }

                if (debug)
                {
                        int v;
                        for (v = 0; v < 4; v++)
                        {
                                dec->verts[v].color[0] = 1.f;
                                dec->verts[v].color[1] = 1.f;
                                dec->verts[v].color[2] = 1.f;
                                dec->verts[v].color[3] = 1.f;
                        }
                        continue;
                }

                if (t - dec->last_light_update > DECAL_LIGHT_REFRESH)
                {
                        vec3_t sample;
                        lightcache_t cache = dec->lightcache;
                        VectorMA (dec->origin, DECAL_OFFSET * 2.f, dec->normal, sample);
                        if (cl.worldmodel && R_LightPoint (sample, 0.f, &cache))
                        {
                                int v;
                                VectorScale (lightcolor, 1.f / 200.f, dec->light_color);
                                for (v = 0; v < 3; v++)
                                        dec->light_color[v] = CLAMP (0.f, dec->light_color[v], 1.f);
                                dec->lightcache = cache;
                        }
                        else
                        {
                                dec->light_color[0] = dec->light_color[1] = dec->light_color[2] = 0.2f;
                        }

                        {
                                vec3_t shaded;
                                int v;
                                for (v = 0; v < 3; v++)
                                        shaded[v] = CLAMP (0.f, dec->light_color[v] * dec->tint[v], 1.f);
                                for (v = 0; v < 4; v++)
                                {
                                        dec->verts[v].color[0] = shaded[0];
                                        dec->verts[v].color[1] = shaded[1];
                                        dec->verts[v].color[2] = shaded[2];
                                        dec->verts[v].color[3] = CLAMP (0.f, dec->tint[3], 1.f);
                                }
                        }

                        dec->last_light_update = t;
                }
        }
}

static decal_t *R_AllocDecal (void)
{
        int i, limit = R_GetDecalLimit ();
        decal_t *oldest = NULL;
        double oldest_die = HUGE_VAL;

        if (limit <= 0)
                return NULL;

        for (i = 0; i < limit; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active)
                        return dec;
                if (dec->die < oldest_die)
                {
                        oldest_die = dec->die;
                        oldest = dec;
                }
        }

        return oldest;
}

static qboolean R_SurfaceIsDecalable (const msurface_t *surf)
{
        int flags = SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWFENCE | SURF_DRAWTELE | SURF_DRAWLAVA | SURF_DRAWSLIME | SURF_DRAWWATER | SURF_DRAWSPRITE;
        return (surf->flags & flags) == 0;
}

static void R_BuildOrthonormalBasis (const vec3_t normal, vec3_t sdir, vec3_t tdir)
{
        vec3_t temp = {0.f, 0.f, 1.f};

        if (fabsf (normal[0]) < 0.1f && fabsf (normal[1]) < 0.1f)
        {
                temp[0] = 1.f;
                temp[1] = 0.f;
                temp[2] = 0.f;
        }
        else
        {
                temp[0] = 0.f;
                temp[1] = 0.f;
                temp[2] = 1.f;
        }

        CrossProduct (temp, normal, sdir);
        VectorNormalizeFast (sdir);
        CrossProduct (normal, sdir, tdir);
        VectorNormalizeFast (tdir);
}

static qboolean R_DecalProject (const vec3_t point, const vec3_t preferred_normal, float maxdist, decalgeom_t *out)
{
        mleaf_t *leaf;
        msurface_t *best = NULL;
        vec3_t best_normal = {0, 0, 0};
        vec3_t best_origin = {0, 0, 0};
        vec3_t best_sdir = {0, 0, 0};
        vec3_t best_tdir = {0, 0, 0};
        vec3_t best_mins = {0, 0, 0};
        vec3_t best_maxs = {0, 0, 0};
        float best_score = -99999.f;
        float preferred_len = VectorLength (preferred_normal);
        int i;

        if (!cl.worldmodel || !cl.worldmodel->numleafs)
                return false;

        vec3_t point_copy;
        VectorCopy (point, point_copy);

        leaf = Mod_PointInLeaf (point_copy, cl.worldmodel);
        if (!leaf)
                return false;

        for (i = 0; i < leaf->nummarksurfaces; i++)
        {
                msurface_t *surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[i]];
                mtexinfo_t *texinfo;
                vec3_t normal, sdir, tdir, origin;
                float plane_dist, d, score;

                if (!R_SurfaceIsDecalable (surf))
                        continue;

                VectorCopy (surf->plane->normal, normal);
                plane_dist = surf->plane->dist;
                if (surf->flags & SURF_PLANEBACK)
                {
                        VectorScale (normal, -1.f, normal);
                        plane_dist = -plane_dist;
                }

                d = DotProduct (point, normal) - plane_dist;
                if (fabsf (d) > maxdist)
                        continue;

                VectorMA (point, -d, normal, origin);

                {
                        float margin = 4.f;
                        if (origin[0] < surf->mins[0] - margin || origin[0] > surf->maxs[0] + margin ||
                                origin[1] < surf->mins[1] - margin || origin[1] > surf->maxs[1] + margin ||
                                origin[2] < surf->mins[2] - margin || origin[2] > surf->maxs[2] + margin)
                                continue;
                }

                texinfo = surf->texinfo;
                if (texinfo)
                {
                        VectorSet (sdir, texinfo->vecs[0][0], texinfo->vecs[0][1], texinfo->vecs[0][2]);
                        VectorSet (tdir, texinfo->vecs[1][0], texinfo->vecs[1][1], texinfo->vecs[1][2]);

                        if (VectorLengthSquared (sdir) < 0.001f || VectorLengthSquared (tdir) < 0.001f)
                                R_BuildOrthonormalBasis (normal, sdir, tdir);
                        else
                        {
                                VectorMA (sdir, -DotProduct (sdir, normal), normal, sdir);
                                VectorMA (tdir, -DotProduct (tdir, normal), normal, tdir);
                                if (VectorLengthSquared (sdir) < 0.001f || VectorLengthSquared (tdir) < 0.001f)
                                        R_BuildOrthonormalBasis (normal, sdir, tdir);
                                else
                                {
                                        VectorNormalizeFast (sdir);
                                        VectorNormalizeFast (tdir);
                                        VectorMA (tdir, -DotProduct (tdir, sdir), sdir, tdir);
                                        VectorNormalizeFast (tdir);
                                        {
                                                vec3_t cross;
                                                CrossProduct (sdir, tdir, cross);
                                                if (DotProduct (cross, normal) < 0.f)
                                                        VectorScale (tdir, -1.f, tdir);
                                        }
                                }
                        }
                }
                else
                        R_BuildOrthonormalBasis (normal, sdir, tdir);

                score = -fabsf (d);
                if (preferred_len > 0.01f)
                {
                        vec3_t pn;
                        VectorCopy (preferred_normal, pn);
                        VectorNormalizeFast (pn);
                        score += DotProduct (normal, pn) * 0.5f;
                }

                if (score > best_score)
                {
                        best_score = score;
                        best = surf;
                        VectorCopy (origin, best_origin);
                        VectorCopy (normal, best_normal);
                        VectorCopy (sdir, best_sdir);
                        VectorCopy (tdir, best_tdir);
                        VectorCopy (surf->mins, best_mins);
                        VectorCopy (surf->maxs, best_maxs);
                }
        }

        if (!best)
                return false;

        VectorCopy (best_origin, out->origin);
        VectorCopy (best_normal, out->normal);
        VectorNormalizeFast (out->normal);
        VectorCopy (best_sdir, out->sdir);
        VectorNormalizeFast (out->sdir);
        VectorCopy (best_tdir, out->tdir);
        VectorNormalizeFast (out->tdir);
        VectorCopy (best_mins, out->mins);
        VectorCopy (best_maxs, out->maxs);

        return true;
}

static qboolean R_DecalHasEdgeRoom (const decalgeom_t *geom, float radius)
{
        int axis;
        float margin = radius + 1.0f;

        for (axis = 0; axis < 3; axis++)
        {
                float span = geom->maxs[axis] - geom->mins[axis];

                if (span <= 0.001f)
                        continue;

                {
                        float center = geom->mins[axis] + span * 0.5f;
                        float dist_to_edge = (span * 0.5f) - fabsf (geom->origin[axis] - center);

                        if (dist_to_edge < margin)
                                return false;
                }
        }

        return true;
}

static float R_RandomRange (float minval, float maxval)
{
        if (maxval <= minval)
                return minval;
        return minval + (maxval - minval) * ((float)rand () / (float)RAND_MAX);
}

static double R_GetDecalLifetime (void)
{
        return q_max (1.0, r_decals_lifetime_cvar.value);
}

static void R_AssignDecalVertices (decal_t *dec, const decalgeom_t *geom, float radius)
{
        vec3_t center, right, up;
        vec3_t sdir = {geom->sdir[0], geom->sdir[1], geom->sdir[2]};
        vec3_t tdir = {geom->tdir[0], geom->tdir[1], geom->tdir[2]};
        float angle = (float) rand () * (2.f * M_PI / (float) RAND_MAX);
        float c = cosf (angle), s = sinf (angle);
        vec3_t rs, rt;
        int i;

        for (i = 0; i < 3; i++)
        {
                rs[i] = sdir[i] * c + tdir[i] * s;
                rt[i] = tdir[i] * c - sdir[i] * s;
        }
        VectorNormalizeFast (rs);
        VectorNormalizeFast (rt);

        VectorScale (rs, radius, right);
        VectorScale (rt, radius, up);

        VectorMA (geom->origin, DECAL_OFFSET, geom->normal, center);

        VectorSubtract (center, right, dec->verts[0].pos);
        VectorSubtract (dec->verts[0].pos, up, dec->verts[0].pos);
        dec->verts[0].uv[0] = 0.f; dec->verts[0].uv[1] = 1.f;

        VectorSubtract (center, right, dec->verts[1].pos);
        VectorAdd (dec->verts[1].pos, up, dec->verts[1].pos);
        dec->verts[1].uv[0] = 0.f; dec->verts[1].uv[1] = 0.f;

        VectorAdd (center, right, dec->verts[2].pos);
        VectorAdd (dec->verts[2].pos, up, dec->verts[2].pos);
        dec->verts[2].uv[0] = 1.f; dec->verts[2].uv[1] = 0.f;

        VectorAdd (center, right, dec->verts[3].pos);
        VectorSubtract (dec->verts[3].pos, up, dec->verts[3].pos);
        dec->verts[3].uv[0] = 1.f; dec->verts[3].uv[1] = 1.f;
}

static qboolean R_CreateDecal (const decalgeom_t *geom, float radius, decaltype_t type, const float *tint)
{
        decal_t *dec;

        if (radius <= 0.f)
                return false;

        dec = R_AllocDecal ();
        if (!dec)
                return false;

        dec->base_texture = r_decal_textures[type];
        if (!dec->base_texture)
                return false;

        dec->texture = dec->base_texture;
        dec->type = type;
        dec->radius = radius;
        VectorCopy (geom->origin, dec->origin);
        VectorCopy (geom->normal, dec->normal);
        dec->lightcache.surfidx = 0;
        dec->last_light_update = 0.0;

        dec->die = cl.time + R_GetDecalLifetime ();
        dec->active = true;
        dec->tint[0] = tint ? tint[0] : 1.f;
        dec->tint[1] = tint ? tint[1] : 1.f;
        dec->tint[2] = tint ? tint[2] : 1.f;
        dec->tint[3] = tint ? tint[3] : 1.f;
        R_AssignDecalVertices (dec, geom, radius);

        {
                vec3_t sample;
                lightcache_t cache = dec->lightcache;
                VectorMA (geom->origin, DECAL_OFFSET * 2.f, geom->normal, sample);
                if (cl.worldmodel && R_LightPoint (sample, 0.f, &cache))
                {
                        int v;
                        VectorScale (lightcolor, 1.f / 200.f, dec->light_color);
                        for (v = 0; v < 3; v++)
                                dec->light_color[v] = CLAMP (0.f, dec->light_color[v], 1.f);
                        dec->lightcache = cache;
                }
                else
                {
                        dec->light_color[0] = dec->light_color[1] = dec->light_color[2] = 0.5f;
                }

                {
                        vec3_t shaded;
                        int v;
                        for (v = 0; v < 3; v++)
                                shaded[v] = CLAMP (0.f, dec->light_color[v] * dec->tint[v], 1.f);
                        for (v = 0; v < 4; v++)
                        {
                                dec->verts[v].color[0] = shaded[0];
                                dec->verts[v].color[1] = shaded[1];
                                dec->verts[v].color[2] = shaded[2];
                                dec->verts[v].color[3] = CLAMP (0.f, dec->tint[3], 1.f);
                        }
                }

                dec->last_light_update = cl.time;
        }

        if (r_decals_debug_cvar.value)
        {
                const char *name = (type == DECAL_BULLET) ? "bullet" : "blood";
                Con_Printf ("Decal[%s] origin=(%5.1f,%5.1f,%5.1f) normal=(%4.2f,%4.2f,%4.2f) radius=%.1f\n",
                        name,
                        geom->origin[0], geom->origin[1], geom->origin[2],
                        geom->normal[0], geom->normal[1], geom->normal[2],
                        radius);
        }
        return true;
}

void R_AddBulletDecal (const vec3_t point)
{
        decalgeom_t geom;
        float radius;
        const float tint[4] = {0.2f, 0.12f, 0.05f, 0.8f};

        if (!r_decals_cvar.value || !r_decals_bullet_cvar.value)
                return;

        if (!R_DecalProject (point, vec3_origin, BULLET_DECAL_DISTANCE, &geom))
                return;

        radius = R_RandomRange (BULLET_DECAL_MIN_RADIUS, BULLET_DECAL_MAX_RADIUS);
        if (!R_DecalHasEdgeRoom (&geom, radius))
                return;
        R_CreateDecal (&geom, radius, DECAL_BULLET, tint);
}

static qboolean R_AddBloodDecalForDirection (const vec3_t point, const vec3_t preferred_normal, float maxdist, float min_z, decaltype_t type, float radius_override, const float *tint)
{
        decalgeom_t geom;
        if (!R_DecalProject (point, preferred_normal, maxdist, &geom))
                return false;

        if (min_z > 0.f && geom.normal[2] < min_z)
                return false;
        if (min_z < 0.f && fabsf (geom.normal[2]) > BLOOD_WALL_THRESHOLD)
                return false;

        {
                float radius;

                if (radius_override > 0.f)
                        radius = radius_override;
                else
                {
                        float base_radius = R_RandomRange (BLOOD_DECAL_MIN_RADIUS, BLOOD_DECAL_MAX_RADIUS);
                        float scale = R_RandomRange (1.f, 2.5f);
                        radius = base_radius * scale * 0.75f;
                }
                if (!R_DecalHasEdgeRoom (&geom, radius))
                        return false;
                return R_CreateDecal (&geom, radius, type, tint);
        }
}

void R_AddBloodDecal (const vec3_t point, const vec3_t dir)
{
        vec3_t up = {0.f, 0.f, 1.f};
        const float tint[4] = {0.5f, 0.05f, 0.05f, 0.85f};
        int spawn_count;
        int i;

        if (!r_decals_cvar.value || !r_decals_blood_cvar.value)
                return;

        spawn_count = (rand () % 4) + 3;
        for (i = 0; i < spawn_count; i++)
        {
                vec3_t spawn_point;
                vec3_t preferred;
                vec3_t negpref;
                qboolean spawned = false;

                VectorCopy (point, spawn_point);
                spawn_point[0] += R_RandomRange (-BLOOD_DECAL_SPREAD, BLOOD_DECAL_SPREAD);
                spawn_point[1] += R_RandomRange (-BLOOD_DECAL_SPREAD, BLOOD_DECAL_SPREAD);
                spawn_point[2] += R_RandomRange (-BLOOD_DECAL_VERTICAL_SPREAD, BLOOD_DECAL_VERTICAL_SPREAD);

                VectorCopy (dir, preferred);
                if (VectorLengthSquared (preferred) > 0.01f)
                {
                        preferred[0] += R_RandomRange (-0.35f, 0.35f);
                        preferred[1] += R_RandomRange (-0.35f, 0.35f);
                        preferred[2] += R_RandomRange (-0.35f, 0.35f);
                        VectorNormalizeFast (preferred);
                        VectorScale (preferred, -1.f, negpref);
                        spawned |= R_AddBloodDecalForDirection (spawn_point, preferred, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, 0.f, tint);
                        if (!spawned)
                                spawned |= R_AddBloodDecalForDirection (spawn_point, negpref, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, 0.f, tint);
                }

                R_AddBloodDecalForDirection (spawn_point, up, FLOOR_BLOOD_DISTANCE, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, 0.f, tint);
        }
}

void R_DrawDecals (qboolean showtris)
{
        int i;
        qboolean drew = false;

        if (!r_decals_cvar.value)
                return;

        for (i = 0; i < MAX_DECALS; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active)
                        continue;
                if (dec->die <= cl.time || !dec->texture)
                        continue;

                if (!drew)
                {
                        qboolean dither = (softemu == SOFTEMU_COARSE && !showtris);
                        GL_BeginGroup ("Decals");
                        GL_UseProgram (glprogs.decals[dither]);
                        if (showtris)
                                GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (3));
                        else
                                GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (3));
                        GL_PolygonOffset (OFFSET_DECAL);
                        drew = true;
                }

                R_AppendDecalToBatch (dec, showtris);
        }

        if (drew)
        {
                R_FlushDecalBatch ();
                GL_PolygonOffset (OFFSET_NONE);
                GL_EndGroup ();
        }
        else
                R_ClearDecalBatch ();
}

void R_DrawDecals_ShowTris (void)
{
        R_DrawDecals (true);
}

static qboolean R_FrameNameIsDeath (const char *name)
{
        char lower[sizeof(((maliasframedesc_t *)0)->name)];
        size_t i;

        if (!name)
                return false;

        for (i = 0; i < sizeof(lower) - 1 && name[i]; i++)
                lower[i] = (char)tolower ((unsigned char)name[i]);
        lower[i] = '\0';

        if (strstr (lower, "dead"))
                return true;
        if (strstr (lower, "death"))
                return true;
        if (strstr (lower, "die"))
                return true;
        return false;
}

static void R_SpawnBloodPool (entity_t *ent)
{
        vec3_t base_point;
        vec3_t up = {0.f, 0.f, 1.f};
        float large_radius = BLOOD_POOL_RADIUS;
        const float pool_tint[4] = {1.f, 1.f, 1.f, 0.8f};
        const float small_tint[4] = {1.f, 1.f, 1.f, 0.8f};
        int small_count;
        int spawned = 0;

        if (!ent || !ent->model)
                return;

        VectorCopy (ent->origin, base_point);
        if (ent->model)
                base_point[2] += ent->model->mins[2];

        if (R_AddBloodDecalForDirection (base_point, up, FLOOR_BLOOD_DISTANCE * 1.5f, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, large_radius, pool_tint))
                spawned++;

        small_count = 5 + (rand () % 4);
        while (small_count--)
        {
                vec3_t jitter = {base_point[0], base_point[1], base_point[2]};
                float angle = ((float)rand () / (float)RAND_MAX) * (float)M_PI * 2.f;
                float dist = R_RandomRange (BLOOD_POOL_SMALL_MIN, BLOOD_POOL_SMALL_MAX);
                float radius = R_RandomRange (BLOOD_POOL_SMALL_MIN, BLOOD_POOL_SMALL_MAX);

                jitter[0] += cosf (angle) * (BLOOD_POOL_JITTER + dist);
                jitter[1] += sinf (angle) * (BLOOD_POOL_JITTER + dist);

                if (R_AddBloodDecalForDirection (jitter, up, FLOOR_BLOOD_DISTANCE, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, radius, small_tint))
                        spawned++;
        }

        if (r_decals_debug_cvar.value && spawned <= 0)
        {
                Con_Printf ("Decal[blood_pool] failed for entity at (%.1f, %.1f, %.1f)\n",
                        ent->origin[0], ent->origin[1], ent->origin[2]);
        }
}

void R_Decals_EntityFrameChanged (entity_t *ent, int prevframe, qboolean model_changed)
{
        int entnum;

        if (!ent)
                return;

        entnum = (int)(ent - cl_entities);
        if (entnum < 0 || entnum >= MAX_EDICTS)
                return;

        if (model_changed)
                r_decal_death_spawned[entnum] = 0;

        if (!ent->model || ent->model->type != mod_alias)
        {
                r_decal_death_spawned[entnum] = 0;
                return;
        }

        if (ent->frame == prevframe)
                return;

        {
                aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata (ent->model);
                if (!hdr)
                        return;
                if (ent->frame < 0 || ent->frame >= hdr->numframes)
                        return;

                if (R_FrameNameIsDeath (hdr->frames[ent->frame].name))
                {
                        if (!r_decal_death_spawned[entnum])
                        {
                                R_SpawnBloodPool (ent);
                                r_decal_death_spawned[entnum] = 1;
                        }
                }
                else
                {
                        r_decal_death_spawned[entnum] = 0;
                }
        }
}
