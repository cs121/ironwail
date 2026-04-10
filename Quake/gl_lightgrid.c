//
// Copyright (C)
// Lightgrid system (octree only)
//

#include "quakedef.h"
#include "glquake.h"
#include "gl_lightgrid.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>

static lightgrid_t *current_lightgrid = NULL;
static char lightgrid_source[16] = "NONE";

cvar_t r_lightgrid              = { "r_lightgrid", "1", CVAR_ARCHIVE };
cvar_t r_lightgrid_debug        = { "r_lightgrid_debug", "0", CVAR_NONE };
cvar_t r_lightgrid_octree_debug = { "r_lightgrid_octree_debug", "0", CVAR_NONE };
cvar_t r_lightgrid_force        = { "r_lightgrid_force", "0", CVAR_ARCHIVE };

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
static void Lightgrid_Dump_f(void);
static void Lightgrid_LogStats(const lightgrid_t *lg);
static int  Lightgrid_OctreeDepth(const lightgrid_octree_t *oct);

/* =====================================================================
   Init / Shutdown
   ===================================================================== */

void Lightgrid_Init(void)
{
    Cvar_RegisterVariable(&r_lightgrid);
    Cvar_RegisterVariable(&r_lightgrid_debug);
    Cvar_RegisterVariable(&r_lightgrid_octree_debug);
    Cvar_RegisterVariable(&r_lightgrid_force);
    Cmd_AddCommand("lightgrid_dump", Lightgrid_Dump_f);

    Lightgrid_Clear();
}

void Lightgrid_Shutdown(void)
{
    Lightgrid_Clear();
}

void Lightgrid_Clear(void)
{
    if (current_lightgrid)
        Lightgrid_Free(current_lightgrid);

    current_lightgrid = NULL;
    cl.lightgrid = NULL;
    q_strlcpy(lightgrid_source, "NONE", sizeof(lightgrid_source));
}

/* =====================================================================
   Backend helpers
   ===================================================================== */

typedef struct lightgrid_stats_s
{
    vec3_t min_rgb;
    vec3_t max_rgb;
    vec3_t sum_rgb;
    float min_ao;
    float max_ao;
    float sum_ao;
    size_t count;
    size_t occluded;
} lightgrid_stats_t;

static void Lightgrid_StatsInit(lightgrid_stats_t *stats)
{
    if (!stats)
        return;

    VectorSet(stats->min_rgb, FLT_MAX, FLT_MAX, FLT_MAX);
    VectorSet(stats->max_rgb, 0.f, 0.f, 0.f);
    VectorSet(stats->sum_rgb, 0.f, 0.f, 0.f);
    stats->min_ao = FLT_MAX;
    stats->max_ao = 0.f;
    stats->sum_ao = 0.f;
    stats->count = 0;
    stats->occluded = 0;
}

static void Lightgrid_StatsAccum(lightgrid_stats_t *stats, const vec3_t rgb, float ao, qboolean occluded)
{
    if (!stats)
        return;

    for (int i = 0; i < 3; i++)
    {
        stats->min_rgb[i] = q_min(stats->min_rgb[i], rgb[i]);
        stats->max_rgb[i] = q_max(stats->max_rgb[i], rgb[i]);
        stats->sum_rgb[i] += rgb[i];
    }

    stats->min_ao = q_min(stats->min_ao, ao);
    stats->max_ao = q_max(stats->max_ao, ao);
    stats->sum_ao += ao;

    if (occluded)
        stats->occluded++;

    stats->count++;
}

static const lightgrid_octree_sample_t *Lightgrid_SelectOctreeSample(const lightgrid_octree_sampleset_t *set)
{
    if (!set || set->used_samples == 0 || set->used_samples > 4)
        return NULL;

    for (int i = 0; i < set->used_samples; i++)
    {
        if (set->samples[i].style == 0)
            return &set->samples[i];
    }

    return &set->samples[0];
}

