#ifndef R_FOGVOL_INTERNAL_H
#define R_FOGVOL_INTERNAL_H

#include "r_fogvol.h"

void R_FogVol_ClearEntities (void);
void R_FogVol_AddEntityVolume (const fog_volume_t *volume);
void R_FogVol_ResetStaticLights (void);
void R_FogVol_FreeStaticField (void);
void R_FogVol_CommitStaticBuildConfig (void);
void R_FogVol_BuildStaticLightInjection (void);
void R_FogVol_ContinueStaticFieldBuild (int sample_budget);
void R_FogVol_SummarizeStaticLightsFromField (void);
void R_FogVol_AccumulateStaticLightsAtPoint (const vec3_t p, vec3_t out_rgb);
qboolean R_FogVol_StaticFieldNeedsBuildContinuation (void);
int R_FogVol_GetStaticFieldTotalSamples (void);
int R_FogVol_GetStaticLightCount (void);
qboolean R_FogVol_GetStaticLight (int idx, vec4_t pos_rad, vec4_t col_int);
qboolean R_Froxel_GetShaderState (GLuint *out_light_tex, int *out_light_count, float params0[4], float params1[4]);
qboolean R_Froxel_GetDebugScales (float *out_dlight_scale, float *out_sun_scale, float *out_emissive_scale);

#endif /* R_FOGVOL_INTERNAL_H */
