#include "r_ssao.h"

#include <math.h>

float R_SSAO_SanitizeValue (float value, float fallback, float minval, float maxval)
{
#if defined(_MSC_VER)
	if (_finite (value) == 0)
		return fallback;
#else
	if (!isfinite (value))
		return fallback;
#endif

	if (value < minval)
		return minval;
	if (value > maxval)
		return maxval;
	return value;
}

void R_SSAO_CaptureFogState (const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state)
{
	if (!out_state)
		return;

	if (!framedata)
	{
		VectorClear (out_state->color);
		out_state->density = 0.f;
		return;
	}

	out_state->color[0] = framedata->fogdata[0];
	out_state->color[1] = framedata->fogdata[1];
	out_state->color[2] = framedata->fogdata[2];
	out_state->density = framedata->fogdata[3];
}
