#ifndef R_REALTIMELIGHT_H
#define R_REALTIMELIGHT_H

/*
 * Shared realtime light collection for forward surface + volumetric consumers.
 *
 * Frame flow:
 * 1) Collect runtime lights on CPU from existing dlight/emissive sources.
 * 2) Cache one shared frame light list.
 * 3) Fan the same list out to each consumer:
 *    - forward world dlight pass
 *    - forward alias/model pass
 *    - froxel fog + GI injection
 *
 * This is not a fullscreen postprocess pass; it is shared scene-light plumbing.
 */

typedef enum rl_light_type_e
{
	RL_LIGHT_POINT = 0,
	RL_LIGHT_EMISSIVE_PROXY = 1,
	RL_LIGHT_GI_PROXY = 2
} rl_light_type_t;

typedef enum rl_light_flags_e
{
	RL_LIGHT_SURFACE_CONTRIB = 1u << 0,
	RL_LIGHT_VOLUMETRIC_CONTRIB = 1u << 1
} rl_light_flags_t;

typedef struct rl_light_s
{
	vec3_t origin;
	float radius;
	vec3_t color;
	float intensity;
	unsigned int type;  /* rl_light_type_t */
	unsigned int flags; /* rl_light_flags_t */
	unsigned int source_id;
	unsigned int reserved;
} rl_light_t;

typedef struct rl_light_collect_stats_s
{
	int source_dlights;
	int source_emissive;
	int accepted;
	int accepted_dlights;
	int accepted_emissive;
	int rejected_inactive;
	int rejected_not_live;
	int rejected_persistent_disabled;
	int rejected_zero_radius;
	int rejected_frustum;
	int rejected_budget;
	int rejected_emissive_budget;
	int rejected_priority;
} rl_light_collect_stats_t;

typedef enum rl_light_consumer_e
{
	RL_CONSUMER_WORLD = 0,
	RL_CONSUMER_MODEL,
	RL_CONSUMER_FOG,
	RL_CONSUMER_COUNT
} rl_light_consumer_t;

typedef enum rl_consumer_reject_reason_e
{
	RL_REJECT_NON_CONTRIB = 0,
	RL_REJECT_DISTANCE,
	RL_REJECT_LOCAL_BUDGET,
	RL_REJECT_HW_BUDGET,
	RL_REJECT_COUNT
} rl_consumer_reject_reason_t;

typedef struct rl_consumer_stats_s
{
	int considered;
	int accepted;
	int rejected[RL_REJECT_COUNT];
	float accepted_energy;
} rl_consumer_stats_t;

extern cvar_t r_ppdlights;
extern cvar_t r_ppdlights_world;
extern cvar_t r_ppdlights_world_scale;
extern cvar_t r_ppdlights_world_luma_clamp;
extern cvar_t r_ppdlights_world_soft_knee;
extern cvar_t r_ppdlights_world_tiles;
extern cvar_t r_ppdlights_world_tiles_debug;
extern cvar_t r_experimental_ppdlights_world_blendop;
extern cvar_t r_ppdlights_models;
extern cvar_t r_ppdlights_fog;
extern cvar_t r_ppdlights_fog_debug;
extern cvar_t r_ppdlights_fog_budget;
extern cvar_t r_ppdlights_gi;
extern cvar_t r_ppdlights_gi_debug;
extern cvar_t r_ppdlights_gi_budget;
extern cvar_t r_ppdlights_debug;
extern cvar_t r_ppdlights_debug_mode;
extern cvar_t r_ppdlights_emissive;
extern cvar_t r_ppdlights_emissive_debug;

void R_PPdlights_RegisterCvars (void);
void R_PPdlights_CollectFrame (void);
const rl_light_t *R_PPdlights_GetFrameLights (int *out_count);
void R_PPdlights_GetFrameStats (rl_light_collect_stats_t *out_stats);
qboolean R_PPdlights_WorldPathEnabled (void);
int R_PPdlights_BuildWorldGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights);
qboolean R_PPdlights_ModelPathEnabled (void);
int R_PPdlights_BuildModelGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights);
void R_PPdlights_RecordConsumerConsidered (rl_light_consumer_t consumer, unsigned int source_id);
void R_PPdlights_RecordConsumerAccept (rl_light_consumer_t consumer, unsigned int source_id, float energy);
void R_PPdlights_RecordConsumerReject (rl_light_consumer_t consumer, unsigned int source_id, rl_consumer_reject_reason_t reason);
qboolean R_PPdlights_GetConsumerStats (rl_light_consumer_t consumer, rl_consumer_stats_t *out_stats);

#endif /* R_REALTIMELIGHT_H */
