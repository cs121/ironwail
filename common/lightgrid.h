#pragma once

#include "quakedef.h"

typedef struct lightcell_s {
    vec3_t rgb;
    vec3_t dir;
    float intensity;
    float ao;
} lightcell_t;
typedef lightcell_t lightgrid_probe_t;

typedef enum lightgrid_backend_e {
    LIGHTGRID_BACKEND_NONE = 0,
    LIGHTGRID_BACKEND_OCTREE
} lightgrid_backend_t;

//
// LIGHTGRID_OCTREE BSPX lump layout (little-endian, 4-byte aligned)
//
// Header (lightgrid_octree_header_t):
//   magic   : 'LGOT' (LIGHTGRID_OCTREE_MAGIC)
//   version : currently LIGHTGRID_OCTREE_VERSION_1
//   flags   : bitfield (LIGHTGRID_OCTREE_FLAG_FLOAT32 required for v1)
//   bounds  : axis-aligned mins/maxs in world space (float32)
//   root    : index of the root node in the node array
//   counts  : node_count + leaf_count
//   offsets : byte offsets (from start of the lump) to the node array and
//             leaf array. Both must be 4-byte aligned and monotonically
//             increasing (nodes first, then leaves). leaf_stride specifies
//             the on-disk size of each leaf payload.
//
// Nodes (lightgrid_octree_disk_node_t):
//   8 uint32_t child slots. Each slot contains either:
//     * LIGHTGRID_OCTREE_CHILD_EMPTY (0xFFFFFFFF) for a missing child
//     * A node index (< node_count) with the high bit clear
//     * A leaf reference with LIGHTGRID_OCTREE_CHILD_LEAF flag set and the
//       low 31 bits holding the leaf index (< leaf_count)
//
// Leaves (lightgrid_octree_disk_leaf_t):
//   Payload encoding is defined by the header flags; v1 requires
//   LIGHTGRID_OCTREE_FLAG_FLOAT32 and stores:
//     rgb[3]      : float32
//     dir[3]      : float32 (unit or near-unit direction)
//     intensity   : float32
//   leaf_stride allows future packed/quantized payloads; for v1 it must be
//   >= sizeof(lightgrid_octree_disk_leaf_t). Implementations must skip any
//   trailing padding when advancing by leaf_stride bytes.
//
// All integer fields are little-endian; floats use IEEE-754 little-endian
// encoding. The loader validates bounds, counts, indices, and alignment
// before use.

#define LIGHTGRID_OCTREE_MAGIC 0x544f474cU /* 'LGOT' */
#define LIGHTGRID_OCTREE_VERSION_1 1u
#define LIGHTGRID_OCTREE_FLAG_FLOAT32 (1u << 0) // Payload stored as float32

#define LIGHTGRID_OCTREE_CHILD_EMPTY 0xFFFFFFFFu
#define LIGHTGRID_OCTREE_CHILD_LEAF  0x80000000u

#define LIGHTGRID_OCTREE_FLAG_LEAF     (1u << 31)
#define LIGHTGRID_OCTREE_FLAG_OCCLUDED (1u << 30)
#define LIGHTGRID_OCTREE_FLAG_MASK     (LIGHTGRID_OCTREE_FLAG_LEAF | LIGHTGRID_OCTREE_FLAG_OCCLUDED)

typedef struct lightgrid_octree_header_s {
    vec3_t grid_dist;
    int32_t grid_size[3];
    vec3_t grid_mins;
    uint8_t num_styles;
    uint32_t root_node;
} lightgrid_octree_header_t;

typedef struct lightgrid_octree_node_s {
    int32_t division_point[3];
    uint32_t child[8];
} lightgrid_octree_node_t;

typedef struct lightgrid_octree_sample_s {
    uint8_t color[3];
    uint8_t style;
} lightgrid_octree_sample_t;

typedef struct lightgrid_octree_sampleset_s {
    lightgrid_octree_sample_t samples[4];
    uint8_t used_samples;
    qboolean occluded;
} lightgrid_octree_sampleset_t;

typedef struct lightgrid_octree_leaf_s {
    int32_t mins[3];
    int32_t size[3];
    lightgrid_octree_sampleset_t *samples; // array of size[0] * size[1] * size[2]
} lightgrid_octree_leaf_t;

typedef struct lightgrid_octree_s {
    lightgrid_octree_header_t header;
    size_t node_count;
    size_t leaf_count;
    lightgrid_octree_node_t *nodes;
    lightgrid_octree_leaf_t *leaves;
} lightgrid_octree_t;

typedef struct lightgrid_s {
    lightgrid_backend_t backend;

    vec3_t mins, maxs;

    // Optional octree backend
    lightgrid_octree_t *octree;

    int source; // enum lightgrid_source_e
} lightgrid_t;

typedef enum lightgrid_source_e {
    LIGHTGRID_SRC_NONE = 0,
    LIGHTGRID_SRC_OCTREE
} lightgrid_source_t;

void Lightgrid_Free(lightgrid_t *lg);
