#ifndef R_FOGVOL_H
#define R_FOGVOL_H

#define MAX_FOGVOLUMES 64

typedef struct fog_volume_s
{
	vec3_t mins;
	vec3_t maxs;
	vec3_t color;
	float density;
	float falloff;
	int mode;
	float noiseScale;
	float noiseAmount;
	float noiseBias;
	vec3_t velocity;
	float maxDistance;
	int priority;
	int enabled;
	float height;
	float heightScale;
} fog_volume_t;

typedef struct froxel_grid_s froxel_grid_t;

extern cvar_t r_fogvol;
extern cvar_t r_fogvol_halfres;
/* BEST PRACTICE #11: Expose all cvars that may be referenced from other
 * translation units (e.g. menu, console completion, test harnesses). */
extern cvar_t r_fogvol_steps;
extern cvar_t r_fogvol_upsample;
extern cvar_t r_fogvol_upsample_k;
extern cvar_t r_fogvol_upsample_taps;
extern cvar_t r_fogvol_noise;
extern cvar_t r_fogvol_noisemode;
extern cvar_t r_fogvol_physblend;
extern cvar_t r_fogvol_temporal_alpha;
extern cvar_t r_fogvol_temporal_depth_reject;
extern cvar_t r_fogvol_jitter;
extern cvar_t r_fogvol_debug;
extern cvar_t r_fogvol_density_scale;
extern cvar_t r_fogvol_sigma_max;
extern cvar_t r_fogvol_testvolumes;
extern cvar_t r_fogvol_globalfog;
extern cvar_t r_fogvol_globalfog_density_scale;
extern cvar_t r_fogvol_globalfog_falloff;
extern cvar_t r_fogvol_globalfog_noise_scale;
extern cvar_t r_fogvol_globalfog_noise_amount;
extern cvar_t r_fogvol_globalfog_noise_bias;
extern cvar_t r_fogvol_globalfog_velocity_x;
extern cvar_t r_fogvol_globalfog_velocity_y;
extern cvar_t r_fogvol_globalfog_velocity_z;
extern cvar_t r_fogvol_globalfog_height;
extern cvar_t r_fogvol_globalfog_height_scale;
extern cvar_t r_fogvol_globalfog_priority;

void R_FogVol_Init (void);
void R_FogVol_Clear (void);
void R_FogVol_ParseEntities (void);
void R_FogVol_BuildList (void);
void R_FogVol_AddTestVolumes (void);
void R_FogVol_Render (void);
void R_FogVol_DrawDebug2D (void);
void R_FogVol_ClearHistory (void);
void R_FogVol_LogEndFrameState (void);
void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num);
qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres);
qboolean R_FogVol_CanRenderGlobal (void);

#endif // R_FOGVOL_H
