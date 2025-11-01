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
#include <math.h>
#include <stdint.h>

#define MAX_DECALS              16384
#define DECAL_TEXTURE_SIZE      64
#define DECAL_OFFSET            0.25f

#define DECAL_GRID_SIZE         256.0f
#define DECAL_GRID_MAX_PER_CELL 32
#define DECAL_GRID_BUCKET_COUNT 2048

#define DECAL_MAX_DISTANCE      64.f
#define BULLET_DECAL_DISTANCE   12.f
#define WALL_BLOOD_DISTANCE     48.f
#define FLOOR_BLOOD_DISTANCE    72.f
#define BULLET_DECAL_MIN_RADIUS 10.0f
#define BULLET_DECAL_MAX_RADIUS 16.0f
#define BLOOD_DECAL_MIN_RADIUS  12.0f
#define BLOOD_DECAL_MAX_RADIUS  25.0f

#define BLOOD_DECAL_SIZE_SCALE  0.5625f

#define EXPLOSION_DECAL_MIN_RADIUS       (BULLET_DECAL_MIN_RADIUS * 10.0f)
#define EXPLOSION_DECAL_MAX_RADIUS       (BULLET_DECAL_MAX_RADIUS * 10.0f)

#define GIB_DECAL_RADIUS         48.0f
#define GIB_DECAL_PARTICLE_THRESHOLD    80

#define BLOOD_POOL_RADIUS       22.0f
#define BLOOD_POOL_JITTER       12.0f
#define BLOOD_POOL_SMALL_MIN    20.0f
#define BLOOD_POOL_SMALL_MAX    48.0f

#define DECAL_LIGHT_REFRESH     0.1

#define BLOOD_WALL_THRESHOLD    0.65f

#define BLOOD_CLUSTER_MIN               7
#define BLOOD_CLUSTER_MAX               13
#define BLOOD_PRIMARY_RADIUS_SCALE      1.35f
#define BLOOD_SECONDARY_SCALE_MIN       0.65f
#define BLOOD_SECONDARY_SCALE_MAX       1.1f
#define BLOOD_SPRAY_CONE_ANGLE_DEG      35.f
#define BLOOD_SPRAY_MIN_DISTANCE        6.f
#define BLOOD_SPRAY_MAX_DISTANCE        56.f
#define BLOOD_SPRAY_GRAVITY_FACTOR      0.32f
#define BLOOD_SPRAY_LATERAL_JITTER      3.5f

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
        float           base_alpha;
        vec3_t          light_color;
        lightcache_t    lightcache;
        double          last_light_update;
        decalvertex_t   verts[4];
        gltexture_t     *normal_map;
        qboolean        use_normal_map;
        unsigned int    flags;
        int             grid_bucket;
        int             grid_slot;
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

typedef struct decal_search_state_s
{
        vec3_t point;
        vec3_t preferred_normal;
        float preferred_len;
        float maxdist;
        float best_score;
        decalgeom_t *geom;
} decal_search_state_t;

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
static cvar_t    r_decals_max_cvar = {"r_decals_max", "4096", CVAR_ARCHIVE};
static cvar_t    r_decals_debug_cvar = {"r_decals_debug", "0", 0};

extern vec3_t lightcolor;

static byte r_decal_death_spawned[MAX_EDICTS];
static double r_last_gib_decal_time = -9999.0;
static vec3_t r_last_gib_decal_origin = {0.f, 0.f, 0.f};
static int r_active_decal_count = 0;

typedef enum {
        DECAL_FLAG_ACTIVE        = (1u << 0),
        DECAL_FLAG_NEEDS_UPDATE  = (1u << 1),
        DECAL_FLAG_PERMANENT     = (1u << 2)
} decal_flags_t;

typedef struct decal_pool_s {
        int free_list[MAX_DECALS];
        int free_count;
} decal_pool_t;

typedef struct decal_grid_cell_s {
        int decals[DECAL_GRID_MAX_PER_CELL];
        int count;
} decal_grid_cell_t;

typedef struct decal_grid_bucket_s {
        qboolean used;
        qboolean ever_used;
        int cell_x;
        int cell_y;
        int cell_z;
        decal_grid_cell_t cell;
} decal_grid_bucket_t;

