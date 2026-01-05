#ifndef R_FOGVOL_H
#define R_FOGVOL_H

#define MAX_FOGVOLUMES 64

typedef struct fog_volume_s
{
	vec3_t mins;
	vec3_t maxs;
	vec3_t color;
	float density;
	float noiseScale;
	float noiseAmount;
	float noiseBias;
	vec3_t velocity;
	float maxDistance;
	int priority;
	int enabled;
} fog_volume_t;

extern cvar_t r_fogvol;

void R_FogVol_Init (void);
void R_FogVol_Clear (void);
void R_FogVol_BuildList (void);
void R_FogVol_AddTestVolumes (void);
void R_FogVol_Render (void);
void R_FogVol_DrawDebug2D (void);
qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres);

#endif // R_FOGVOL_H
