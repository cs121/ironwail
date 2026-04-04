#ifndef GL_SHADOW_H
#define GL_SHADOW_H

#include "quakedef.h"

int R_Shadow_ClampMapSize (float value);
void R_Shadow_MatrixIdentity (float m[16]);
void R_Shadow_MatrixMultiply (float out[16], const float a[16], const float b[16]);
void R_Shadow_MatrixPerspective (float m[16], float fovy_deg, float aspect, float znear, float zfar);
void R_Shadow_MatrixOrtho (float m[16], float left, float right, float bottom, float top, float znear, float zfar);
void R_Shadow_MatrixLook (float m[16], const vec3_t eye, const vec3_t dir, const vec3_t up_hint);
void R_Shadow_ExtractFrustumPlane (const float mvp[16], int axis, float ndcval, qboolean flip, mplane_t *out);
void R_Shadow_ExtractFrustum (const float viewproj[16], mplane_t out[4]);

#endif
