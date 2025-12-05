#include "quakedef.h"
#include "lightgrid.h"

#define LIGHTGRID_MAGIC 0x4c475244u /* 'LGRD' */
#define LIGHTGRID_VERSION 1u
#define LIGHTGRID_MAX_DEPTH 32

#define NODE_STRIDE 4
#define LEAF_FLOATS 7

typedef struct lightgrid_decode_state_s {
    const byte *nodes;
    size_t node_count;
    size_t node_index;

    const float *leaves;
    size_t leaf_count;
    size_t leaf_index;

    lightgrid_t *grid;
    float cellsize;
    vec3_t mins;
    vec3_t maxs;
} lightgrid_decode_state_t;

static void Lightgrid_FillLeaf(lightgrid_decode_state_t *state, const vec3_t bmins, const vec3_t bmaxs, const float *leaf)
{
    vec3_t dir;
    int x0, y0, z0, x1, y1, z1;
    const lightgrid_t *grid = state->grid;

    dir[0] = LittleFloat(leaf[3]);
    dir[1] = LittleFloat(leaf[4]);
    dir[2] = LittleFloat(leaf[5]);

    x0 = (int)ceilf(((bmins[0] - state->mins[0]) / state->cellsize) - 0.5f);
    y0 = (int)ceilf(((bmins[1] - state->mins[1]) / state->cellsize) - 0.5f);
    z0 = (int)ceilf(((bmins[2] - state->mins[2]) / state->cellsize) - 0.5f);

    x1 = (int)floorf(((bmaxs[0] - state->mins[0]) / state->cellsize) - 0.5f);
    y1 = (int)floorf(((bmaxs[1] - state->mins[1]) / state->cellsize) - 0.5f);
    z1 = (int)floorf(((bmaxs[2] - state->mins[2]) / state->cellsize) - 0.5f);

    x0 = CLAMP(0, x0, grid->nx - 1);
    y0 = CLAMP(0, y0, grid->ny - 1);
    z0 = CLAMP(0, z0, grid->nz - 1);

    x1 = CLAMP(0, x1, grid->nx - 1);
    y1 = CLAMP(0, y1, grid->ny - 1);
    z1 = CLAMP(0, z1, grid->nz - 1);

    if (x1 < x0 || y1 < y0 || z1 < z0)
        return;

    if (VectorNormalize(dir) == 0.f)
    {
        dir[0] = dir[1] = 0.f;
        dir[2] = 1.f;
    }

    for (int z = z0; z <= z1; z++)
    {
        for (int y = y0; y <= y1; y++)
        {
            for (int x = x0; x <= x1; x++)
            {
                lightgrid_probe_t *probe = &grid->probes[(z * grid->ny + y) * grid->nx + x];

                probe->rgb[0] = LittleFloat(leaf[0]);
                probe->rgb[1] = LittleFloat(leaf[1]);
                probe->rgb[2] = LittleFloat(leaf[2]);

                VectorCopy(dir, probe->dir);
                probe->intensity = LittleFloat(leaf[6]);
            }
        }
    }
}

static qboolean Lightgrid_DecodeOctree(lightgrid_decode_state_t *state, int depth, const vec3_t bmins, const vec3_t bmaxs)
{
    if (depth > LIGHTGRID_MAX_DEPTH)
        return false;

    if (state->node_index >= state->node_count)
        return false;

    const byte child_mask = state->nodes[state->node_index * NODE_STRIDE];
    state->node_index++;

    if (!child_mask)
    {
        if (state->leaf_index >= state->leaf_count)
            return false;

        Lightgrid_FillLeaf(state, bmins, bmaxs, state->leaves + LEAF_FLOATS * state->leaf_index);
        state->leaf_index++;
        return true;
    }

    vec3_t mid;
    VectorAverage(bmins, bmaxs, mid);

    for (int i = 0; i < 8; i++)
    {
        if (!(child_mask & (1 << i)))
            continue;

        vec3_t child_mins, child_maxs;

        const qboolean x_pos = (i & 4) == 0;
        const qboolean y_pos = (i & 2) == 0;
        const qboolean z_pos = (i & 1) == 0;

        child_mins[0] = x_pos ? mid[0] : bmins[0];
        child_maxs[0] = x_pos ? bmaxs[0] : mid[0];

        child_mins[1] = y_pos ? mid[1] : bmins[1];
        child_maxs[1] = y_pos ? bmaxs[1] : mid[1];

        child_mins[2] = z_pos ? mid[2] : bmins[2];
        child_maxs[2] = z_pos ? bmaxs[2] : mid[2];

        if (!Lightgrid_DecodeOctree(state, depth + 1, child_mins, child_maxs))
            return false;
    }

    return true;
}

