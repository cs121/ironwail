#ifndef R_ENTITYLIGHT_H
#define R_ENTITYLIGHT_H

#include "quakedef.h"
#include "render.h"

float R_LightgridLuminance (const vec3_t color);
float R_ModelLightLuma (const vec3_t color);
void R_LightgridChroma (const vec3_t color, vec3_t chroma);
void R_ScaleAliasLighting (vec3_t light, vec3_t ambient, vec3_t dlight, float scale);
void R_DefaultStaticLightDir (vec3_t dir);
const char *R_StaticSourceName (entity_static_light_source_t source);
void R_ClampSampleColor (vec3_t color);
void R_ApplyLightgridLighting (const entity_t *e, vec3_t ambientcolor);
void R_EntityLightDebugReport (const entity_t *e, const entity_lightinfo_t *info);
qboolean R_ModelLightWouldExceedBudget (int requested_samples);
void R_ModelLightStats_AddCall (int sample_count, qboolean used_multisample, qboolean budget_fallback, double elapsed_ms);
qboolean R_LightPointNoGrid (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache, vec3_t out_color);
const lightgrid_probe_t *R_GetLightgridSample (const vec3_t pos);
qboolean R_EntityStaticLight (entity_t *e, vec3_t out_color255, entity_lightinfo_t *info);
void R_FinalizeAliasLighting (entity_t *e, vec3_t lightcolor, vec3_t ambientcolor, vec3_t dlightcolor, vec3_t dlightdir, vec3_t staticlightdir, entity_lightinfo_t *lightinfo_ptr);

#endif /* R_ENTITYLIGHT_H */
