#ifndef R_SHADOWS_H
#define R_SHADOWS_H

#include "quakedef.h"

#ifdef __cplusplus
extern "C" {
#endif

extern cvar_t r_shadows;

void R_InitShadowMaps(void);
void R_BeginShadowPass(void);
void R_EndShadowPass(void);
void R_DrawShadowDebug(void);
void R_UploadShadowMatrices(const float *matrices, int light_index);

#ifdef __cplusplus
}
#endif

#endif /* R_SHADOWS_H */