lightgrid_t *Lightgrid_Alloc(int nx, int ny, int nz, float cellsize, const vec3_t mins, const vec3_t maxs)
{
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return NULL;

    const size_t total = (size_t)nx * ny * nz;
    if (total > SIZE_MAX / sizeof(lightgrid_probe_t))
        return NULL;

    lightgrid_t *lg = (lightgrid_t *)Z_Malloc(sizeof(lightgrid_t));
    if (!lg)
        return NULL;

    memset(lg, 0, sizeof(*lg));
    lg->probes = (lightgrid_probe_t *)Z_Malloc(total * sizeof(lightgrid_probe_t));
    if (!lg->probes)
    {
        Z_Free(lg);
        return NULL;
    }

    lg->nx = nx;
    lg->ny = ny;
    lg->nz = nz;
    lg->cellsize = cellsize;
    VectorCopy(mins, lg->mins);
    VectorCopy(maxs, lg->maxs);

    return lg;
}

void Lightgrid_Free(lightgrid_t *lg)
{
    if (!lg)
        return;

    if (lg->probes)
        Z_Free(lg->probes);

    Z_Free(lg);
}

lightgrid_t *Lightgrid_LoadFromBSPX_Octree(const bspx_lump_t *l)
{
    if (!l || !l->data || l->size < sizeof(uint32_t) * 3 + sizeof(vec3_t) * 2 + sizeof(float))
        return NULL;

    const byte *data = (const byte *)l->data;
    size_t remaining = l->size;

    uint32_t magic = LittleLong(*(const uint32_t *)data);
    data += sizeof(uint32_t);
    remaining -= sizeof(uint32_t);

    uint32_t version = LittleLong(*(const uint32_t *)data);
    data += sizeof(uint32_t);
    remaining -= sizeof(uint32_t);

    vec3_t mins, maxs;
    memcpy(mins, data, sizeof(vec3_t));
    data += sizeof(vec3_t);
    remaining -= sizeof(vec3_t);

    memcpy(maxs, data, sizeof(vec3_t));
    data += sizeof(vec3_t);
    remaining -= sizeof(vec3_t);

    for (int i = 0; i < 3; i++)
    {
        mins[i] = LittleFloat(mins[i]);
        maxs[i] = LittleFloat(maxs[i]);
    }

    float cellsize = LittleFloat(*(const float *)data);
    data += sizeof(float);
    remaining -= sizeof(float);

    if (remaining < sizeof(uint32_t) * 2)
        return NULL;

    const uint32_t node_count = LittleLong(*(const uint32_t *)data);
    data += sizeof(uint32_t);
    const uint32_t leaf_count = LittleLong(*(const uint32_t *)data);
    data += sizeof(uint32_t);

    remaining -= sizeof(uint32_t) * 2;

    if (magic != LIGHTGRID_MAGIC || version != LIGHTGRID_VERSION)
        return NULL;

    if (!node_count || !leaf_count)
        return NULL;

    if (node_count > SIZE_MAX / NODE_STRIDE || leaf_count > SIZE_MAX / (sizeof(float) * LEAF_FLOATS))
        return NULL;

    size_t nodes_size = (size_t)node_count * NODE_STRIDE;
    size_t leaves_size = (size_t)leaf_count * sizeof(float) * LEAF_FLOATS;

    if (nodes_size > remaining)
        return NULL;
    if (leaves_size != remaining - nodes_size)
        return NULL;

    const byte *nodes = data;
    const float *leaves = (const float *)(data + nodes_size);

    vec3_t size;
    VectorSubtract(maxs, mins, size);
    if (cellsize <= 0.f)
        return NULL;

    int nx = (int)ceilf(size[0] / cellsize);
    int ny = (int)ceilf(size[1] / cellsize);
    int nz = (int)ceilf(size[2] / cellsize);

    if (nx <= 0 || ny <= 0 || nz <= 0)
        return NULL;

    lightgrid_t *lg = Lightgrid_Alloc(nx, ny, nz, cellsize, mins, maxs);
    if (!lg)
        return NULL;

    lightgrid_decode_state_t state = {
        .nodes = nodes,
        .node_count = node_count,
        .node_index = 0,
        .leaves = leaves,
        .leaf_count = leaf_count,
        .leaf_index = 0,
        .grid = lg,
        .cellsize = cellsize,
    };

    VectorCopy(mins, state.mins);
    VectorCopy(maxs, state.maxs);

    if (!Lightgrid_DecodeOctree(&state, 0, mins, maxs) || state.node_index != state.node_count || state.leaf_index != state.leaf_count)
    {
        Lightgrid_Free(lg);
        return NULL;
    }

    return lg;
}