typedef struct decal_stats_s {
        int total_spawned;
        int total_culled;
        int batch_count;
        float avg_batch_size;
} decal_stats_t;

static decal_pool_t decal_pool;
static decal_grid_bucket_t decal_grid[DECAL_GRID_BUCKET_COUNT];
static decal_stats_t decal_stats;

static void R_PrintDecalStats (void)
{
        Con_Printf ("Decals: %d active, %d batches, avg %.1f per batch\n",
                r_active_decal_count,
                decal_stats.batch_count,
                decal_stats.avg_batch_size);
}

static int R_GetDecalLimit (void)
{
        return CLAMP (0, (int) r_decals_max_cvar.value, MAX_DECALS);
}

static void R_InitDecalPool (void)
{
        int i;
        decal_pool.free_count = MAX_DECALS;
        for (i = 0; i < MAX_DECALS; i++)
                decal_pool.free_list[i] = i;
}

static void R_ResetDecalGrid (void)
{
        memset (decal_grid, 0, sizeof (decal_grid));
}

static int R_DecalIndex (const decal_t *dec)
{
        if (!dec)
                return -1;
        return (int)(dec - r_decals);
}

static int R_DecalGridCoord (float value)
{
        return (int)floorf (value / DECAL_GRID_SIZE);
}

static uint32_t R_DecalGridHash (int cx, int cy, int cz)
{
        uint32_t x = (uint32_t)cx * 73856093u;
        uint32_t y = (uint32_t)cy * 19349663u;
        uint32_t z = (uint32_t)cz * 83492791u;
        return (x ^ y ^ z) & (DECAL_GRID_BUCKET_COUNT - 1);
}

static decal_grid_bucket_t *R_DecalGridFind (int cx, int cy, int cz, qboolean create)
{
        uint32_t hash = R_DecalGridHash (cx, cy, cz);
        uint32_t i;

        for (i = 0; i < DECAL_GRID_BUCKET_COUNT; i++)
        {
                uint32_t idx = (hash + i) & (DECAL_GRID_BUCKET_COUNT - 1);
                decal_grid_bucket_t *bucket = &decal_grid[idx];

                if (bucket->used && bucket->cell_x == cx && bucket->cell_y == cy && bucket->cell_z == cz)
                        return bucket;

                if (!bucket->used)
                {
                        if (!bucket->ever_used)
                        {
                                if (!create)
                                        return NULL;

                                bucket->ever_used = true;
                                bucket->used = true;
                                bucket->cell_x = cx;
                                bucket->cell_y = cy;
                                bucket->cell_z = cz;
                                bucket->cell.count = 0;
                                return bucket;
                        }

                        if (create)
                        {
                                bucket->used = true;
                                bucket->cell_x = cx;
                                bucket->cell_y = cy;
                                bucket->cell_z = cz;
                                bucket->cell.count = 0;
                                return bucket;
                        }
                }
        }

        return NULL;
}

static void R_DecalGridDetach (decal_t *dec)
{
        if (!dec)
                return;

        if (dec->grid_bucket < 0 || dec->grid_bucket >= DECAL_GRID_BUCKET_COUNT)
        {
                dec->grid_bucket = -1;
                dec->grid_slot = -1;
                return;
        }

        decal_grid_bucket_t *bucket = &decal_grid[dec->grid_bucket];
        if (!bucket->used || dec->grid_slot < 0 || dec->grid_slot >= bucket->cell.count)
        {
                dec->grid_bucket = -1;
                dec->grid_slot = -1;
                return;
        }

        bucket->cell.count--;
        if (dec->grid_slot < bucket->cell.count)
        {
                int moved = bucket->cell.decals[bucket->cell.count];
                bucket->cell.decals[dec->grid_slot] = moved;
                r_decals[moved].grid_slot = dec->grid_slot;
        }
        bucket->cell.decals[bucket->cell.count] = -1;

        if (bucket->cell.count <= 0)
                bucket->used = false;

        dec->grid_bucket = -1;
        dec->grid_slot = -1;
}

