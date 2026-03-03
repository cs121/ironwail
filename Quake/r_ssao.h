#ifndef R_SSAO_H
#define R_SSAO_H

#include "quakedef.h"

typedef struct r_ssao_fog_state_s
{
	float color[3];
	float density;
} r_ssao_fog_state_t;

float R_SSAO_SanitizeValue (float value, float fallback, float minval, float maxval);
void R_SSAO_CaptureFogState (const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state);

#endif
