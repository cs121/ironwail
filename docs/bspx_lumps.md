# BSPX lump support in Ironwail

This document summarizes which BSPX lumps Ironwail loads and how they are used during BSP import.

## Lump overrides applied at load
When a BSPX directory is present on the main model, the loader remaps several legacy lumps so BSPX data replaces the BSP payload:

- `BSPX_VERTS` and `BSPX_FACES` override the standard vertex and face lumps.
- Lighting and entity visibility can be replaced via `LIGHTING`, `DLIT`, `RGBLIGHTING`, `VISX`, `PVS2`, and `PVS_COMPRESSED`.
- Entities and models can be swapped with `BSPX_ENTITYSTRING` and `BSPX_MODELS`.
These overrides run before the engine processes the classic lumps, so later loading code reads the BSPX-provided data transparently.

## Lighting-related BSPX lumps
The lighting loader checks several optional BSPX lumps, marking them used or unsupported depending on validity:

- `LIGHTING_E5BGR9`: HDR lighting; decoded into 24-bit RGB and used as the primary lightmap when the lump size is a multiple of 4 bytes.
- `RGBLIGHTING`: 24-bit lightmap data; used when the size is a multiple of 3 bytes.
- `DLIT`: dual-lightmap data with interleaved RGB and direction samples; only read when present alongside standard lighting and size is a multiple of 6 bytes.
- `LIGHTINGDIR`: separate light direction samples; used when the sample count matches the active lightmap.
- `DECOUPLED_LM`: per-face packed lightmap data that bypasses LM shift/offset handling when present.
- `LMSHIFT`, `LMOFFSET`: per-face lightmap shift and offset arrays; each must match the face count.
- `LMSTYLE16`, `LMSTYLE`: per-face style tables (16-bit or 8-bit entries) used to populate `styles` for each surface.

## Geometry and normal data
- `VERTEXNORMALS`: optional per-vertex normal vectors; accepted when the lump size equals the vertex count times the `vec3_t` size.
- `FACENORMALS`: optional per-face normals loaded after faces; used to support smoothing.

## Lightgrid support
- `LIGHTGRID_RAW` and `LIGHTGRID_OCTREE` are fed to the lightgrid loader. Successful parsing marks the lump as used; invalid data is marked unsupported.
- Compatibility aliases `LIGHTRID`, `LGHTGRID`, and `LIGHTGRID` are also routed to the same handler.

## Discovered-but-not-consumed lumps
During dispatch, several lump names are merely acknowledged so they are not reported as unsupported, but the engine does not currently consume their contents: `CUBEMAPS`, `MATERIALS`, `DECALS`, `MAPSCRIPTS`, `QC_EMBED`, `BSPX_SURFPROPS`, and `BSPX_MODELNAMES`.

## Usage logging
After loading, the engine logs which BSPX lumps were present, used, or rejected, ensuring mappers can see whether their data was recognized.
