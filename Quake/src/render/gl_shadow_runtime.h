#ifndef GL_SHADOW_RUNTIME_H
#define GL_SHADOW_RUNTIME_H

#include "quakedef.h"

#define SHADOW_DLIGHT_FACES 6
#define SHADOW_SUN_CASCADE_MAX 4

typedef struct shadow_runtime_s
{
	qboolean	valid;
	int			sun_cascade_count;
	float		sun_split_dist[SHADOW_SUN_CASCADE_MAX];
	float		sun_viewproj[SHADOW_SUN_CASCADE_MAX][16];
	vec3_t		sun_eye[SHADOW_SUN_CASCADE_MAX];
	mplane_t	sun_frustum[SHADOW_SUN_CASCADE_MAX][4];
	int			num_dlights;
	int			dlight_indices[SHADOW_DLIGHT_MAX];
	dlight_t	*dlight_sources[SHADOW_DLIGHT_MAX];
	vec4_t		dlight_pos_radius[SHADOW_DLIGHT_MAX];
	float		dlight_viewproj[SHADOW_DLIGHT_MAX][SHADOW_DLIGHT_FACES][16];
	mplane_t	dlight_frustum[SHADOW_DLIGHT_MAX][SHADOW_DLIGHT_FACES][4];
	float		caster_viewproj[16];
	vec4_t		caster_lightpos_far_mode;
	int			caster_mode;
} shadow_runtime_t;

qboolean R_Shadow_Enabled (void);
qboolean R_Shadow_SunEnabled (void);
qboolean R_Shadow_DlightEnabled (void);

void R_Shadow_NormalizeSettings (void);
void R_Shadow_ResetRuntime (shadow_runtime_t *state);
void R_Shadow_SelectDlights (shadow_runtime_t *state);
void R_Shadow_UpdateSunMatrix (shadow_runtime_t *state);
void R_Shadow_UpdateDlightMatrices (shadow_runtime_t *state);
void R_Shadow_SetCasterState (shadow_runtime_t *state, const float viewproj[16], qboolean dlight, const vec3_t lightpos, float far_plane);
void R_Shadow_ClearProgramUniformCaches (void);

void R_Shadow_CreateFrameBuffers (void);
void R_Shadow_DeleteFrameBuffers (void);
void R_Shadow_ReconcileResources (void);
void R_Shadow_RenderMaps (entity_t **shadow_visedicts, int numshadowedicts);

#endif
