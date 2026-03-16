#include "quakedef.h"
#include "r_fogvol_internal.h"

/*
 * Unified froxel/raymarch path removed static probe baking.
 * Keep these entry points as no-ops so entity parsing and runtime
 * call sites remain link-stable while the legacy system is retired.
 */
void R_FogVol_ResetStaticLights (void)
{
}

void R_FogVol_FreeStaticField (void)
{
}

void R_FogVol_BuildStaticLightInjection (void)
{
}

void R_FogVol_ContinueStaticFieldBuild (int sample_budget)
{
	(void)sample_budget;
}

void R_FogVol_SummarizeStaticLightsFromField (void)
{
}

void R_FogVol_AccumulateStaticLightsAtPoint (const vec3_t p, vec3_t out_rgb)
{
	(void)p;
	if (out_rgb)
		VectorClear (out_rgb);
}

qboolean R_FogVol_StaticFieldNeedsBuildContinuation (void)
{
	return false;
}

int R_FogVol_GetStaticFieldTotalSamples (void)
{
	return 0;
}

int R_FogVol_GetStaticLightCount (void)
{
	return 0;
}

qboolean R_FogVol_GetStaticLight (int idx, vec4_t pos_rad, vec4_t col_int)
{
	(void)idx;
	if (pos_rad)
	{
		pos_rad[0] = 0.f;
		pos_rad[1] = 0.f;
		pos_rad[2] = 0.f;
		pos_rad[3] = 0.f;
	}
	if (col_int)
	{
		col_int[0] = 0.f;
		col_int[1] = 0.f;
		col_int[2] = 0.f;
		col_int[3] = 0.f;
	}
	return false;
}
