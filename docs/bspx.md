# BSPX Integration

Ironwail can read and cache [BSPX](https://www.quaddicted.com/files/maps/bspx.txt) footer data that is appended to a BSP file. The loader walks the footer table after the vanilla lumps have been parsed and keeps every recognised entry inside the `qmodel_t` so it is available for renderer, tools, and debugging code.

## Recognised lumps

| Lump name      | Purpose | Usage |
| -------------- | ------- | ----- |
| `VERTEXNORMALS` | Array of vertex normals stored as little-endian float triples. | Parsed into `qmodel_t::bspx_vertex_normals` for optional shading/decals when `r_bspx_normals` is enabled. |
| `FACENORMALS`   | Per-surface normals stored as float triples. | Used as the preferred normal in decal projection (`r_decals`), falling back to averaged vertex normals. |
| `RGBLIGHTING`   | Packed RGB lightmap samples. | Copied into the standard lightmap buffer when `r_bspx_lighting` is enabled and no external `.lit` file overrides the map. |
| `LIGHTINGDIR`   | Directional lighting samples aligned with the BSP lightmap. | When `r_bspx_lighting` is enabled the samples populate per-surface deluxemaps so shaders can reconstruct smooth normals; otherwise the data remains cached for tooling. |
| `STATICLIGHTS`  | Array of static point lights stored as float tuples. | Converted into GPU lights at runtime so BSPX maps can ship with authored static light sources. |
| `STATICSHADOWS` | Static shadow metadata. | When supplied as light tuples they are treated as additional shadow-casting lights; index payloads mark entries inside `STATICLIGHTS` that should feed the shadow system. |
| `ENVMAP`, `SURFENVMAP` | Additional material metadata. | Stored as opaque blobs for future renderer features. |
| `BRUSHLIST`     | List of brush indices. | Available through the `bspx_brushlist` console command for debugging tools. |
| `ZIP_PAKFILE`   | Embedded archive payload. | Parsed and stored as an opaque blob. |

Unknown lumps are preserved as opaque data and trigger a one-time developer warning so that maps can ship with forward-compatible extensions without breaking the loader.

## Safety checks

* Footer signature (`"BSPX"`) and table size are validated before data is copied from the map buffer.
* Lump offsets and sizes are clamped to the BSP file boundaries and must not exceed 64&nbsp;MiB per entry.
* Overlapping lump payloads are rejected to prevent accidental corruption.
* The loader degrades gracefully if parsing fails – the core BSP path remains untouched.

## CVars

| CVar | Default | Description |
| ---- | ------- | ----------- |
| `r_bspx_enable` | `1` | Master switch that enables BSPX parsing. When `0`, only vanilla BSP data is loaded. |
| `r_bspx_normals` | `1` | Allows renderer subsystems to consume BSPX normals. Decal projection uses them when this cvar is set. |
| `r_bspx_lighting` | `1` | Allows colour lightmaps supplied by `RGBLIGHTING` to replace the grayscale BSP lump when no `.lit` file is present. |
| `r_bspx_envmap` | `0` | Reserved flag for future environment mapping support. |

Changes to these CVars take effect on the next map load.

Directional lighting reconstructed from `LIGHTINGDIR` only activates while `r_bspx_lighting` is non-zero; the raw samples are still cached so tools can inspect them when the feature is disabled.

## Developer tooling

* `bspx_brushlist` – prints the indices contained in the `BRUSHLIST` lump (first 32 entries, followed by a summary when more are present). Useful when validating tooling pipelines.

## Opaque data access

Every lump – recognised or not – is retained inside `qmodel_t::bspx_lumps`. Mods or tools can request the raw blob through `Mod_BspxFindLump` and perform their own decoding without touching the loader.

## Failure modes

* If a lump fails validation (e.g. truncated payload or size mismatch) it is ignored and a developer warning is emitted.
* When `r_bspx_normals` is disabled the renderer falls back to the legacy plane normals.
* When `r_bspx_lighting` is disabled or a `.lit` file is present, vanilla lightmaps are preserved.