static void R_DecalGridAttach (decal_t *dec, int decal_index)
{
        int cx, cy, cz;
        decal_grid_bucket_t *bucket;

        if (!dec)
                return;

        cx = R_DecalGridCoord (dec->origin[0]);
        cy = R_DecalGridCoord (dec->origin[1]);
        cz = R_DecalGridCoord (dec->origin[2]);

        bucket = R_DecalGridFind (cx, cy, cz, true);
        if (!bucket)
        {
                dec->grid_bucket = -1;
                dec->grid_slot = -1;
                return;
        }

        if (bucket->cell.count >= DECAL_GRID_MAX_PER_CELL)
        {
                dec->grid_bucket = -1;
                dec->grid_slot = -1;
                return;
        }

        dec->grid_bucket = (int)(bucket - decal_grid);
        dec->grid_slot = bucket->cell.count;
        bucket->cell.decals[bucket->cell.count++] = decal_index;
}

static void R_DeactivateDecal (decal_t *dec)
{
        int index;

        if (!dec || !dec->active)
                return;

        index = R_DecalIndex (dec);

        R_DecalGridDetach (dec);

        dec->active = false;
        dec->texture = NULL;
        dec->base_texture = NULL;
        dec->last_light_update = 0.0;
        dec->spawn_time = 0.0;
        dec->base_alpha = 0.f;
        dec->normal_map = NULL;
        dec->use_normal_map = false;
        dec->flags = 0;

        if (r_active_decal_count > 0)
                r_active_decal_count--;

        if (index >= 0 && decal_pool.free_count < MAX_DECALS)
                decal_pool.free_list[decal_pool.free_count++] = index;
}

static decal_t *R_FindOldestDecal (void)
{
        double oldest_spawn = HUGE_VAL;
        decal_t *oldest = NULL;
        int i;

        for (i = 0; i < MAX_DECALS; i++)
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

        return oldest;
}

static void R_RemoveExcessDecals (int limit)
{
        int to_remove;

        if (limit <= 0)
        {
                int i;
                for (i = 0; i < MAX_DECALS; i++)
                        R_DeactivateDecal (&r_decals[i]);
                return;
        }

        if (r_active_decal_count <= limit)
                return;

        to_remove = r_active_decal_count - limit;
        while (to_remove-- > 0)
        {
                decal_t *oldest = R_FindOldestDecal ();
                if (!oldest)
                        break;
                R_DeactivateDecal (oldest);
        }
}

static void R_UpdateDecalAlpha (decal_t *dec, double time)
{
        double age;
        float fade_start = 30.0f;
        float fade_duration = 10.0f;

        if (!dec)
                return;

        age = time - dec->spawn_time;
        if (age <= fade_start)
        {
                if (dec->tint[3] != dec->base_alpha)
                {
                        dec->tint[3] = dec->base_alpha;
                        dec->flags |= DECAL_FLAG_NEEDS_UPDATE;
                }
                return;
        }

        {
                float fade = 1.0f - CLAMP (0.f, (float)((age - fade_start) / fade_duration), 1.f);
                dec->tint[3] = dec->base_alpha * fade;
                dec->flags |= DECAL_FLAG_NEEDS_UPDATE;
        }
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

static qboolean R_DecalInFrustum (const decal_t *dec)
{
        vec3_t mins, maxs;
        int i;

        if (!dec)
                return false;

        VectorCopy (dec->verts[0].pos, mins);
        VectorCopy (dec->verts[0].pos, maxs);

        for (i = 1; i < 4; i++)
        {
                const float *pos = dec->verts[i].pos;
                mins[0] = q_min (mins[0], pos[0]);
                mins[1] = q_min (mins[1], pos[1]);
                mins[2] = q_min (mins[2], pos[2]);
                maxs[0] = q_max (maxs[0], pos[0]);
                maxs[1] = q_max (maxs[1], pos[1]);
                maxs[2] = q_max (maxs[2], pos[2]);
        }

        return !R_CullBox (mins, maxs);
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
        if (GL_DrawElementsInstancedFunc)
                GL_DrawElementsInstancedFunc (GL_TRIANGLES, decal_batch_count * 6, GL_UNSIGNED_SHORT, ofs, 1);
        else
                glDrawElements (GL_TRIANGLES, decal_batch_count * 6, GL_UNSIGNED_SHORT, ofs);

        R_ClearDecalBatch ();
}

static int R_CompareDecalsByTexture (const void *a, const void *b)
{
        const decal_t *const *da = (const decal_t *const *)a;
        const decal_t *const *db = (const decal_t *const *)b;
        intptr_t texdiff;

        if (!da || !db)
                return 0;

        texdiff = (intptr_t)(*da)->texture - (intptr_t)(*db)->texture;
        if (texdiff < 0)
                return -1;
        if (texdiff > 0)
                return 1;

        if ((*da)->spawn_time < (*db)->spawn_time)
                return -1;
        if ((*da)->spawn_time > (*db)->spawn_time)
                return 1;
        return 0;
}

static void R_SortDecalsByTexture (decal_t **list, int count)
{
        if (!list || count <= 1)
                return;
        qsort (list, count, sizeof (decal_t *), R_CompareDecalsByTexture);
}

static qboolean R_AppendDecalToBatch (decal_t *dec, qboolean showtris)
{
        qboolean flushed = false;

        if (!dec->texture)
                return false;

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
                flushed = true;
        }

        if (decal_batch_count == MAX_DECALS)
                return flushed;

        memcpy (&decal_batch[decal_batch_count * 4], dec->verts, sizeof(dec->verts));
        decal_batch_count++;
        return flushed;
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

        for (i = 0; i < DECAL_COUNT; i++)
                R_GenerateDecalTexture ((decaltype_t)i);

        R_InitDecalPool ();
        R_ResetDecalGrid ();
        memset (&decal_stats, 0, sizeof (decal_stats));
        R_ClearDecals ();
}

