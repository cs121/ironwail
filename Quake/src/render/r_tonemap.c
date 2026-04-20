#include "quakedef.h"
#include "glquake.h"
#include "r_tonemap.h"

#include <math.h>

float R_Tonemap_TemperedOverbright (float overbright)
{
	const float exponent = 0.75f;
	float tempered;

	if (overbright <= 1.f)
		return 1.f;

	tempered = powf (overbright, exponent);
	return q_max (tempered, 1.f);
}
