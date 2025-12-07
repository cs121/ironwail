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

    lightgrid_t *lg = (lightgrid_t *)Hunk_AllocName(sizeof(lightgrid_t), "lightgrid");
    if (!lg)
        return NULL;

    memset(lg, 0, sizeof(*lg));
    lg->probes = (lightgrid_probe_t *)Hunk_AllocName(total * sizeof(lightgrid_probe_t), "lightgrid_probes");
    if (!lg->probes)
        return NULL;

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

typedef struct bspxlg_node_s {
    int mid[3];
    int child[8];
} bspxlg_node_t;

typedef struct bspxlg_sample_s {
    byte style;
    byte rgb[3];
} bspxlg_sample_t;

typedef struct bspxlg_leaf_s {
    int mins[3];
    int size[3];
    unsigned char numstyles;
    bspxlg_sample_t *rgbvalues;
} bspxlg_leaf_t;

typedef struct bspxlg_grid_s {
    vec3_t gridscale;
    vec3_t mins;
    int count[3];
    bspxlg_sample_t *samples;
    bspxlg_node_t *nodes;
    bspxlg_leaf_t *leafs;
    unsigned int numnodes;
    unsigned int numleafs;
    unsigned int rootnode;
} bspxlg_grid_t;

typedef struct bspxlg_ctx_s {
    const byte *data;
    size_t ofs;
    size_t size;
} bspxlg_ctx_t;

#define BSPXLG_NODE_LEAF 0x80000000u

static byte BSPXLG_ReadByte(bspxlg_ctx_t *ctx)
{
    if (ctx->ofs >= ctx->size)
    {
        ctx->ofs++;
        return 0;
    }

    return ctx->data[ctx->ofs++];
}

static int BSPXLG_ReadInt(bspxlg_ctx_t *ctx)
{
    int r = 0;
    r |= (int)BSPXLG_ReadByte(ctx) << 0;
    r |= (int)BSPXLG_ReadByte(ctx) << 8;
    r |= (int)BSPXLG_ReadByte(ctx) << 16;
    r |= (int)BSPXLG_ReadByte(ctx) << 24;
    return r;
}

static float BSPXLG_ReadFloat(bspxlg_ctx_t *ctx)
{
    union { float f; int i; } u;
    u.i = BSPXLG_ReadInt(ctx);
    return u.f;
}

static void BSPXLG_FreeGrid(bspxlg_grid_t *grid)
{
    if (!grid)
        return;

    Z_Free(grid->samples);
    Z_Free(grid->leafs);
    Z_Free(grid->nodes);

    Z_Free(grid);
}