static qboolean Lightgrid_ValidateLeaf(const lightgrid_octree_t *oct, const lightgrid_octree_leaf_t *leaf, size_t leaf_index, qboolean verbose)
{
    if (!oct || !leaf || !leaf->samples)
        return false;

    for (int i = 0; i < 3; i++)
    {
        if (leaf->size[i] <= 0)
        {
            if (verbose)
                Con_Printf("LIGHTGRID_OCTREE leaf %zu has non-positive size (%d %d %d)\n", leaf_index, leaf->size[0], leaf->size[1], leaf->size[2]);
            return false;
        }
    }

    size_t count = (size_t)leaf->size[0] * (size_t)leaf->size[1] * (size_t)leaf->size[2];
    for (size_t i = 0; i < count; i++)
    {
        const lightgrid_octree_sampleset_t *set = &leaf->samples[i];

        if (set->occluded)
            continue;

        if (set->used_samples > 4 || set->used_samples > oct->header.num_styles)
        {
            if (verbose)
                Con_Printf("LIGHTGRID_OCTREE leaf %zu sample %zu has invalid used_samples %u\n", leaf_index, i, set->used_samples);
            return false;
        }
    }

    return true;
}

qboolean Lightgrid_ValidateOctree(const lightgrid_octree_t *oct, qboolean verbose)
{
    if (!oct)
        return false;

    if (!oct->node_count || !oct->leaf_count)
    {
        if (verbose)
            Con_Printf("LIGHTGRID_OCTREE missing nodes or leaves\n");
        return false;
    }

    if (oct->header.root_node >= oct->node_count)
    {
        if (verbose)
            Con_Printf("LIGHTGRID_OCTREE root %u outside node count %zu\n", oct->header.root_node, oct->node_count);
        return false;
    }

    for (int i = 0; i < 3; i++)
    {
        if (!R_IsFinite(oct->header.grid_dist[i]) || oct->header.grid_dist[i] <= 0.f)
        {
            if (verbose)
                Con_Printf("LIGHTGRID_OCTREE invalid grid dist %f axis %d\n", oct->header.grid_dist[i], i);
            return false;
        }

        if (!R_IsFinite(oct->header.grid_mins[i]))
        {
            if (verbose)
                Con_Printf("LIGHTGRID_OCTREE invalid grid mins %f axis %d\n", oct->header.grid_mins[i], i);
            return false;
        }
    }

    for (int i = 0; i < 3; i++)
    {
        if (oct->header.grid_size[i] <= 0)
        {
            if (verbose)
                Con_Printf("LIGHTGRID_OCTREE invalid grid size axis %d: %d\n", i, oct->header.grid_size[i]);
            return false;
        }
    }

    if (!oct->nodes || !oct->leaves)
        return false;

    for (size_t i = 0; i < oct->node_count; i++)
    {
        for (int c = 0; c < 8; c++)
        {
            uint32_t child = oct->nodes[i].child[c];
            if (child & LIGHTGRID_OCTREE_FLAG_OCCLUDED)
                continue;

            if (child & LIGHTGRID_OCTREE_FLAG_LEAF)
            {
                uint32_t leaf_index = child & ~LIGHTGRID_OCTREE_FLAG_MASK;
                if (leaf_index >= oct->leaf_count)
                {
                    if (verbose)
                        Con_Printf("LIGHTGRID_OCTREE node %zu child %d references leaf %u >= %zu\n", i, c, leaf_index, oct->leaf_count);
                    return false;
                }
            }
            else if (child >= oct->node_count)
            {
                if (verbose)
                    Con_Printf("LIGHTGRID_OCTREE node %zu child %d references node %u >= %zu\n", i, c, child, oct->node_count);
                return false;
            }
        }
    }

    for (size_t i = 0; i < oct->leaf_count; i++)
    {
        if (!Lightgrid_ValidateLeaf(oct, &oct->leaves[i], i, verbose))
            return false;
    }

    if (verbose)
        Con_Printf("LIGHTGRID_OCTREE validated (%zu nodes, %zu leaves)\n", oct->node_count, oct->leaf_count);

    return true;
}