void R_ClearDecals (void)
{
        memset (r_decals, 0, sizeof (r_decals));
        memset (r_decal_death_spawned, 0, sizeof (r_decal_death_spawned));
        r_active_decal_count = 0;
        R_InitDecalPool ();
        R_ResetDecalGrid ();
        memset (&decal_stats, 0, sizeof (decal_stats));
        {
                int i;
                for (i = 0; i < MAX_DECALS; i++)
                {
                        r_decals[i].grid_bucket = -1;
                        r_decals[i].grid_slot = -1;
                }
        }
        R_ClearDecalBatch ();
}

void R_UpdateDecals (void)
{
        int i, limit = R_GetDecalLimit ();
        double t = cl.time;
        qboolean debug = r_decals_debug_cvar.value != 0.f;

        R_RemoveExcessDecals (limit);

        if (r_active_decal_count <= 0)
                return;

        for (i = 0; i < MAX_DECALS; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active)
                        continue;
                if (!dec->texture)
                {
                        R_DeactivateDecal (dec);
                        continue;
                }

                R_UpdateDecalAlpha (dec, t);

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
                        dec->flags &= ~DECAL_FLAG_NEEDS_UPDATE;
                        continue;
                }

                if ((t - dec->last_light_update > DECAL_LIGHT_REFRESH) || (dec->flags & DECAL_FLAG_NEEDS_UPDATE))
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
                        dec->flags &= ~DECAL_FLAG_NEEDS_UPDATE;
                }
        }
}

static decal_t *R_AllocDecal (void)
{
        int limit = R_GetDecalLimit ();

        if (limit <= 0)
                return NULL;

        if (r_active_decal_count >= limit)
                R_RemoveExcessDecals (limit - 1);

        if (decal_pool.free_count <= 0)
        {
                decal_t *oldest = R_FindOldestDecal ();
                if (oldest)
                        R_DeactivateDecal (oldest);
        }

        if (decal_pool.free_count <= 0)
                return NULL;

        return &r_decals[decal_pool.free_list[--decal_pool.free_count]];
}

static qboolean R_SurfaceIsDecalable (const msurface_t *surf)
{
        int flags = SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWFENCE | SURF_DRAWTELE | SURF_DRAWLAVA | SURF_DRAWSLIME | SURF_DRAWWATER | SURF_DRAWSPRITE;
        return (surf->flags & flags) == 0;
}