static bspxlg_grid_t *BSPXLG_Load(const bspx_lump_t *l)
{
    bspxlg_ctx_t ctx = { .data = (const byte *)l->data, .ofs = 0u, .size = l->size };
    bspxlg_grid_t *grid;
    bspxlg_sample_t *samples;
    unsigned int nodestart;

    vec3_t step;
    vec3_t gridscale;
    unsigned int numstyles, numnodes, numleafs, rootnode;
    unsigned int leafsamps = 0;

    if (!l || !l->data || l->size <= 0)
        return NULL;

    for (int j = 0; j < 3; j++)
        step[j] = BSPXLG_ReadFloat(&ctx);
    for (int j = 0; j < 3; j++)
        gridscale[j] = step[j] ? 1.0f / step[j] : 0.0f;

    vec3_t mins;
    int size[3];
    for (int j = 0; j < 3; j++)
        size[j] = BSPXLG_ReadInt(&ctx);
    for (int j = 0; j < 3; j++)
        mins[j] = BSPXLG_ReadFloat(&ctx);

    numstyles = BSPXLG_ReadByte(&ctx);
    (void)numstyles;
    rootnode = (unsigned int)BSPXLG_ReadInt(&ctx);
    numnodes = (unsigned int)BSPXLG_ReadInt(&ctx);
    nodestart = (unsigned int)ctx.ofs;

    ctx.ofs += (3 + 8) * sizeof(int) * numnodes;
    numleafs = (unsigned int)BSPXLG_ReadInt(&ctx);

    for (unsigned int i = 0; i < numleafs; i++)
    {
        unsigned int lsz[3];
        unsigned int total;
        unsigned int ms = 1;
        for (int j = 0; j < 3; j++)
            BSPXLG_ReadInt(&ctx);
        for (int j = 0; j < 3; j++)
            lsz[j] = (unsigned int)BSPXLG_ReadInt(&ctx);

        total = lsz[0] * lsz[1] * lsz[2];

        for (unsigned int j = 0; j < total; j++)
        {
            byte s = BSPXLG_ReadByte(&ctx);
            if (s == 255)
                continue;
            if (ms < s)
                ms = s;
            ctx.ofs += s * 4u;
        }

        if (total > 0 && ms > UINT_MAX / total)
            return NULL;

        leafsamps += total * ms;
    }

    grid = (bspxlg_grid_t *)Z_Malloc(sizeof(*grid));
    if (!grid)
        return NULL;

    memset(grid, 0, sizeof(*grid));
    grid->nodes = (bspxlg_node_t *)Z_Malloc(sizeof(*grid->nodes) * numnodes);
    grid->leafs = (bspxlg_leaf_t *)Z_Malloc(sizeof(*grid->leafs) * numleafs);
    samples = (bspxlg_sample_t *)Z_Malloc(sizeof(*samples) * leafsamps);

    if (!grid->nodes || !grid->leafs || !samples)
    {
        BSPXLG_FreeGrid(grid);
        if (samples)
            Z_Free(samples);
        return NULL;
    }

    VectorCopy(mins, grid->mins);
    for (int j = 0; j < 3; j++)
        grid->gridscale[j] = gridscale[j];
    grid->count[0] = size[0];
    grid->count[1] = size[1];
    grid->count[2] = size[2];
    grid->numnodes = numnodes;
    grid->numleafs = numleafs;
    grid->rootnode = rootnode;
    grid->samples = samples;

    ctx.ofs = nodestart;
    for (unsigned int i = 0; i < numnodes; i++)
    {
        for (int j = 0; j < 3; j++)
            grid->nodes[i].mid[j] = BSPXLG_ReadInt(&ctx);
        for (int j = 0; j < 8; j++)
            grid->nodes[i].child[j] = BSPXLG_ReadInt(&ctx);
    }

    BSPXLG_ReadInt(&ctx);

    for (unsigned int i = 0; i < numleafs; i++)
    {
        unsigned int total;
        unsigned int ms = 1;

        for (int j = 0; j < 3; j++)
            grid->leafs[i].mins[j] = BSPXLG_ReadInt(&ctx);
        for (int j = 0; j < 3; j++)
            grid->leafs[i].size[j] = BSPXLG_ReadInt(&ctx);

        total = (unsigned int)(grid->leafs[i].size[0] * grid->leafs[i].size[1] * grid->leafs[i].size[2]);
        grid->leafs[i].rgbvalues = samples;

        {
            size_t leafdataofs = ctx.ofs;
            for (unsigned int j = 0; j < total; j++)
            {
                byte s = BSPXLG_ReadByte(&ctx);
                if (s == 0xff)
                    continue;
                if (ms < s)
                    ms = s;
                ctx.ofs += s * 4u;
            }
            grid->leafs[i].numstyles = (unsigned char)ms;
            ctx.ofs = leafdataofs;
        }

        while (total-- > 0)
        {
            byte s = BSPXLG_ReadByte(&ctx);
            if (s == 0xff)
            {
                memset(samples, 0xff, sizeof(*samples));
                samples += ms;
                continue;
            }

            for (unsigned int k = 0; k < s; k++)
            {
                if (k >= ms)
                {
                    BSPXLG_ReadInt(&ctx);
                }
                else
                {
                    samples[k].style = BSPXLG_ReadByte(&ctx);
                    samples[k].rgb[0] = BSPXLG_ReadByte(&ctx);
                    samples[k].rgb[1] = BSPXLG_ReadByte(&ctx);
                    samples[k].rgb[2] = BSPXLG_ReadByte(&ctx);
                }
            }
            for (unsigned int k = s; k < ms; k++)
            {
                samples[k].style = k ? (byte)~0u : 0;
                samples[k].rgb[0] = samples[k].rgb[1] = samples[k].rgb[2] = 0;
            }

            samples += ms;
        }
    }

    if (ctx.ofs != ctx.size)
    {
        BSPXLG_FreeGrid(grid);
        return NULL;
    }

    return grid;
}

