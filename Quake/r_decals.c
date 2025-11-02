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
#include <stdlib.h>

static const int R_DECAL_INITIAL_CAPACITY = 256;
static const int R_DECAL_CAP_MAX = 16384;
#define DECAL_TEXTURE_SIZE      64
#define DECAL_OFFSET            0.25f

#define DECAL_MAX_DISTANCE      64.f
#define BULLET_DECAL_DISTANCE   12.f
#define WALL_BLOOD_DISTANCE     48.f
#define FLOOR_BLOOD_DISTANCE    72.f
#define BULLET_DECAL_MIN_RADIUS 7.0f
#define BULLET_DECAL_MAX_RADIUS 10.0f
#define BLOOD_DECAL_MIN_RADIUS  6.0f
#define BLOOD_DECAL_MAX_RADIUS  12.0f

#define BLOOD_DECAL_SIZE_SCALE  0.5625f

#define EXPLOSION_DECAL_MIN_RADIUS       (BULLET_DECAL_MIN_RADIUS * 4.0f)
#define EXPLOSION_DECAL_MAX_RADIUS       (BULLET_DECAL_MAX_RADIUS * 4.0f)

#define GIB_DECAL_RADIUS         28.0f
#define GIB_DECAL_PARTICLE_THRESHOLD    80

#define BLOOD_POOL_RADIUS       22.0f
#define BLOOD_POOL_JITTER       12.0f
#define BLOOD_POOL_SMALL_MIN    6.0f
#define BLOOD_POOL_SMALL_MAX    10.0f

#define DECAL_LIGHT_REFRESH     0.1

#define BLOOD_WALL_THRESHOLD    0.65f

#define BLOOD_CLUSTER_MIN               7
#define BLOOD_CLUSTER_MAX               13
#define BLOOD_PRIMARY_RADIUS_SCALE      1.35f
#define BLOOD_SECONDARY_SCALE_MIN       0.65f
#define BLOOD_SECONDARY_SCALE_MAX       1.1f
#define BLOOD_SPRAY_CONE_ANGLE_DEG      43.75f
#define BLOOD_SPRAY_MIN_DISTANCE        6.f
#define BLOOD_SPRAY_MAX_DISTANCE        56.f
#define BLOOD_SPRAY_GRAVITY_FACTOR      0.32f
#define BLOOD_SPRAY_LATERAL_JITTER      4.375f

typedef enum decaltype_e
{
        DECAL_BULLET,
        DECAL_BLOOD,
        DECAL_SCORCH,
        DECAL_COUNT
} decaltype_t;

typedef struct decalvertex_s
{
        vec3_t          pos;
        float           uv[2];
        float           color[4];
        float           normal_spec[4];
} decalvertex_t;