static qboolean R_ValidateDecalGeometry (const decalgeom_t *geom)
{
        int axis;

        if (!geom)
                return false;

        if (VectorLengthSquared (geom->normal) < 0.001f)
                return false;

        for (axis = 0; axis < 3; axis++)
        {
                if (isnan (geom->origin[axis]) || isnan (geom->normal[axis]))
                        return false;
        }

        return true;
}

static void R_EvaluateDecalSurface (const msurface_t *surf, decal_search_state_t *state)
{
        vec3_t normal;
        vec3_t origin;
        mtexinfo_t *texinfo;
        vec3_t sdir, tdir;
        float plane_dist, d, score;

        if (!surf || !state || !state->geom)
                return;

        if (!R_SurfaceIsDecalable (surf))
                return;

        VectorCopy (surf->plane->normal, normal);
        plane_dist = surf->plane->dist;
        if (surf->flags & SURF_PLANEBACK)
        {
                VectorScale (normal, -1.f, normal);
                plane_dist = -plane_dist;
        }

        d = DotProduct (state->point, normal) - plane_dist;
        if (fabsf (d) > state->maxdist)
                return;

        VectorMA (state->point, -d, normal, origin);

        {
                float margin = 4.f;
                if (origin[0] < surf->mins[0] - margin || origin[0] > surf->maxs[0] + margin ||
                        origin[1] < surf->mins[1] - margin || origin[1] > surf->maxs[1] + margin ||
                        origin[2] < surf->mins[2] - margin || origin[2] > surf->maxs[2] + margin)
                        return;
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
        if (state->preferred_len > 0.01f)
        {
                vec3_t pn;
                VectorCopy (state->preferred_normal, pn);
                VectorNormalizeFast (pn);
                score += DotProduct (normal, pn) * 0.5f;
        }

        if (score > state->best_score)
        {
                state->best_score = score;
                VectorCopy (origin, state->geom->origin);
                VectorCopy (normal, state->geom->normal);
                VectorNormalizeFast (state->geom->normal);
                VectorCopy (sdir, state->geom->sdir);
                VectorNormalizeFast (state->geom->sdir);
                VectorCopy (tdir, state->geom->tdir);
                VectorNormalizeFast (state->geom->tdir);
                VectorCopy (surf->mins, state->geom->mins);
                VectorCopy (surf->maxs, state->geom->maxs);
        }
}

static void R_FindBestDecalSurfaceNode (mnode_t *node, decal_search_state_t *state)
{
        if (!node || !state || !state->geom)
                return;

        if (node->contents < 0)
        {
                mleaf_t *leaf = (mleaf_t *)node;
                int i;

                for (i = 0; i < leaf->nummarksurfaces; i++)
                {
                        int surf_index = leaf->firstmarksurface[i];
                        if (surf_index < 0 || surf_index >= cl.worldmodel->numsurfaces)
                                continue;
                        R_EvaluateDecalSurface (&cl.worldmodel->surfaces[surf_index], state);
                }
                return;
        }

        {
                float dist = DotProduct (state->point, node->plane->normal) - node->plane->dist;
                mnode_t *front = node->children[dist >= 0.f ? 0 : 1];
                mnode_t *back = node->children[dist >= 0.f ? 1 : 0];

                if (front)
                        R_FindBestDecalSurfaceNode (front, state);

                if (back && fabsf (dist) <= state->maxdist)
                        R_FindBestDecalSurfaceNode (back, state);
        }
}

static qboolean R_FindBestDecalSurface (const vec3_t point, const vec3_t preferred_normal, float maxdist, decalgeom_t *out)
{
        decal_search_state_t state;

        if (!out || !cl.worldmodel || !cl.worldmodel->nodes)
                return false;

        VectorCopy (point, state.point);
        VectorCopy (preferred_normal, state.preferred_normal);
        state.preferred_len = VectorLength (preferred_normal);
        state.maxdist = maxdist;
        state.best_score = -99999.f;
        state.geom = out;

        R_FindBestDecalSurfaceNode (cl.worldmodel->nodes, &state);

        return state.best_score > -99999.f;
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
        return R_FindBestDecalSurface (point, preferred_normal, maxdist, out);
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

        if (!R_ValidateDecalGeometry (geom))
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
        dec->flags = DECAL_FLAG_ACTIVE | DECAL_FLAG_NEEDS_UPDATE;
        r_active_decal_count++;
        dec->tint[0] = tint ? tint[0] : 1.f;
        dec->tint[1] = tint ? tint[1] : 1.f;
        dec->tint[2] = tint ? tint[2] : 1.f;
        dec->tint[3] = tint ? tint[3] : 1.f;
        dec->base_alpha = CLAMP (0.f, dec->tint[3], 1.f);
        dec->normal_map = NULL;
        dec->use_normal_map = false;
        dec->grid_bucket = -1;
        dec->grid_slot = -1;
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

        R_DecalGridAttach (dec, R_DecalIndex (dec));
        decal_stats.total_spawned++;

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
        decal_t *draw_list[MAX_DECALS];
        int draw_count = 0;
        int culled = 0;
        int i;

        if (!r_decals_cvar.value)
                return;

        if (r_active_decal_count <= 0)
        {
                R_ClearDecalBatch ();
                return;
        }

        for (i = 0; i < DECAL_GRID_BUCKET_COUNT; i++)
        {
                decal_grid_bucket_t *bucket = &decal_grid[i];
                int j;

                if (!bucket->used || bucket->cell.count <= 0)
                        continue;

                {
                        vec3_t mins, maxs;
                        mins[0] = bucket->cell_x * DECAL_GRID_SIZE;
                        mins[1] = bucket->cell_y * DECAL_GRID_SIZE;
                        mins[2] = bucket->cell_z * DECAL_GRID_SIZE;
                        maxs[0] = mins[0] + DECAL_GRID_SIZE;
                        maxs[1] = mins[1] + DECAL_GRID_SIZE;
                        maxs[2] = mins[2] + DECAL_GRID_SIZE;

                        if (R_CullBox (mins, maxs))
                        {
                                culled += bucket->cell.count;
                                continue;
                        }
                }

                for (j = 0; j < bucket->cell.count && draw_count < MAX_DECALS; j++)
                {
                        int idx = bucket->cell.decals[j];
                        decal_t *dec;

                        if (idx < 0 || idx >= MAX_DECALS)
                                continue;

                        dec = &r_decals[idx];
                        if (!dec->active || !dec->texture)
                                continue;
                        if (!R_DecalInFrustum (dec))
                        {
                                culled++;
                                continue;
                        }

                        draw_list[draw_count++] = dec;
                }
        }

        for (i = 0; i < MAX_DECALS && draw_count < MAX_DECALS; i++)
        {
                decal_t *dec = &r_decals[i];
                if (!dec->active || !dec->texture)
                        continue;
                if (dec->grid_bucket >= 0)
                        continue;
                if (!R_DecalInFrustum (dec))
                {
                        culled++;
                        continue;
                }
                draw_list[draw_count++] = dec;
        }

        if (!draw_count)
        {
                decal_stats.batch_count = 0;
                decal_stats.avg_batch_size = 0.0f;
                R_ClearDecalBatch ();
                return;
        }

        R_SortDecalsByTexture (draw_list, draw_count);

        {
                qboolean dither = (softemu == SOFTEMU_COARSE && !showtris);
                int batches = 0;
                int rendered = 0;

                GL_BeginGroup ("Decals");
                GL_UseProgram (glprogs.decals[dither]);
                if (showtris)
                        GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (3));
                else
                        GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS (3));
                GL_PolygonOffset (OFFSET_DECAL);

                R_ClearDecalBatch ();

                for (i = 0; i < draw_count; i++)
                {
                        if (R_AppendDecalToBatch (draw_list[i], showtris))
                                batches++;
                        rendered++;
                }

                if (decal_batch_count > 0)
                {
                        R_FlushDecalBatch ();
                        batches++;
                }
                else
                        R_ClearDecalBatch ();

                GL_PolygonOffset (OFFSET_NONE);
                GL_EndGroup ();

                decal_stats.batch_count = batches;
                decal_stats.avg_batch_size = batches > 0 ? (float)rendered / (float)batches : 0.0f;
        }

        decal_stats.total_culled += culled;

        if (r_decals_debug_cvar.value > 1.0f)
                R_PrintDecalStats ();
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
