/*
 * Transitional model type shim used by core-facing headers.
 * TODO_CORE_DECONTAMINATION: split truly neutral model types out of gl_model.h.
 *
 * Phase 2 split-point notes:
 * - This header is the intended future home for renderer/backend-neutral model
 *   declarations that core, client, server, physics, and tools can include
 *   without importing GL model internals.
 * - The include below deliberately remains in place during Phase 2. Moving
 *   fields or changing structure ownership belongs to a later, separately
 *   tested migration phase.
 *
 * TODO_MODEL_TYPES_NEUTRAL_BASE:
 * - Define backend-neutral model_t base data here once ownership is clear.
 * - Keep file identity, bounds, hull/clip data, visibility, and CPU-side
 *   metadata separate from render-backend uploads.
 *
 * TODO_MODEL_TYPES_KIND_SPLIT:
 * - Split mesh/brush/alias/sprite declarations into neutral CPU-facing
 *   concepts before any backend resource handles are attached.
 * - Preserve existing qmodel_t/model_t behavior until every call site has
 *   an audited owner.
 *
 * TODO_MODEL_TYPES_CPU_GPU_SEPARATION:
 * - Keep CPU-side model data in core/client-visible structs.
 * - Move GPU/GL allocation state into the GL backend or a renderer resource
 *   layer with explicit lifetime rules.
 *
 * TODO_MODEL_TYPES_NO_NATIVE_GL_FIELDS:
 * - Future neutral model types must not contain GLuint, GLenum, gltexture_t,
 *   or other API-native renderer fields.
 */

#ifndef CORE_MODEL_TYPES_H
#define CORE_MODEL_TYPES_H

#include "gl_model.h"

#endif /* CORE_MODEL_TYPES_H */
