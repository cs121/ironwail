#ifndef R_SSAO_H
#define R_SSAO_H

#include "quakedef.h"

typedef struct r_ssao_fog_state_s
{
	float color[3];
	float density;
	qboolean fogvol_valid;
	int transmittance_policy;
} r_ssao_fog_state_t;

typedef enum r_ssao_fog_transmittance_policy_e
{
	R_SSAO_FOG_TRANS_GLOBAL_ONLY = 0,
	R_SSAO_FOG_TRANS_FOGVOL_OR_GLOBAL = 1
} r_ssao_fog_transmittance_policy_t;

float R_SSAO_SanitizeValue (float value, float fallback, float minval, float maxval);
void R_SSAO_CaptureFogState (const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state);
void R_SSAO_RegisterCvars (void);

#endif
