#pragma once

#include "quakedef.h"

/*
 * Envlight/entity ambient probes are expressed in linear RGB intensity.
 * Callers must keep entity lighting data (ambientcolor/lightcolor)
 * in linear space and rely on the renderer's global postprocess / framebuffer
 * sRGB path for the final transfer conversion.
 *
 * Expectations for color-space toggles/debug:
 *  - r_srgb_textures controls sampler decode, not entity-light payload encoding.
 *  - r_srgb_framebuffer / postprocess controls final linear->sRGB transfer only.
 *  - ColorSpaceParams debug views are expected to observe linear entity lighting
 *    without gamma jumps when envlight toggles on/off.
 */
qboolean R_EnvLight_SampleEntityAmbient (const entity_t *e, vec3_t out_rgb_linear, float *out_ao);

/* Registers envlight master cvars and legacy alias callbacks. */
void R_EnvLight_RegisterCvars (void);

/* Fills frame-uniform envlight/lightgrid vectors from centralized envlight state. */
void R_EnvLight_BuildFrameUniforms (vec4_t lighting_params, vec4_t lightgrid_params);
