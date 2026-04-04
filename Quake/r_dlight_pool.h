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

#endif // R_DLIGHT_POOL_H
