#ifndef R_FOGVOL_H
#define R_FOGVOL_H

#define MAX_FOGVOLUMES 64
#define MAX_FOGLIGHTS 31 /* keep FogLightsUBO under 64KB std140 limit on NVIDIA */

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
/* BEST PRACTICE #11: Expose all cvars that may be referenced from other
 * translation units (e.g. menu, console completion, test harnesses). */
extern cvar_t r_fogvol_steps;
extern cvar_t r_fogvol_maxsteps;
extern cvar_t r_fogvol_stepsize;
/* BUG FIX (C-05): r_fogvol_steps_scale_halfres was defined in r_fogvol.c but
 * not exported — violates BEST PRACTICE #11. */
extern cvar_t r_fogvol_steps_scale_halfres;
extern cvar_t r_fogvol_noise;
extern cvar_t r_fogvol_noise_subsample;
extern cvar_t r_fogvol_noise_lod_switch_dist;
extern cvar_t r_fogvol_domainwarp_dist;
extern cvar_t r_fogvol_noise_detail_strength;
extern cvar_t r_fogvol_quality;
extern cvar_t r_fogvol_atmosphere;
extern cvar_t r_fogvol_noisemode;
extern cvar_t r_fogvol_physblend;
extern cvar_t r_fogvol_blendmode;
extern cvar_t r_fogvol_emissive;
extern cvar_t r_fogvol_temporal_alpha;
extern cvar_t r_fogvol_temporal_depth_reject;
extern cvar_t r_fogvol_temporal_confidence_min_alpha;
extern cvar_t r_fogvol_temporal_disocclusion_bias;
extern cvar_t r_fogvol_temporal_clamp_strength;
extern cvar_t r_fogvol_checkerboard;
extern cvar_t r_fogvol_light_subsample;
extern cvar_t r_fogvol_jitter;
extern cvar_t r_fogvol_debug;
extern cvar_t r_fogvol_inject_debug;
extern cvar_t r_fogvol_density_scale;
extern cvar_t r_fogvol_sigma_max;
extern cvar_t r_fogvol_testvolumes;
/* BUG FIX (C-06): r_fogvol_testvolumes_dumpstate was defined in r_fogvol.c
 * but not exported — violates BEST PRACTICE #11. */
extern cvar_t r_fogvol_testvolumes_dumpstate;
extern cvar_t r_fogvol_global;
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
extern cvar_t r_fogvol_global_shadow;
extern cvar_t r_fogvol_height_mist_strength;
extern cvar_t r_fogvol_global_priority;
extern cvar_t r_fogvol_stats;
extern cvar_t r_fogvol_light;
extern cvar_t r_fogvol_lighting_mode;
extern cvar_t r_fogvol_lightgrid;
extern cvar_t r_fogvol_light_max;
extern cvar_t r_fogvol_dlightscale;
extern cvar_t r_fogvol_gpu_light_select;
extern cvar_t r_fogvol_gpu_static_build;
extern cvar_t r_fogvol_shadow;
extern cvar_t r_fogvol_shadow_samples;
extern cvar_t r_fogvol_shadow_strength;
extern cvar_t r_fogvol_shadow_jitter;
extern cvar_t r_fogvol_sun_scatter;
extern cvar_t r_fogvol_froxel;
extern cvar_t r_fogvol_froxel_parity;
extern cvar_t r_fogvol_froxel_sun;
extern cvar_t r_fogvol_froxel_sun_legacy;
extern cvar_t r_fogvol_froxel_static;
extern cvar_t r_fogvol_static_probe_mode;
extern cvar_t r_fogvol_static_probe_density;
extern cvar_t r_fogvol_static_probe_budget_load;
extern cvar_t r_fogvol_static_probe_budget_frame;
extern cvar_t r_fogvol_static_probe_grid;
extern cvar_t r_fogvol_froxel_godrays;
extern cvar_t r_fogvol_froxel_temporal_alpha;
extern cvar_t r_fogvol_froxel_temporal_reject_threshold;
extern cvar_t r_fogvol_godray_coupling;
extern cvar_t r_fogvol_froxel_scale;
extern cvar_t r_fogvol_lightlist_scale;
extern cvar_t r_fogvol_lightgrid_scale;
extern cvar_t r_fogvol_lava_emissive;
extern cvar_t r_froxel_debug;

void R_FogVol_RegisterCvars (void);
void R_FogVol_Init (void);
void R_FogVol_Clear (void);
void R_FogVol_ParseEntities (void);
void R_FogVol_BuildList (void);
void R_FogVol_AddTestVolumes (void);
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
/* FogVol rendering-availability gate: volumetric resources/programs + r_fogvol are ready this frame. */
qboolean R_FogVol_IsEnabledForFrame (void);
/* Content gate: at least one fog source could contribute this frame
 * (global fog, entity fog volumes, or debug test volumes). */
qboolean R_FogVol_HasRenderableContent (void);
/* Output-level gate: fogvol produced a valid composite texture this frame (safe to sample now). */
qboolean R_FogVol_HasValidComposite (void);
/* PostFX gate: volumetric output may be produced this frame (not tied to global fog density). */
qboolean R_FogVol_ShouldAffectPostFX (void);
/* Global-replacement gate: volumetric global fog is eligible this frame (includes classic fog density + r_fogvol_global). */
qboolean R_FogVol_CanRenderGlobal (void);
/*
 * FogVol Composite Contract (for CPU + shader users):
 *  - RGB: fog radiance already composited over the scene color (display-space contribution).
 *  - A:   fog coverage proxy = 1 - transmittance (a.k.a. integrated fog density/opacity).
 *         0.0 = no volumetric medium on this pixel, 1.0 = fully opaque medium.
 *  - Validity: sample only when R_FogVol_HasValidComposite() is true for the frame.
 */
/* Returns the composite texture that received the fogvol temporal output this
 * frame. Use this for SSAO suppression — it is the correct per-pixel fog density
 * source for spatial/noisy volumetric fog that cannot be modelled analytically. */
GLuint R_FogVol_GetCompositeTex (void);

#endif // R_FOGVOL_H