typedef struct decal_s
{
        qboolean        active;
        double          spawn_time;
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

static decal_t                 *r_decals = NULL;
static gltexture_t     *r_decal_textures[DECAL_COUNT];

static GLushort          *decal_indices = NULL;
static qboolean          decal_indices_init = false;

static decalvertex_t     *decal_batch = NULL;
static decal_t           **decal_draw_list = NULL;
static int                       decal_batch_count = 0;
static gltexture_t       *decal_batch_texture = NULL;
static qboolean          decal_batch_showtris = false;
static int                       r_decal_capacity = 0;

static cvar_t    r_decals_cvar = {"r_decals", "1", CVAR_ARCHIVE};
static cvar_t    r_decals_blood_cvar = {"r_decals_blood", "1", CVAR_ARCHIVE};
static cvar_t    r_decals_bullet_cvar = {"r_decals_bullet", "1", CVAR_ARCHIVE};
static cvar_t    r_decals_max_cvar = {"r_decals_max", "128", CVAR_ARCHIVE};
static cvar_t    r_decals_debug_cvar = {"r_decals_debug", "0", 0};

extern vec3_t lightcolor;

static byte r_decal_death_spawned[MAX_EDICTS];
static double r_last_gib_decal_time = -9999.0;
static vec3_t r_last_gib_decal_origin = {0.f, 0.f, 0.f};
static int r_active_decal_count = 0;

static void R_ReserveDecalStorage (int desired)
{
        size_t old_capacity = (size_t) r_decal_capacity;
        int new_capacity;

        if (desired > R_DECAL_CAP_MAX)
                desired = R_DECAL_CAP_MAX;

        if (desired <= r_decal_capacity)
                return;

        if (r_decal_capacity <= 0)
                new_capacity = R_DECAL_INITIAL_CAPACITY;
        else
                new_capacity = r_decal_capacity;

        if (new_capacity <= 0)
                new_capacity = R_DECAL_INITIAL_CAPACITY;

        while (new_capacity < desired && new_capacity < R_DECAL_CAP_MAX)
        {
                int doubled = new_capacity * 2;
                if (doubled <= new_capacity)
                        break;
                new_capacity = doubled;
        }

        if (new_capacity > R_DECAL_CAP_MAX)
                new_capacity = R_DECAL_CAP_MAX;

        if (new_capacity < desired)
                new_capacity = desired;

        {
                decal_t *new_decals = (decal_t *) realloc (r_decals, (size_t)new_capacity * sizeof (*r_decals));
                if (!new_decals)
                        Sys_Error ("R_ReserveDecalStorage: failed to allocate %zu bytes for decals", (size_t)new_capacity * sizeof (*r_decals));
                if ((size_t)new_capacity > old_capacity)
                        memset (new_decals + old_capacity, 0, ((size_t)new_capacity - old_capacity) * sizeof (*r_decals));
                r_decals = new_decals;
        }

        {
                GLushort *new_indices = (GLushort *) realloc (decal_indices, (size_t)new_capacity * 6 * sizeof (*decal_indices));
                if (!new_indices)
                        Sys_Error ("R_ReserveDecalStorage: failed to allocate %zu bytes for indices", (size_t)new_capacity * 6 * sizeof (*decal_indices));
                if ((size_t)new_capacity > old_capacity)
                        memset (new_indices + old_capacity * 6, 0, ((size_t)new_capacity - old_capacity) * 6 * sizeof (*decal_indices));
                decal_indices = new_indices;
                decal_indices_init = false;
        }

        {
                decalvertex_t *new_batch = (decalvertex_t *) realloc (decal_batch, (size_t)new_capacity * 4 * sizeof (*decal_batch));
                if (!new_batch)
                        Sys_Error ("R_ReserveDecalStorage: failed to allocate %zu bytes for decal batch", (size_t)new_capacity * 4 * sizeof (*decal_batch));
                if ((size_t)new_capacity > old_capacity)
                        memset (new_batch + old_capacity * 4, 0, ((size_t)new_capacity - old_capacity) * 4 * sizeof (*decal_batch));
                decal_batch = new_batch;
        }

        {
                decal_t **new_draw_list = (decal_t **) realloc (decal_draw_list, (size_t)new_capacity * sizeof (*decal_draw_list));
                if (!new_draw_list)
                        Sys_Error ("R_ReserveDecalStorage: failed to allocate %zu bytes for decal draw list", (size_t)new_capacity * sizeof (*decal_draw_list));
                if ((size_t)new_capacity > old_capacity)
                        memset (new_draw_list + old_capacity, 0, ((size_t)new_capacity - old_capacity) * sizeof (*decal_draw_list));
                decal_draw_list = new_draw_list;
        }

        r_decal_capacity = new_capacity;
        decal_batch_count = 0;
        decal_batch_texture = NULL;
        decal_batch_showtris = false;
}

static int R_GetDecalLimit (void)
{
        int desired = (int) CLAMP (0, r_decals_max_cvar.value, (float) R_DECAL_CAP_MAX);

        if (r_decals_max_cvar.value > (float) R_DECAL_CAP_MAX)
                Cvar_SetValueQuick (&r_decals_max_cvar, (float) R_DECAL_CAP_MAX);

        if (desired > r_decal_capacity)
                R_ReserveDecalStorage (desired);

        if (r_decal_capacity <= 0)
                return desired;

        return CLAMP (0, desired, r_decal_capacity);
}

static void R_DeactivateDecal (decal_t *dec)
{
        if (!dec || !dec->active)
                return;

        dec->active = false;
        dec->texture = NULL;
        dec->base_texture = NULL;
        dec->last_light_update = 0.0;
        dec->spawn_time = 0.0;

        if (r_active_decal_count > 0)
                r_active_decal_count--;
}

static void R_RemoveExcessDecals (int limit)
{
        int to_remove;

        if (!r_decals || r_decal_capacity <= 0)
                return;

        if (limit <= 0)
        {
                int i;
                for (i = 0; i < r_decal_capacity; i++)
                        R_DeactivateDecal (&r_decals[i]);
                return;
        }

        if (r_active_decal_count <= limit)
                return;

        to_remove = r_active_decal_count - limit;
        while (to_remove-- > 0)
        {
                decal_t *oldest = NULL;
                double oldest_spawn = HUGE_VAL;
                int i;

                for (i = 0; i < r_decal_capacity; i++)
                {
                        decal_t *dec = &r_decals[i];
                        if (!dec->active)
                                continue;
                        if (dec->spawn_time < oldest_spawn)
                        {
                                oldest_spawn = dec->spawn_time;
                                oldest = dec;
                        }
                }

                if (!oldest)
                        break;

                R_DeactivateDecal (oldest);
        }
}

static int R_CompareDecalTexture (const void *a, const void *b)
{
        const decal_t *const *da = (const decal_t *const *) a;
        const decal_t *const *db = (const decal_t *const *) b;

        if ((*da)->texture < (*db)->texture)
                return -1;
        if ((*da)->texture > (*db)->texture)
                return 1;
        if (*da < *db)
                return -1;
        if (*da > *db)
                return 1;
        return 0;
}

static void R_InitDecalIndices (void)
{
        int i;
        if (decal_indices_init)
                return;

        if (!decal_indices || r_decal_capacity <= 0)
                return;

        for (i = 0; i < r_decal_capacity; i++)
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

        if (!decal_batch || !decal_indices)
        {
                R_ClearDecalBatch ();
                return;
        }

        GL_Bind (GL_TEXTURE0, decal_batch_showtris ? whitetexture : decal_batch_texture);

        GL_Upload (GL_ARRAY_BUFFER, decal_batch, sizeof(decal_batch[0]) * decal_batch_count * 4, &buf, &ofs);
        GL_BindBuffer (GL_ARRAY_BUFFER, buf);
        GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, pos));
        GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, uv));
        GL_VertexAttribPointerFunc (2, 4, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, color));
        GL_VertexAttribPointerFunc (3, 4, GL_FLOAT, GL_FALSE, sizeof(decal_batch[0]), ofs + offsetof(decalvertex_t, normal_spec));

        GL_Upload (GL_ELEMENT_ARRAY_BUFFER, decal_indices, sizeof(decal_indices[0]) * decal_batch_count * 6, &buf, &ofs);
        GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
        glDrawElements (GL_TRIANGLES, decal_batch_count * 6, GL_UNSIGNED_SHORT, ofs);

        R_ClearDecalBatch ();
}