static int Lightgrid_OctreeDepth(const lightgrid_octree_t *oct)
{
    int max_depth = 0;

    if (!oct || !oct->nodes || !oct->node_count)
        return 0;

    size_t stack_size = oct->node_count;
    uint32_t *node_stack = (uint32_t *)q_malloc(stack_size * sizeof(uint32_t));
    int *depth_stack = (int *)q_malloc(stack_size * sizeof(int));

    if (!node_stack || !depth_stack)
        goto done;

    size_t top = 0;
    node_stack[top] = oct->header.root_node;
    depth_stack[top] = 1;
    top++;

    while (top > 0)
    {
        top--;
        uint32_t node_index = node_stack[top];
        int depth = depth_stack[top];
        max_depth = q_max(max_depth, depth);

        const lightgrid_octree_node_t *node = &oct->nodes[node_index];
        for (int c = 0; c < 8; c++)
        {
            uint32_t child = node->child[c];
            if ((child & LIGHTGRID_OCTREE_FLAG_OCCLUDED) || (child & LIGHTGRID_OCTREE_FLAG_LEAF))
                continue;

            if (child < oct->node_count && top < stack_size)
            {
                node_stack[top] = child;
                depth_stack[top] = depth + 1;
                top++;
            }
        }
    }

done:
    if (node_stack)
        q_free(node_stack);
    if (depth_stack)
        q_free(depth_stack);

    return max_depth;
}

static void Lightgrid_LogStats(const lightgrid_t *lg)
{
    lightgrid_stats_t stats;
    Lightgrid_StatsInit(&stats);

    if (!lg || !lg->octree || !lg->octree->leaves)
        return;

    Con_Printf("LIGHTGRID_OCTREE header: dist(%.3f %.3f %.3f) size(%d %d %d) mins(%.3f %.3f %.3f) styles %u\n",
        lg->octree->header.grid_dist[0], lg->octree->header.grid_dist[1], lg->octree->header.grid_dist[2],
        lg->octree->header.grid_size[0], lg->octree->header.grid_size[1], lg->octree->header.grid_size[2],
        lg->octree->header.grid_mins[0], lg->octree->header.grid_mins[1], lg->octree->header.grid_mins[2],
        lg->octree->header.num_styles);

    for (size_t leaf_index = 0; leaf_index < lg->octree->leaf_count; leaf_index++)
    {
        const lightgrid_octree_leaf_t *leaf = &lg->octree->leaves[leaf_index];
        if (!leaf->samples)
            continue;

        size_t sample_count = (size_t)leaf->size[0] * (size_t)leaf->size[1] * (size_t)leaf->size[2];
        for (size_t i = 0; i < sample_count; i++)
        {
            const lightgrid_octree_sampleset_t *set = &leaf->samples[i];
            const lightgrid_octree_sample_t *sample = Lightgrid_SelectOctreeSample(set);
            vec3_t rgb = {0, 0, 0};
            float ao = 0.f;

            if (sample)
            {
                rgb[0] = sample->color[0] / 255.0f;
                rgb[1] = sample->color[1] / 255.0f;
                rgb[2] = sample->color[2] / 255.0f;
                ao = 1.f;
            }
            Lightgrid_StatsAccum(&stats, rgb, ao, set->occluded);
        }
    }

    if (!stats.count)
        return;

    vec3_t avg_rgb;
    VectorCopy(stats.sum_rgb, avg_rgb);
    for (int i = 0; i < 3; i++)
        avg_rgb[i] /= (float)stats.count;

    const float avg_ao = stats.sum_ao / (float)stats.count;
    const float occluded_pct = (stats.count > 0) ? (100.f * (float)stats.occluded / (float)stats.count) : 0.f;

    Con_Printf("Lightgrid stats: samples=%zu occluded=%.2f%%\n", stats.count, occluded_pct);
    Con_Printf(" RGB min(%.3f %.3f %.3f) max(%.3f %.3f %.3f) avg(%.3f %.3f %.3f)\n",
        stats.min_rgb[0], stats.min_rgb[1], stats.min_rgb[2],
        stats.max_rgb[0], stats.max_rgb[1], stats.max_rgb[2],
        avg_rgb[0], avg_rgb[1], avg_rgb[2]);
    Con_Printf(" AO  min(%.3f) max(%.3f) avg(%.3f)\n", stats.min_ao, stats.max_ao, avg_ao);
    Con_Printf(" Lightgrid colors treated as linear (no sRGB conversion).\n");
}

