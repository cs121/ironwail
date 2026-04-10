#include "quakedef.h"
#include "glquake.h"

#include "r_sanitize.h"

#include <math.h>

qboolean R_IsFiniteFloat (float value)
{
#if defined(_MSC_VER)
	return (_finite (value) != 0);
#else
	return isfinite (value);
#endif
}

float R_SanitizeFloatRange (float value, float fallback, float minval, float maxval)
{
	if (!R_IsFiniteFloat (value))
		return fallback;
	if (value < minval)
		return minval;
	if (value > maxval)
		return maxval;
	return value;
}
