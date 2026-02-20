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
 *
 * DLight interaction
 * ------------------
 * Two new cvars control how lightgrid ambient interacts with clustered dlights:
 *
 *  r_envlight_dlight_ambient_cap  [0..1, default 0.5]
 *    Maximum fraction of entity ambient that dlights may displace via the
 *    classic  ambient = max(LightColor - DLightColor, 0)  subtraction path.
 *    0 = fully displaceable (original behaviour).
 *    1 = ambient fully protected against dlight cancellation.
 *    Use R_EnvLight_SampleEntityAmbientDLight() to retrieve the floor value
 *    and clamp DLightColor before uploading to the shader.
 *
 *  r_envlight_dlight_fill_suppress  [0..1, default 0.6]
 *    Suppresses the world-fill baseline (lightgrid_params.w) proportionally
 *    to local dlight luminance.  Prevents double-ambient in well-lit clusters.
 *    0 = disabled.  Pass the per-frame dlight luma to
 *    R_EnvLight_BuildFrameUniformsDLight().
 */

/* ------------------------------------------------------------------ */
/* Cvars (extern declarations for callers that need direct access)     */
/* ------------------------------------------------------------------ */

extern cvar_t r_envlight;
extern cvar_t r_envlight_entity_intensity;
extern cvar_t r_envlight_world_fill;
extern cvar_t r_envlight_sky_sh;
extern cvar_t r_envlight_envmap;
extern cvar_t r_envlight_indoor_dampen;
extern cvar_t r_envlight_dlight_ambient_cap;
extern cvar_t r_envlight_dlight_fill_suppress;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/* Registers envlight master cvars and legacy alias callbacks. */
void R_EnvLight_RegisterCvars (void);

/*
 * Fills frame-uniform envlight/lightgrid vectors from centralized envlight
 * state.  Use this variant when no per-frame dlight luma is available.
 */
void R_EnvLight_BuildFrameUniforms (vec4_t lighting_params, vec4_t lightgrid_params);

/*
 * Extended variant that factors in the per-frame aggregate dlight luminance
 * (dlight_luma, in [0, dlight_overbright]).  Applies world-fill suppression
 * and packs the dlight_ambient_cap into lighting_params[3] for the shader.
 *
 * Shader decoding of lighting_params[3]:
 *   int   debug_level = int(lighting_params[3]);
 *   float dlight_cap  = fract(lighting_params[3]) * 10.0;
 */
void R_EnvLight_BuildFrameUniformsDLight (vec4_t lighting_params, vec4_t lightgrid_params,
                                          float dlight_luma, float dlight_overbright);

/*
 * Samples the lightgrid at the entity's position (with one height-offset retry
 * for floor-flush entities) and returns linear-space RGB ambient + AO.
 *
 * Returns true if the result contains non-zero ambient or AO data.
 * out_ao may be NULL.
 */
qboolean R_EnvLight_SampleEntityAmbient (const entity_t *e, vec3_t out_rgb_linear, float *out_ao);

/*
 * Extended variant that also returns out_dlight_floor – the minimum fraction
 * of entity ambient that must survive dlight subtraction.
 *
 * Usage pattern in alias renderer:
 *
 *   vec3_t  amb;
 *   float   ao, floor;
 *   R_EnvLight_SampleEntityAmbientDLight (e, amb, &ao, &floor);
 *
 *   // Cap dlight subtraction to preserve at least (floor) of ambient:
 *   for (int i = 0; i < 3; i++)
 *       dlight_color[i] = min(dlight_color[i], amb[i] * (1.f - floor));
 *
 *   // Upload amb and capped dlight_color to shader as usual.
 *
 * out_dlight_floor is 0 when envlight is disabled (legacy behaviour).
 */
qboolean R_EnvLight_SampleEntityAmbientDLight (const entity_t *e,
                                               vec3_t out_rgb_linear,
                                               float *out_ao,
                                               float *out_dlight_floor);
