#define RENDERER_PLUGIN_BUILD 1

#include "quakedef.h"
#include "renderer_plugin.h"
#include "ref_gl_bridge.h"

static const iw_renderer_host_bridge_t *s_bridge = NULL;

static iw_renderer_entry_points_t s_entry_points;

extern void R_Init (void);
extern void R_RenderView (void);
extern void R_NewMap (void);
extern void R_ClearEfrags (void);
extern void R_CheckEfrags (void);
extern void R_AddEfrags (entity_t *ent);
extern void R_AddStaticModels (const byte *vis);
extern void R_PushDlights (void);
extern void R_ParseDlightEntities (void);
extern void R_ParseParticleEffect (void);
extern void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count);
extern void R_RocketTrail (vec3_t start, vec3_t end, int type);
extern void R_EntityParticles (entity_t *ent);
extern void R_BlobExplosion (vec3_t org);
extern void R_ParticleExplosion (vec3_t org);
extern void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength);
extern void R_LavaSplash (vec3_t org);
extern void R_TeleportSplash (vec3_t org);
extern const lightgrid_probe_t *R_GetLightgridSample (const vec3_t pos);
extern void R_SpawnImpactDecal (const char *category, const vec3_t origin, const vec3_t normal);
extern void R_SpawnImpactDecalEx (const char *category, const vec3_t origin, const vec3_t normal, const vec3_t hit_dir, qboolean heavy_blood);
extern void R_TranslatePlayerSkin (int playernum);
extern void R_TranslateNewPlayerSkin (int playernum);
extern void R_ClearBoundingBoxes (void);
extern void R_ClearParticles (void);
extern void R_ClearDecals (void);
extern void R_ReloadDecals (void);
extern void R_InitDecals (void);
extern void R_StorePrevFrameState (void);
extern void R_GetParticleDebugStats (particle_debug_stats_t *stats);
extern void R_SetAlphaMode (alphamode_t mode);
extern alphamode_t R_GetAlphaMode (void);
extern alphamode_t R_GetEffectiveAlphaMode (void);

extern const IRenderBackend *GL_Backend_GetInterface (void);

static void REFGL_FillEntryPoints (void)
{
	s_entry_points.struct_size = sizeof (iw_renderer_entry_points_t);
	s_entry_points.R_Init = R_Init;
	s_entry_points.R_RenderView = R_RenderView;
	s_entry_points.R_NewMap = R_NewMap;
	s_entry_points.R_ClearEfrags = R_ClearEfrags;
	s_entry_points.R_CheckEfrags = R_CheckEfrags;
	s_entry_points.R_AddEfrags = R_AddEfrags;
	s_entry_points.R_ParseParticleEffect = R_ParseParticleEffect;
	s_entry_points.R_RunParticleEffect = R_RunParticleEffect;
	s_entry_points.R_RocketTrail = R_RocketTrail;
	s_entry_points.R_EntityParticles = R_EntityParticles;
	s_entry_points.R_BlobExplosion = R_BlobExplosion;
	s_entry_points.R_ParticleExplosion = R_ParticleExplosion;
	s_entry_points.R_ParticleExplosion2 = R_ParticleExplosion2;
	s_entry_points.R_LavaSplash = R_LavaSplash;
	s_entry_points.R_TeleportSplash = R_TeleportSplash;
	s_entry_points.R_SpawnImpactDecal = R_SpawnImpactDecal;
	s_entry_points.R_SpawnImpactDecalEx = R_SpawnImpactDecalEx;
	s_entry_points.R_TranslatePlayerSkin = R_TranslatePlayerSkin;
	s_entry_points.R_TranslateNewPlayerSkin = R_TranslateNewPlayerSkin;
	s_entry_points.R_ClearBoundingBoxes = R_ClearBoundingBoxes;
	s_entry_points.R_ClearParticles = R_ClearParticles;
	s_entry_points.R_ClearDecals = R_ClearDecals;
	s_entry_points.R_ReloadDecals = R_ReloadDecals;
	s_entry_points.R_InitDecals = R_InitDecals;
	s_entry_points.R_StorePrevFrameState = R_StorePrevFrameState;
	s_entry_points.R_GetParticleDebugStats = R_GetParticleDebugStats;
	s_entry_points.R_SetAlphaMode = (void (*)(int))R_SetAlphaMode;
	s_entry_points.R_GetAlphaMode = (int (*)(void))R_GetAlphaMode;
	s_entry_points.R_GetEffectiveAlphaMode = (int (*)(void))R_GetEffectiveAlphaMode;
	s_entry_points.R_AddStaticModels = R_AddStaticModels;
	s_entry_points.R_PushDlights = R_PushDlights;
	s_entry_points.R_ParseDlightEntities = R_ParseDlightEntities;
	s_entry_points.R_GetLightgridSample = (const void *(*)(const vec3_t))R_GetLightgridSample;
}

static qboolean IW_RendererRefGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V4_SIZE)
		return false;

	if (IW_RENDERER_PLUGIN_HOST_HAS_FIELD (host_api, bridge) && host_api->bridge)
	{
		Bridge_Init (host_api->bridge);
		s_bridge = host_api->bridge;
	}

	if (!host_api->register_backend)
		return false;

	const IRenderBackend *gl_backend = GL_Backend_GetInterface ();
	if (!gl_backend)
		return false;

	if (!host_api->register_backend (gl_backend))
		return false;

	REFGL_FillEntryPoints ();

	if (IW_RENDERER_PLUGIN_HOST_HAS_FIELD (host_api, register_entry_points) && host_api->register_entry_points)
	{
		if (!host_api->register_entry_points (&s_entry_points))
			return false;
	}

	return true;
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"ref_gl",
		IW_RendererRefGL_Register
	};

	return &descriptor;
}