static void Lightgrid_DumpOctree(const lightgrid_octree_t *oct)
{
    if (!oct)
    {
        Con_Printf("No LIGHTGRID_OCTREE present\n");
        return;
    }

    int depth = Lightgrid_OctreeDepth(oct);
    Con_Printf("LIGHTGRID_OCTREE: %zu nodes, %zu leaves, depth %d\n",
        oct->node_count, oct->leaf_count, depth);
    Con_Printf(" grid: dist(%.1f %.1f %.1f) size(%d %d %d) mins(%.1f %.1f %.1f) styles %u\n",
        oct->header.grid_dist[0], oct->header.grid_dist[1], oct->header.grid_dist[2],
        oct->header.grid_size[0], oct->header.grid_size[1], oct->header.grid_size[2],
        oct->header.grid_mins[0], oct->header.grid_mins[1], oct->header.grid_mins[2],
        oct->header.num_styles);

    if (r_lightgrid_octree_debug.value > 0.f)
        Lightgrid_ValidateOctree(oct, true);
}

static void Lightgrid_Dump_f(void)
{
    const lightgrid_t *lg = Lightgrid_Get();

    if (!lg)
    {
        Con_Printf("No lightgrid loaded\n");
        return;
    }

    Lightgrid_DumpOctree(lg->octree);
}

/* =====================================================================
   Sampling
   ===================================================================== */

static void Lightgrid_DefaultSample(vec3_t out_color, float *out_ao)
{
    VectorSet(out_color, 1.f, 1.f, 1.f);
    if (out_ao)
        *out_ao = 1.f;
}