static void R_AppendDecalToBatch (decal_t *dec, qboolean showtris)
{
        if (!dec->texture)
                return;

        if (!decal_batch || r_decal_capacity <= 0)
                return;

        if (!decal_batch_count)
        {
                decal_batch_texture = dec->texture;
                decal_batch_showtris = showtris;
        }

        if (decal_batch_count == r_decal_capacity || decal_batch_texture != dec->texture || decal_batch_showtris != showtris)
        {
                R_FlushDecalBatch ();
                decal_batch_texture = dec->texture;
                decal_batch_showtris = showtris;
        }

        if (decal_batch_count == r_decal_capacity)
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
                                        alpha = powf (noisefall, 1.6f);
                                        float base = 0.15f + noisefall * 0.25f;
                                        r = base * 0.9f;
                                        g = base * 0.18f;
                                        b = base * 0.16f;
                                }
                                break;
                        }

                        case DECAL_SCORCH:
                        {
                                const float radius = 0.9f;
                                if (dist <= radius)
                                {
                                        float falloff = 1.0f - (dist / radius);
                                        float ring = sinf ((fx * fx + fy * fy) * 6.0f) * 0.05f;
                                        float shade = CLAMP (0.f, falloff + ring, 1.f);
                                        alpha = powf (shade, 1.8f) * 0.85f;
                                        float core = 0.15f + shade * 0.35f;
                                        r = core * 0.4f;
                                        g = core * 0.36f;
                                        b = core * 0.32f;
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

        {
                const char *texname = "decal_generic";
                switch (type)
                {
                case DECAL_BULLET: texname = "decal_bullet"; break;
                case DECAL_BLOOD: texname = "decal_blood"; break;
                case DECAL_SCORCH: texname = "decal_scorch"; break;
                default: break;
                }

                r_decal_textures[type] = TexMgr_LoadImage (NULL,
                        texname,
                        DECAL_TEXTURE_SIZE, DECAL_TEXTURE_SIZE, SRC_RGBA, data, "", 0,
                        TEXPREF_ALPHA | TEXPREF_PERSIST | TEXPREF_CLAMP | TEXPREF_NOPICMIP);
        }
}

void R_InitDecals (void)
{
        int i;

        Cvar_RegisterVariable (&r_decals_cvar);
        Cvar_RegisterVariable (&r_decals_blood_cvar);
        Cvar_RegisterVariable (&r_decals_bullet_cvar);
        Cvar_RegisterVariable (&r_decals_max_cvar);
        Cvar_RegisterVariable (&r_decals_debug_cvar);

        R_ReserveDecalStorage (R_DECAL_INITIAL_CAPACITY);

        for (i = 0; i < DECAL_COUNT; i++)
                R_GenerateDecalTexture ((decaltype_t)i);

        R_ClearDecals ();
}

void R_ClearDecals (void)
{
        if (r_decals && r_decal_capacity > 0)
                memset (r_decals, 0, (size_t)r_decal_capacity * sizeof (*r_decals));
        memset (r_decal_death_spawned, 0, sizeof (r_decal_death_spawned));
        r_active_decal_count = 0;
        R_ClearDecalBatch ();
}

void R_UpdateDecals (void)
{
        int i, limit = R_GetDecalLimit ();
        double t = cl.time;
        qboolean debug = r_decals_debug_cvar.value != 0.f;

        if (limit <= 0)
        {
                R_RemoveExcessDecals (limit);
                R_ClearDecalBatch ();
                return;
        }

        if (r_active_decal_count <= 0 || !r_decals || r_decal_capacity <= 0)
        {
                R_ClearDecalBatch ();
                return;
        }

        for (i = 0; i < r_decal_capacity; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active)
                        continue;
                if (!dec->texture)
                {
                        R_DeactivateDecal (dec);
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
        double oldest_spawn = HUGE_VAL;
        decal_t *inactive = NULL;
        int active = 0;

        if (limit <= 0 || !r_decals || r_decal_capacity <= 0)
                return NULL;

        for (i = 0; i < r_decal_capacity; i++)
        {
                decal_t *dec = &r_decals[i];
                if (dec->active)
                {
                        active++;
                        if (dec->spawn_time < oldest_spawn)
                        {
                                oldest_spawn = dec->spawn_time;
                                oldest = dec;
                        }
                }
                else if (!inactive)
                        inactive = dec;
        }

        if (active < limit && inactive)
                return inactive;

        if (active < limit && !inactive)
                return oldest;

        if (active >= limit)
        {
                if (oldest)
                {
                        R_DeactivateDecal (oldest);
                        return oldest;
                }
        }

        if (inactive)
                return inactive;
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

static float R_ComputeBloodDecalRadius (float scale)
{
        float base_radius = R_RandomRange (BLOOD_DECAL_MIN_RADIUS, BLOOD_DECAL_MAX_RADIUS);
        float random_scale = R_RandomRange (1.f, 2.5f);
        float radius = base_radius * random_scale * BLOOD_DECAL_SIZE_SCALE;
        if (scale > 0.f)
                radius *= scale;
        return radius;
}

static void R_RandomUnitVector (vec3_t out)
{
        float z = R_RandomRange (-1.f, 1.f);
        float angle = R_RandomRange (0.f, (float)M_PI * 2.f);
        float radius = sqrtf (q_max (0.f, 1.f - z * z));
        out[0] = cosf (angle) * radius;
        out[1] = sinf (angle) * radius;
        out[2] = z;
}

static void R_RandomDirectionInCone (const vec3_t axis, float cone_angle_deg, vec3_t out)
{
        vec3_t norm_axis;
        float axis_len = VectorLength (axis);

        if (axis_len < 0.001f)
        {
                R_RandomUnitVector (out);
                return;
        }

        VectorScale (axis, 1.f / axis_len, norm_axis);

        {
                vec3_t basis_u, basis_v;
                float cone_angle = cone_angle_deg * ((float)M_PI / 180.f);
                float cos_min = cosf (cone_angle);
                float u = (float)rand () / (float)RAND_MAX;
                float cos_theta = 1.f - u * (1.f - cos_min);
                float sin_theta = sqrtf (q_max (0.f, 1.f - cos_theta * cos_theta));
                float phi = R_RandomRange (0.f, (float)M_PI * 2.f);
                vec3_t result;

                R_BuildOrthonormalBasis (norm_axis, basis_u, basis_v);

                VectorScale (norm_axis, cos_theta, result);
                VectorMA (result, cosf (phi) * sin_theta, basis_u, result);
                VectorMA (result, sinf (phi) * sin_theta, basis_v, result);
                VectorNormalizeFast (result);
                VectorCopy (result, out);
        }
}

static void R_AssignDecalVertices (decal_t *dec, const decalgeom_t *geom, float radius, float spec)
{
        vec3_t center, right, up;
        vec3_t sdir = {geom->sdir[0], geom->sdir[1], geom->sdir[2]};
        vec3_t tdir = {geom->tdir[0], geom->tdir[1], geom->tdir[2]};
        float angle = (float) rand () * (2.f * M_PI / (float) RAND_MAX);
        float c = cosf (angle), s = sinf (angle);
        vec3_t rs, rt;
        int i;
        vec3_t normal;

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

        VectorCopy (geom->normal, normal);
        if (VectorLengthSquared (normal) > 0.0001f)
                VectorNormalizeFast (normal);
        else
        {
                normal[0] = 0.f;
                normal[1] = 0.f;
                normal[2] = 1.f;
        }

        for (i = 0; i < 4; i++)
        {
                dec->verts[i].normal_spec[0] = normal[0];
                dec->verts[i].normal_spec[1] = normal[1];
                dec->verts[i].normal_spec[2] = normal[2];
                dec->verts[i].normal_spec[3] = spec;
        }
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

        if (dec->active)
                R_DeactivateDecal (dec);

        dec->texture = dec->base_texture;
        dec->type = type;
        dec->radius = radius;
        VectorCopy (geom->origin, dec->origin);
        VectorCopy (geom->normal, dec->normal);
        dec->lightcache.surfidx = 0;
        dec->last_light_update = 0.0;
        dec->spawn_time = cl.time;
        dec->active = true;
        r_active_decal_count++;
        dec->tint[0] = tint ? tint[0] : 1.f;
        dec->tint[1] = tint ? tint[1] : 1.f;
        dec->tint[2] = tint ? tint[2] : 1.f;
        dec->tint[3] = tint ? tint[3] : 1.f;
        {
                float spec = 0.f;
                if (type == DECAL_BLOOD)
                        spec = 0.2f * CLAMP (0.f, dec->tint[3], 1.f);
                R_AssignDecalVertices (dec, geom, radius, spec);
        }

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
                const char *name = "blood";
                if (type == DECAL_BULLET)
                        name = "bullet";
                else if (type == DECAL_SCORCH)
                        name = "scorch";
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

void R_AddExplosionDecal (const vec3_t point)
{
        decalgeom_t geom;
        float radius;
        const float tint[4] = {0.32f, 0.29f, 0.26f, 0.78f};

        if (!r_decals_cvar.value || !r_decals_bullet_cvar.value)
                return;

        if (!R_DecalProject (point, vec3_origin, DECAL_MAX_DISTANCE, &geom))
                return;

        radius = R_RandomRange (EXPLOSION_DECAL_MIN_RADIUS, EXPLOSION_DECAL_MAX_RADIUS);
        if (!R_DecalHasEdgeRoom (&geom, radius))
                return;
        R_CreateDecal (&geom, radius, DECAL_SCORCH, tint);
}

static qboolean R_AddBloodDecalForDirection (const vec3_t point, const vec3_t preferred_normal, float maxdist, float min_z, decaltype_t type, float radius_override, const float *tint, decalgeom_t *out_geom)
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
                        radius = base_radius * scale * BLOOD_DECAL_SIZE_SCALE;
                }
                if (!R_DecalHasEdgeRoom (&geom, radius))
                        return false;
                if (out_geom)
                        *out_geom = geom;
                return R_CreateDecal (&geom, radius, type, tint);
        }
}

void R_AddBloodDecal (const vec3_t point, const vec3_t dir)
{
        vec3_t up = {0.f, 0.f, 1.f};
        vec3_t down = {0.f, 0.f, -1.f};
        const float tint[4] = {0.35f, 0.02f, 0.02f, 0.92f};
        vec3_t spray_axis = {0.f, 0.f, 1.f};
        float dir_len2 = VectorLengthSquared (dir);
        int spawn_count;
        int i;
        qboolean primary_spawned = false;
        decalgeom_t primary_geom;
        float primary_radius;

        if (!r_decals_cvar.value || !r_decals_blood_cvar.value)
                return;

        if (dir_len2 > 0.001f)
        {
                VectorCopy (dir, spray_axis);
                VectorNormalizeFast (spray_axis);
        }

        primary_radius = R_ComputeBloodDecalRadius (BLOOD_PRIMARY_RADIUS_SCALE);

        if (dir_len2 > 0.001f)
        {
                vec3_t preferred;
                vec3_t opposite;

                VectorCopy (spray_axis, preferred);
                VectorNormalizeFast (preferred);
                VectorScale (preferred, -1.f, opposite);

                primary_spawned = R_AddBloodDecalForDirection (point, opposite, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, primary_radius, tint, &primary_geom);
                if (!primary_spawned)
                        primary_spawned = R_AddBloodDecalForDirection (point, preferred, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, primary_radius, tint, &primary_geom);
        }

        if (!primary_spawned)
        {
                primary_spawned = R_AddBloodDecalForDirection (point, up, FLOOR_BLOOD_DISTANCE, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, primary_radius, tint, &primary_geom);
                if (!primary_spawned)
                        primary_spawned = R_AddBloodDecalForDirection (point, down, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, primary_radius, tint, &primary_geom);
        }

        if (primary_spawned && dir_len2 <= 0.001f)
        {
                VectorCopy (primary_geom.normal, spray_axis);
                VectorNormalizeFast (spray_axis);
        }

        spawn_count = BLOOD_CLUSTER_MIN;
        if (BLOOD_CLUSTER_MAX > BLOOD_CLUSTER_MIN)
                spawn_count += rand () % (BLOOD_CLUSTER_MAX - BLOOD_CLUSTER_MIN + 1);

        for (i = 0; i < spawn_count; i++)
        {
                vec3_t scatter_dir;
                vec3_t spawn_point;
                vec3_t preferred;
                vec3_t opposite;
                qboolean spawned = false;
                float dist = R_RandomRange (BLOOD_SPRAY_MIN_DISTANCE, BLOOD_SPRAY_MAX_DISTANCE);
                float radius_scale = R_RandomRange (BLOOD_SECONDARY_SCALE_MIN, BLOOD_SECONDARY_SCALE_MAX);
                float radius = R_ComputeBloodDecalRadius (radius_scale);
                float distance_span = BLOOD_SPRAY_MAX_DISTANCE - BLOOD_SPRAY_MIN_DISTANCE;

                if (distance_span > 1.f)
                {
                        float t = (dist - BLOOD_SPRAY_MIN_DISTANCE) / distance_span;
                        float falloff = 1.f - CLAMP (0.f, t, 1.f);
                        radius *= CLAMP (0.55f, falloff + R_RandomRange (-0.1f, 0.2f), 1.4f);
                }

                R_RandomDirectionInCone (spray_axis, BLOOD_SPRAY_CONE_ANGLE_DEG, scatter_dir);

                VectorCopy (point, spawn_point);
                VectorMA (spawn_point, dist, scatter_dir, spawn_point);
                spawn_point[0] += R_RandomRange (-BLOOD_SPRAY_LATERAL_JITTER, BLOOD_SPRAY_LATERAL_JITTER);
                spawn_point[1] += R_RandomRange (-BLOOD_SPRAY_LATERAL_JITTER, BLOOD_SPRAY_LATERAL_JITTER);
                spawn_point[2] += R_RandomRange (-BLOOD_SPRAY_LATERAL_JITTER * 0.35f, BLOOD_SPRAY_LATERAL_JITTER * 0.35f);
                spawn_point[2] -= dist * BLOOD_SPRAY_GRAVITY_FACTOR * R_RandomRange (0.6f, 1.1f);

                VectorCopy (scatter_dir, preferred);
                if (VectorLengthSquared (preferred) > 0.001f)
                {
                        VectorNormalizeFast (preferred);
                        VectorScale (preferred, -1.f, opposite);

                        spawned |= R_AddBloodDecalForDirection (spawn_point, opposite, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, radius, tint, NULL);
                        if (!spawned)
                                spawned |= R_AddBloodDecalForDirection (spawn_point, preferred, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, radius, tint, NULL);
                }

                if (!spawned)
                {
                        spawned = R_AddBloodDecalForDirection (spawn_point, up, FLOOR_BLOOD_DISTANCE, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, radius, tint, NULL);
                        if (!spawned)
                                R_AddBloodDecalForDirection (spawn_point, down, WALL_BLOOD_DISTANCE, -1.f, DECAL_BLOOD, radius, tint, NULL);
                }
        }
}

void R_AddGibDecal (const vec3_t point, int particle_count)
{
        vec3_t up = {0.f, 0.f, 1.f};
        vec3_t delta;
        float dist2;
        const float tint[4] = {0.4f, 0.03f, 0.03f, 0.95f};

        if (particle_count < GIB_DECAL_PARTICLE_THRESHOLD)
                return;

        if (!r_decals_cvar.value || !r_decals_blood_cvar.value)
                return;

        if (cl.time - r_last_gib_decal_time < 0.15)
        {
                VectorSubtract (point, r_last_gib_decal_origin, delta);
                dist2 = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
                if (dist2 < (24.f * 24.f))
                        return;
        }

        if (R_AddBloodDecalForDirection (point, up, FLOOR_BLOOD_DISTANCE * 1.5f, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, GIB_DECAL_RADIUS, tint, NULL))
        {
                r_last_gib_decal_time = cl.time;
                VectorCopy (point, r_last_gib_decal_origin);
        }
}

void R_DrawDecals (qboolean showtris)
{
        int i;
        int draw_count = 0;
        qboolean drew = false;

        if (!r_decals_cvar.value)
                return;

        if (r_active_decal_count <= 0 || !r_decals || r_decal_capacity <= 0 || !decal_draw_list)
        {
                R_ClearDecalBatch ();
                return;
        }

        for (i = 0; i < r_decal_capacity; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active)
                        continue;
                if (!dec->texture)
                        continue;
                decal_draw_list[draw_count++] = dec;
        }

        if (!draw_count)
        {
                R_ClearDecalBatch ();
                return;
        }

        qsort (decal_draw_list, (size_t) draw_count, sizeof (decal_draw_list[0]), R_CompareDecalTexture);

        for (i = 0; i < draw_count; i++)
        {
                decal_t *dec = decal_draw_list[i];

                if (!drew)
                {
                        qboolean dither = (softemu == SOFTEMU_COARSE && !showtris);
                        GL_BeginGroup ("Decals");
                        GL_UseProgram (glprogs.decals[dither]);
                        if (showtris)
                                GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (4));
                        else
                                GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (4));
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

        if (R_AddBloodDecalForDirection (base_point, up, FLOOR_BLOOD_DISTANCE * 1.5f, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, large_radius, pool_tint, NULL))
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

                if (R_AddBloodDecalForDirection (jitter, up, FLOOR_BLOOD_DISTANCE, BLOOD_WALL_THRESHOLD, DECAL_BLOOD, radius, small_tint, NULL))
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
