#ifndef R_FOGVOL_H
#define R_FOGVOL_H

#define MAX_FOGVOLUMES 64
#define MAX_FOGLIGHTS 256 /* SSBO-backed light lists; no std140 UBO 64KB ceiling */

/* Fog volume shape encoding used by CPU logic, entity parsing, and GPU UBOs.
 * 0 = axis-aligned box (mins/maxs)
 * 1 = sphere (sphereCenter/sphereRadius)
 * Values outside this set are unsupported and must be normalized before use. */
typedef enum fog_volume_shape_e
{
	FOGVOL_SHAPE_BOX = 0,
	FOGVOL_SHAPE_SPHERE = 1
} fog_volume_shape_t;

typedef struct fog_volume_s
{
	vec3_t mins;
	vec3_t maxs;
	vec3_t sphereCenter;
	float sphereRadius;
	vec3_t color;
	float density;
	float falloff;
	int mode;
	int shape; /* fog_volume_shape_t */
	int blendMode;
	float emissiveStrength;
	float noiseScale;
	float noiseAmount;
	float noiseBias;
	float turbulence;
	vec3_t velocity;
	float windSpeed;
	vec3_t windDir;
	float maxDistance;
	int priority;
	int enabled;
	float height;
	float heightScale;
	float edgeSoftness;
} fog_volume_t;

typedef struct froxel_grid_s
{
	int dims[3];
	vec3_t mins;
	vec3_t maxs;
	float *density;
	float *color;
	float *emissive;
} froxel_grid_t;

extern cvar_t r_fogvol;
extern cvar_t r_fogvol_halfres;
extern cvar_t r_fogvol_steps;
extern cvar_t r_fogvol_noise;
extern cvar_t r_fogvol_noisemode;
extern cvar_t r_fogvol_noise_scale;
extern cvar_t r_fogvol_noise_amount;
extern cvar_t r_fogvol_noise_bias;
extern cvar_t r_fogvol_jitter;
extern cvar_t r_fogvol_blendmode;
extern cvar_t r_fogvol_emissive;
extern cvar_t r_fogvol_debug;
extern cvar_t r_fogvol_debug_froxel_random;
extern cvar_t r_fogvol_density_scale;
extern cvar_t r_fogvol_global;
extern cvar_t r_fogvol_global_color;
extern cvar_t r_fogvol_global_density_scale;
extern cvar_t r_fogvol_global_falloff;
extern cvar_t r_fogvol_global_noise_scale;
extern cvar_t r_fogvol_global_noise_amount;
extern cvar_t r_fogvol_global_noise_bias;
extern cvar_t r_fogvol_global_velocity_x;
extern cvar_t r_fogvol_global_velocity_y;
extern cvar_t r_fogvol_global_velocity_z;
extern cvar_t r_fogvol_global_height;
extern cvar_t r_fogvol_global_height_scale;
extern cvar_t r_fogvol_height_mist_strength;
extern cvar_t r_fogvol_global_priority;
extern cvar_t r_fogvol_stats;
extern cvar_t r_fogvol_light;
extern cvar_t r_fogvol_dlightscale;
extern cvar_t r_fogvol_froxel_sun;
extern cvar_t r_fogvol_light_dlight_boost;
extern cvar_t r_fogvol_light_sun_boost;
extern cvar_t r_fogvol_light_emissive_boost;
extern cvar_t r_fogvol_light_ambient;
extern cvar_t r_fogvol_light_contrast;
extern cvar_t r_fogvol_shadow_contrast;
extern cvar_t r_fogvol_light_extinction_relief;
extern cvar_t r_fogvol_shadow;
extern cvar_t r_fogvol_shadow_strength;
extern cvar_t r_fogvol_lava_emissive;

void R_FogVol_RegisterCvars (void);
void R_FogVol_Init (void);
void R_FogVol_Clear (void);
void R_FogVol_ParseEntities (void);
void R_FogVol_BuildList (void);
void R_FogVol_Render (void);
void R_FogVol_DrawDebug2D (void);
void R_FogVol_ClearHistory (void);
void R_FogVol_LogEndFrameState (void);
void R_Froxel_BeginFrame (float near_clip, float far_clip);
void R_Froxel_InjectDlights (void);
void R_Froxel_EndFrame (void);
void R_Froxel_ResetResources (void);
void R_FogVol_SetGodrayCouplingTextures (GLuint shafts_tex, GLuint mask_tex, GLuint source_tex, qboolean ready);
void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num);
qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres);
qboolean R_FogVol_IsEnabledForFrame (void);
qboolean R_FogVol_HasRenderableContent (void);
qboolean R_FogVol_HasValidComposite (void);
qboolean R_FogVol_ShouldAffectPostFX (void);
qboolean R_FogVol_CanRenderGlobal (void);
qboolean R_FogVol_GetGlobalFogState (vec3_t color, float *density);
GLuint R_FogVol_GetCompositeTex (void);

#endif /* R_FOGVOL_H */