static qboolean Lightgrid_SampleOctreeProbe(const lightgrid_t *lg, const vec3_t pos, lightgrid_probe_t *out_probe)
{
    const lightgrid_octree_t *oct = lg ? lg->octree : NULL;
    int test_point[3];
    const lightgrid_octree_leaf_t *leaf = NULL;
    const lightgrid_octree_sampleset_t *set = NULL;
    uint32_t node_index;
    size_t steps = 0;

    if (!oct || !out_probe)
        return false;

    for (int i = 0; i < 3; i++)
    {
        float local = (pos[i] - oct->header.grid_mins[i]) / oct->header.grid_dist[i];
        test_point[i] = Q_rint(local);
        if (test_point[i] < 0 || test_point[i] >= oct->header.grid_size[i])
            return false;
    }

    node_index = oct->header.root_node;

    while (steps < (oct->node_count + oct->leaf_count))
    {
        if (node_index & LIGHTGRID_OCTREE_FLAG_OCCLUDED)
        {
            VectorSet(out_probe->rgb, 0.f, 0.f, 0.f);
            VectorSet(out_probe->dir, 0.f, 0.f, 1.f);
            out_probe->ao = 0.f;
            out_probe->intensity = 0.f;
            return true;
        }

        if (node_index & LIGHTGRID_OCTREE_FLAG_LEAF)
        {
            uint32_t leaf_index = node_index & ~LIGHTGRID_OCTREE_FLAG_MASK;
            if (leaf_index >= oct->leaf_count)
                return false;
            leaf = &oct->leaves[leaf_index];
            break;
        }

        if (node_index >= oct->node_count)
            return false;

        const lightgrid_octree_node_t *node = &oct->nodes[node_index];

        int signx = test_point[0] >= node->division_point[0];
        int signy = test_point[1] >= node->division_point[1];
        int signz = test_point[2] >= node->division_point[2];
        int child = 4 * signx + 2 * signy + signz;

        node_index = node->child[child];
        steps++;
    }

    if (!leaf)
        return false;

    if (leaf->size[0] <= 0 || leaf->size[1] <= 0 || leaf->size[2] <= 0)
        return false;

    int lx = CLAMP(0, test_point[0] - leaf->mins[0], leaf->size[0] - 1);
    int ly = CLAMP(0, test_point[1] - leaf->mins[1], leaf->size[1] - 1);
    int lz = CLAMP(0, test_point[2] - leaf->mins[2], leaf->size[2] - 1);

    size_t idx = ((size_t)leaf->size[0] * (size_t)leaf->size[1] * (size_t)lz) + ((size_t)leaf->size[0] * (size_t)ly) + (size_t)lx;
    set = &leaf->samples[idx];

    if (set->occluded)
    {
        VectorSet(out_probe->rgb, 0.f, 0.f, 0.f);
        VectorSet(out_probe->dir, 0.f, 0.f, 1.f);
        out_probe->ao = 0.f;
        out_probe->intensity = 0.f;
        return true;
    }

    const lightgrid_octree_sample_t *chosen = Lightgrid_SelectOctreeSample(set);

    if (chosen)
    {
        out_probe->rgb[0] = chosen->color[0] / 255.0f;
        out_probe->rgb[1] = chosen->color[1] / 255.0f;
        out_probe->rgb[2] = chosen->color[2] / 255.0f;
    }
    else
    {
        VectorSet(out_probe->rgb, 0.f, 0.f, 0.f);
    }

    VectorSet(out_probe->dir, 0.f, 0.f, 1.f);
    out_probe->ao = 1.f;
    out_probe->intensity = (out_probe->rgb[0] + out_probe->rgb[1] + out_probe->rgb[2]) * (1.f / 3.f);
    return true;
}

qboolean Lightgrid_SampleProbe(const lightgrid_t *lg, const vec3_t pos, lightgrid_probe_t *out_probe)
{
    if (!lg || !out_probe || !r_lightgrid.value)
        return false;

    if (lg->backend != LIGHTGRID_BACKEND_OCTREE)
        return false;

    return Lightgrid_SampleOctreeProbe(lg, pos, out_probe);
}

void Lightgrid_Sample(const vec3_t pos, vec3_t out_color, float *out_ao)
{
    const lightgrid_t *lg = Lightgrid_Get();
    lightgrid_probe_t probe;

    if (!Lightgrid_SampleProbe(lg, pos, &probe))
    {
        Lightgrid_DefaultSample(out_color, out_ao);
        return;
    }

    VectorCopy(probe.rgb, out_color);
    if (out_ao)
        *out_ao = CLAMP(0.f, probe.ao, 1.f);
}

/* =====================================================================
   Public API
   ===================================================================== */

void Lightgrid_Set(lightgrid_t *lg)
{
    Lightgrid_Clear();

    if (!lg)
        return;

    current_lightgrid = lg;
    cl.lightgrid = lg;

    switch (lg->source)
    {
    case LIGHTGRID_SRC_OCTREE:
        q_strlcpy(lightgrid_source, "OCTREE", sizeof(lightgrid_source));
        break;
    default:
        q_strlcpy(lightgrid_source, "UNKNOWN", sizeof(lightgrid_source));
        break;
    }

    if (r_lightgrid_debug.value >= 1.f)
        Lightgrid_LogStats(lg);
}

const lightgrid_t *Lightgrid_Get(void)
{
    return current_lightgrid ? current_lightgrid : cl.lightgrid;
}

const char *Lightgrid_GetSource(void)
{
    return lightgrid_source;
}
