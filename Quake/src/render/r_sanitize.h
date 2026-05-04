#ifndef R_SANITIZE_H
#define R_SANITIZE_H

qboolean R_IsFiniteFloat (float value);
float R_SanitizeFloatRange (float value, float fallback, float minval, float maxval);

#endif