static float BSPXLG_SingleValue(const bspxlg_grid_t *grid, int x, int y, int z, float w, vec3_t res_diffuse)
{
    const bspxlg_node_t *n;
    const bspxlg_sample_t *samp;
    unsigned int node = grid->rootnode;
    int i;
    float lev;

    while (!(node & BSPXLG_NODE_LEAF))
    {
        if (node >= grid->numnodes)
            return 0;

        n = &grid->nodes[node];
        node = (unsigned int)n->child[((x >= n->mid[0]) << 2) | ((y >= n->mid[1]) << 1) | ((z >= n->mid[2]) << 0)];
    }

    const bspxlg_leaf_t *leaf = &grid->leafs[node & ~BSPXLG_NODE_LEAF];
    x -= leaf->mins[0];
    y -= leaf->mins[1];
    z -= leaf->mins[2];

    if (x >= leaf->size[0] || y >= leaf->size[1] || z >= leaf->size[2])
        return 0;

    i = x + leaf->size[0] * (y + leaf->size[1] * z);
    samp = leaf->rgbvalues + i * leaf->numstyles;

    for (i = 0; i < leaf->numstyles; i++)
    {
        if (samp[i].style == 255)
            break;

        if (samp[i].style < cl_max_lightstyles)
        {
            lev = d_lightstylevalue[samp[i].style] * w;
            res_diffuse[0] += samp[i].rgb[0] * lev * cl_lightstyle[samp[i].style].colours[0];
            res_diffuse[1] += samp[i].rgb[1] * lev * cl_lightstyle[samp[i].style].colours[1];
            res_diffuse[2] += samp[i].rgb[2] * lev * cl_lightstyle[samp[i].style].colours[2];
        }
    }

    if (i == 0)
        w = 0;

    return w;
}

static void BSPXLG_Sample(const bspxlg_grid_t *grid, const vec3_t point, vec3_t res_diffuse, vec3_t res_dir)
{
    int tile[3];
    float frac[3];
    float s = 0.f;

    VectorSet(res_diffuse, 0, 0, 0);
    VectorSet(res_dir, 1, 0, 1);
    VectorNormalize(res_dir);

    for (int i = 0; i < 3; i++)
    {
        tile[i] = (int)floorf((point[i] - grid->mins[i]) * grid->gridscale[i]);
        frac[i] = (point[i] - grid->mins[i]) * grid->gridscale[i] - tile[i];
    }

    for (int i = 0; i < 8; i++)
    {
        float w = ((i & 1) ? frac[0] : 1 - frac[0]) * ((i & 2) ? frac[1] : 1 - frac[1]) * ((i & 4) ? frac[2] : 1 - frac[2]);
        s += BSPXLG_SingleValue(grid, tile[0] + !!(i & 1), tile[1] + !!(i & 2), tile[2] + !!(i & 4), w, res_diffuse);
    }

    if (s)
        VectorScale(res_diffuse, 1.0f / (s * 255.0f), res_diffuse);
}

lightgrid_t *Lightgrid_LoadFromBSPX_FTE(const bspx_lump_t *l)
{
    bspxlg_grid_t *grid = BSPXLG_Load(l);
    if (!grid)
        return NULL;

    if (grid->count[0] <= 0 || grid->count[1] <= 0 || grid->count[2] <= 0)
    {
        BSPXLG_FreeGrid(grid);
        return NULL;
    }

    lightgrid_t *lg = Lightgrid_Alloc(grid->count[0], grid->count[1], grid->count[2],
                                      grid->gridscale[0] ? 1.0f / grid->gridscale[0] : 0.0f,
                                      grid->mins, grid->mins); /* maxs will be filled below */

    if (!lg)
    {
        BSPXLG_FreeGrid(grid);
        return NULL;
    }

    vec3_t size;
    for (int i = 0; i < 3; i++)
        size[i] = grid->count[i] * (lg->cellsize > 0.f ? lg->cellsize : 0.f);
    VectorAdd(lg->mins, size, lg->maxs);

    for (int z = 0; z < lg->nz; z++)
    {
        for (int y = 0; y < lg->ny; y++)
        {
            for (int x = 0; x < lg->nx; x++)
            {
                vec3_t p;
                vec3_t diffuse, dir;
                lightgrid_probe_t *probe = &lg->probes[(z * lg->ny + y) * lg->nx + x];

                p[0] = lg->mins[0] + (x + 0.5f) * lg->cellsize;
                p[1] = lg->mins[1] + (y + 0.5f) * lg->cellsize;
                p[2] = lg->mins[2] + (z + 0.5f) * lg->cellsize;

                BSPXLG_Sample(grid, p, diffuse, dir);

                float intensity = VectorLength(diffuse);
                if (intensity > 0.f)
                {
                    VectorScale(diffuse, 1.0f / intensity, probe->rgb);
                    probe->intensity = intensity;
                }
                else
                {
                    probe->rgb[0] = probe->rgb[1] = probe->rgb[2] = 1.f;
                    probe->intensity = 0.f;
                }

                VectorCopy(dir, probe->dir);
            }
        }
    }

    BSPXLG_FreeGrid(grid);
    return lg;
}
