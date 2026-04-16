# BSPX Lump Support

This document summarizes the BSPX lumps Ironwail currently recognizes during BSP load.

## Load behavior

- BSPX entries are recorded on the brush model and then resolved through the normal BSP loader.
- Some lumps override classic payloads, others are only acknowledged, and a few are forwarded to dedicated loaders.

## Lighting and visibility

Current lighting precedence is:

1. external `.lit`
2. BSPX `LIGHTING_E5BGR9`
3. BSPX `RGBLIGHTING`
4. BSPX `DLIT`
5. BSPX `LIGHTING`
6. embedded classic `LUMP_LIGHTING`

Direction data is loaded from `LIGHTINGDIR` when the sample count matches the active lightmap.

Recognized lighting lumps:

- `LIGHTING_E5BGR9`
- `RGBLIGHTING`
- `DLIT`
- `LIGHTINGDIR`
- `LIGHTING`

## Geometry and normals

- `VERTEXNORMALS` and `FACENORMALS` are accepted when their sample counts match the loaded geometry.

## Lightgrid

- `LIGHTGRID_OCTREE` is the current lightgrid source used by the octree loader.
- Compatibility aliases `LIGHTRID`, `LGHTGRID`, and `LIGHTGRID` are routed to the same handler.

## Acknowledged lumps

These are recognized for logging/compatibility, but their payload is not consumed by the current runtime: `CUBEMAPS`, `MATERIALS`, `DECALS`, `MAPSCRIPTS`, `QC_EMBED`, `BSPX_SURFPROPS`, `BSPX_MODELNAMES`.

## Logging

- The loader logs which lumps were used or rejected so map authors can see exactly what the engine accepted.
