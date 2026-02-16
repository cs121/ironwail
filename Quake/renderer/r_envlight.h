#pragma once

#include "quakedef.h"

/*
 * Envlight/entity ambient probes are expressed in linear RGB intensity.
 * Callers must keep entity lighting data (ambientcolor/dlightcolor/lightcolor)
 * in linear space and rely on the renderer's global postprocess / framebuffer
 * sRGB path for the final transfer conversion.
 */
qboolean R_EnvLight_SampleEntityAmbient (const entity_t *e, vec3_t out_rgb_linear, float *out_ao);
