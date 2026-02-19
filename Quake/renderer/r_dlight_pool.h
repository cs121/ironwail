#ifndef R_DLIGHT_POOL_H
#define R_DLIGHT_POOL_H

typedef struct dlight_pool_stats_s
{
	int active;
	int persistent;
	int transient;
	int submitted;
	int expired;
	int evicted;
} dlight_pool_stats_t;

typedef enum dlight_reject_reason_e
{
	DLIGHT_REJECT_NONE = 0,
	DLIGHT_REJECT_INACTIVE,
	DLIGHT_REJECT_LIFETIME,
	DLIGHT_REJECT_RADIUS,
	DLIGHT_REJECT_WORLD_FLAG,
	DLIGHT_REJECT_PVS,
	DLIGHT_REJECT_FRUSTUM,
	DLIGHT_REJECT_BUDGET,
	DLIGHT_REJECT_OTHER
} dlight_reject_reason_t;

typedef struct dlight_filter_debug_s
{
	int created_count;
	int after_merge_count;
	int total_active;
	int pass_lifetime_radius;
	int pass_world_flag;
	int pass_pvs;
	int pass_frustum;
	int pass_budget;
	int final_active_count;
	float min_radius;
	float max_radius;
	int rejected_lifetime_radius;
	int rejected_world_flag;
	int rejected_pvs;
	int rejected_frustum;
	int rejected_budget;
} dlight_filter_debug_t;

typedef struct dlight_debug_entry_s
{
	qboolean captured;
	int id;
	vec3_t origin;
	vec3_t color;
	float radius;
	float baseradius;
	float die;
	int flags;
	vec3_t mins;
	vec3_t maxs;
	qboolean has_leaf;
	int leaf_index;
	qboolean in_pvs;
	qboolean in_frustum;
	dlight_reject_reason_t reason;
} dlight_debug_entry_t;

void DLightPool_Init (void);
void DLightPool_Shutdown (void);
void DLightPool_Clear (void);
void DLightPool_ClearPersistent (void);
void DLightPool_NewFrame (double time, int framecount);
void DLightPool_Decay (float frametime, double time);
int DLightPool_GetBudget (void);
dlight_t *DLightPool_AllocTransient (double time);
dlight_t *DLightPool_AllocTransientByKey (int key, double time);
dlight_t *DLightPool_GetOrCreatePersistent (int key, double time);
void DLightPool_KillByKey (int key);
int DLightPool_CollectForRender (double time, const vec3_t vieworg, const mleaf_t *viewleaf,
		dlight_t **out, int out_max);
const dlight_t *const *DLightPool_GetActiveList (int *count);
void DLightPool_GetStats (dlight_pool_stats_t *out);
void DLightPool_DebugPrint (void);

void DLightPool_GetFilterDebug (dlight_filter_debug_t *out_stats, dlight_debug_entry_t out_entries[4]);
const char *DLightPool_RejectReasonName (dlight_reject_reason_t reason);

#endif // R_DLIGHT_POOL_H
