#include "quakedef.h"
#include "glquake.h"
#include "render_dispatch.h"

const iw_renderer_entry_points_t *g_rend = NULL;

static iw_renderer_entry_points_t s_entries;

static void RenderDispatch_FillInternalEntryPoints (iw_renderer_entry_points_t *entries)
{
	if (!entries)
		return;

	memset (entries, 0, sizeof (*entries));
	entries->struct_size = sizeof (*entries);
	entries->R_Init = R_Init;
	entries->R_RenderView = R_RenderView;
	entries->R_NewMap = R_NewMap;
	entries->R_ClearEfrags = R_ClearEfrags;
	entries->R_CheckEfrags = R_CheckEfrags;
	entries->R_AddEfrags = R_AddEfrags;
	entries->R_ParseParticleEffect = R_ParseParticleEffect;
	entries->R_RunParticleEffect = R_RunParticleEffect;
	entries->R_RocketTrail = R_RocketTrail;
	entries->R_EntityParticles = R_EntityParticles;
	entries->R_BlobExplosion = R_BlobExplosion;
	entries->R_ParticleExplosion = R_ParticleExplosion;
	entries->R_ParticleExplosion2 = R_ParticleExplosion2;
	entries->R_LavaSplash = R_LavaSplash;
	entries->R_TeleportSplash = R_TeleportSplash;
	entries->R_SpawnImpactDecal = R_SpawnImpactDecal;
	entries->R_SpawnImpactDecalEx = R_SpawnImpactDecalEx;
	entries->R_TranslatePlayerSkin = R_TranslatePlayerSkin;
	entries->R_TranslateNewPlayerSkin = R_TranslateNewPlayerSkin;
	entries->R_ClearBoundingBoxes = R_ClearBoundingBoxes;
	entries->R_ClearParticles = R_ClearParticles;
	entries->R_ClearDecals = R_ClearDecals;
	entries->R_ReloadDecals = R_ReloadDecals;
	entries->R_InitDecals = R_InitDecals;
	entries->R_StorePrevFrameState = R_StorePrevFrameState;
	entries->R_GetParticleDebugStats = R_GetParticleDebugStats;
	entries->R_SetAlphaMode = (void (*)(int))R_SetAlphaMode;
	entries->R_GetAlphaMode = (int (*)(void))R_GetAlphaMode;
	entries->R_GetEffectiveAlphaMode = (int (*)(void))R_GetEffectiveAlphaMode;
	entries->R_AddStaticModels = R_AddStaticModels;
	entries->R_PushDlights = R_PushDlights;
	entries->R_ParseDlightEntities = R_ParseDlightEntities;
	entries->R_GetLightgridSample = (const void *(*)(const vec3_t))R_GetLightgridSample;
}

void RenderDispatch_Init (void)
{
	RenderDispatch_FillInternalEntryPoints (&s_entries);
	g_rend = &s_entries;
}

void RenderDispatch_SetEntryPoints (const iw_renderer_entry_points_t *entry_points)
{
#define IW_RENDERER_ENTRY_HAS_FIELD(ep, field_name) \
	((ep) != NULL && (ep)->struct_size >= (unsigned int)(offsetof(iw_renderer_entry_points_t, field_name) + sizeof ((ep)->field_name)))
#define IW_RENDERER_ENTRY_COPY_FN(field_name) \
	do \
	{ \
		if (IW_RENDERER_ENTRY_HAS_FIELD (entry_points, field_name) && entry_points->field_name) \
			s_entries.field_name = entry_points->field_name; \
	} while (0)

	RenderDispatch_FillInternalEntryPoints (&s_entries);
	if (!entry_points)
	{
		g_rend = &s_entries;
		return;
	}

	IW_RENDERER_ENTRY_COPY_FN (R_Init);
	IW_RENDERER_ENTRY_COPY_FN (R_RenderView);
	IW_RENDERER_ENTRY_COPY_FN (R_NewMap);
	IW_RENDERER_ENTRY_COPY_FN (R_ClearEfrags);
	IW_RENDERER_ENTRY_COPY_FN (R_CheckEfrags);
	IW_RENDERER_ENTRY_COPY_FN (R_AddEfrags);
	IW_RENDERER_ENTRY_COPY_FN (R_ParseParticleEffect);
	IW_RENDERER_ENTRY_COPY_FN (R_RunParticleEffect);
	IW_RENDERER_ENTRY_COPY_FN (R_RocketTrail);
	IW_RENDERER_ENTRY_COPY_FN (R_EntityParticles);
	IW_RENDERER_ENTRY_COPY_FN (R_BlobExplosion);
	IW_RENDERER_ENTRY_COPY_FN (R_ParticleExplosion);
	IW_RENDERER_ENTRY_COPY_FN (R_ParticleExplosion2);
	IW_RENDERER_ENTRY_COPY_FN (R_LavaSplash);
	IW_RENDERER_ENTRY_COPY_FN (R_TeleportSplash);
	IW_RENDERER_ENTRY_COPY_FN (R_SpawnImpactDecal);
	IW_RENDERER_ENTRY_COPY_FN (R_SpawnImpactDecalEx);
	IW_RENDERER_ENTRY_COPY_FN (R_TranslatePlayerSkin);
	IW_RENDERER_ENTRY_COPY_FN (R_TranslateNewPlayerSkin);
	IW_RENDERER_ENTRY_COPY_FN (R_ClearBoundingBoxes);
	IW_RENDERER_ENTRY_COPY_FN (R_ClearParticles);
	IW_RENDERER_ENTRY_COPY_FN (R_ClearDecals);
	IW_RENDERER_ENTRY_COPY_FN (R_ReloadDecals);
	IW_RENDERER_ENTRY_COPY_FN (R_InitDecals);
	IW_RENDERER_ENTRY_COPY_FN (R_StorePrevFrameState);
	IW_RENDERER_ENTRY_COPY_FN (R_GetParticleDebugStats);
	IW_RENDERER_ENTRY_COPY_FN (R_SetAlphaMode);
	IW_RENDERER_ENTRY_COPY_FN (R_GetAlphaMode);
	IW_RENDERER_ENTRY_COPY_FN (R_GetEffectiveAlphaMode);
	IW_RENDERER_ENTRY_COPY_FN (R_AddStaticModels);
	IW_RENDERER_ENTRY_COPY_FN (R_PushDlights);
	IW_RENDERER_ENTRY_COPY_FN (R_ParseDlightEntities);
	IW_RENDERER_ENTRY_COPY_FN (R_GetLightgridSample);
	g_rend = &s_entries;

#undef IW_RENDERER_ENTRY_COPY_FN
#undef IW_RENDERER_ENTRY_HAS_FIELD
}
