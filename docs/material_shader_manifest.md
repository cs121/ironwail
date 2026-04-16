# Material Shader Manifest

This file is the manifest-style reference for Ironwail material shader syntax.
It describes the current engine-facing contract for `materials/*.material`.

## Scope
- Source format: `.material`
- Loaded through the material pipeline
- Used for texture metadata and selected stage behavior

## Core material fields
- `map`
- `blendFunc`
- `alphaFunc`
- `rgbGen`
- `alphaGen`
- `tcGen`
- `tcMod`

## Particle-facing stage support
Supported in the current particle contract:
- `map <path>`
- `clampmap <path>`
- `map $whiteimage`
- `map $blackimage`
- `rgbGen identity|vertex|const|wave`
- `alphaGen identity|vertex|const|wave`
- `tcMod scroll|scale|rotate|turb|stretch`
- `blendFunc replace|alpha|add|mult|premult|custom`

Not supported for particle use:
- `map $lightmap`
- `tcGen environment`
- unsupported `tcMod` types

## Rules
- Unknown tokens are tolerated by the parser and reported when relevant.
- Strict particle mode may reject unsupported stages instead of skipping them.

## Example
```text
material textures/common/nodraw {
  surfaceparm nodraw
}
```
