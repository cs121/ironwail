/*
Copyright (C) 2026 Ironwail contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

#ifndef GL_MODEL_GLB_H
#define GL_MODEL_GLB_H

/*
 * GLB/glTF models are decoded into IQM-style alias surfaces. Each glTF
 * primitive becomes one surface so material ranges render independently.
 * World/brush replacements intentionally remain render-only model assets here:
 * BSP visibility, collision, submodels, and lightmaps continue to come from
 * the owning BSP until a native GLB world backend exists.
 */
void Mod_LoadGLBModel (qmodel_t *mod, void *buffer);
void R_DrawGLBModels (entity_t **ents, int count);
void R_DrawGLBModels_ShowTris (entity_t **ents, int count);

#endif
