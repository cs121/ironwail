#define RENDERER_PLUGIN_BUILD 1

#include "quakedef.h"
#include "renderer_plugin.h"
#include "ref_gl_bridge.h"
#include "gl_backend.h"

static const iw_renderer_host_bridge_t *s_bridge = NULL;

static iw_renderer_entry_points_t s_entry_points;
static void REFGL_RenderView_Entry (void);

/* LEGACY_COMPAT_ENTRYPOINT:
 * This file currently wires a broad legacy entrypoint set into the plugin.
 * Phase-2 objective is visibility and robustness, not large boundary rewrites. */

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
extern void GL_CreateFrameBuffers (void);
extern void GL_DeleteFrameBuffers (void);
extern void R_ResetDRSState (void);
extern void R_ResetGodraysStabilization (void);
extern void SCR_UpdateScreen (void);
extern void CL_RunParticles (void);
extern qpic_t *Draw_PicFromWad2 (const char *name, unsigned int texflags);
extern qpic_t *Draw_PicFromWad (const char *name);
extern qpic_t *Draw_CachePic (const char *path);
extern qpic_t *Draw_TryCachePic (const char *path, unsigned int texflags);
extern void Draw_NewGame (void);
extern void Draw_FillEx (float x, float y, float w, float h, const float *rgb, float alpha);
extern void Draw_PartialFadeScreen (float x0, float x1, float y0, float y1, float alpha);
extern void Draw_Character (int x, int y, int num);
extern void Draw_CharacterEx (float x, float y, float dimx, float dimy, int num);
extern void Draw_String (int x, int y, const char *str);
extern void Draw_StringEx (float x, float y, float dim, const char *str);
extern void Draw_Pic (int x, int y, qpic_t *pic);
extern void Draw_SubPic (float x, float y, float w, float h, qpic_t *pic, float s1, float t1, float s2, float t2, const float *rgb, float alpha);
extern void Draw_TransPicTranslate (int x, int y, qpic_t *pic, int top, int bottom);
extern void Draw_ConsoleBackground (void);
extern void Draw_TileClear (int x, int y, int w, int h);
extern void Draw_Fill (int x, int y, int w, int h, int c, float alpha);
extern void Draw_SetCanvas (canvastype newcanvas);
extern void Draw_SetCanvasColor (float r, float g, float b, float a);
extern void Draw_PushCanvasColor (float r, float g, float b, float a);
extern void Draw_PopCanvasColor (void);
extern void Draw_SetClipRect (float x, float y, float width, float height);
extern void Draw_ResetClipping (void);
extern void Draw_FadeScreen (float alpha);
extern void GL_SetCanvas (canvastype newcanvas);
extern void GL_SetCanvasColor (float r, float g, float b, float a);
extern void GL_PushCanvasColor (float r, float g, float b, float a);
extern void GL_PopCanvasColor (void);
extern void GL_Set2D (void);
extern void SCR_CenterPrint (const char *str);
extern void GL_SCR_BeginLoadingPlaque (void);
extern void GL_SCR_EndLoadingPlaque (void);
extern int GL_SCR_ModalMessage (const char *text, float timeout);
extern void Bridge_DrawFlush (void);
extern void Bridge_DrawInit (void);
extern void SCR_Init (void);
extern refdef_t r_refdef;

extern const IRenderBackend *GL_Backend_GetInterface (void);

static void REFGL_FillEntryPoints (void)
{
	/* TODO_RENDER_CONTRACT:
	 * Keep legacy callbacks grouped here until core contract + compat extension split. */
	s_entry_points.struct_size = sizeof (iw_renderer_entry_points_t);
	s_entry_points.R_Init = R_Init;
	s_entry_points.R_RenderView = REFGL_RenderView_Entry;
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
	s_entry_points.R_DrawPolyblendOverlay = R_DrawPolyblendOverlay;
	s_entry_points.R_GetCanvasMetrics = R_GetCanvasMetrics;
	s_entry_points.R_GetSceneSampleCount = R_GetSceneSampleCount;
	s_entry_points.R_GetMaxSampleCount = R_GetMaxSampleCount;
	s_entry_points.R_GetMaxAnisotropy = R_GetMaxAnisotropy;
	s_entry_points.R_IsClearEnabled = R_IsClearEnabled;
	s_entry_points.R_NewGame = R_NewGame;
	s_entry_points.R_CreateFrameBuffers = GL_CreateFrameBuffers;
	s_entry_points.R_DeleteFrameBuffers = GL_DeleteFrameBuffers;
	s_entry_points.R_ResetDRSState = R_ResetDRSState;
	s_entry_points.R_ResetGodraysStabilization = R_ResetGodraysStabilization;
	s_entry_points.SCR_UpdateScreen = SCR_UpdateScreen;
	s_entry_points.CL_RunParticles = CL_RunParticles;
	s_entry_points.Draw_PicFromWad2 = Draw_PicFromWad2;
	s_entry_points.Draw_PicFromWad = Draw_PicFromWad;
	s_entry_points.Draw_CachePic = Draw_CachePic;
	s_entry_points.Draw_TryCachePic = Draw_TryCachePic;
	s_entry_points.Draw_NewGame = Draw_NewGame;
	s_entry_points.Draw_FillEx = Draw_FillEx;
	s_entry_points.Draw_PartialFadeScreen = Draw_PartialFadeScreen;
	s_entry_points.Draw_Character = Draw_Character;
	s_entry_points.Draw_CharacterEx = Draw_CharacterEx;
	s_entry_points.Draw_String = Draw_String;
	s_entry_points.Draw_StringEx = Draw_StringEx;
	s_entry_points.Draw_Pic = Draw_Pic;
	s_entry_points.Draw_SubPic = Draw_SubPic;
	s_entry_points.Draw_TransPicTranslate = Draw_TransPicTranslate;
	s_entry_points.Draw_ConsoleBackground = Draw_ConsoleBackground;
	s_entry_points.Draw_TileClear = Draw_TileClear;
	s_entry_points.Draw_Fill = Draw_Fill;
	s_entry_points.Draw_SetCanvas = Draw_SetCanvas;
	s_entry_points.Draw_SetCanvasColor = Draw_SetCanvasColor;
	s_entry_points.Draw_PushCanvasColor = Draw_PushCanvasColor;
	s_entry_points.Draw_PopCanvasColor = Draw_PopCanvasColor;
	s_entry_points.Draw_SetClipRect = Draw_SetClipRect;
	s_entry_points.Draw_ResetClipping = Draw_ResetClipping;
	s_entry_points.Draw_FadeScreen = Draw_FadeScreen;
	s_entry_points.GL_SetCanvas = GL_SetCanvas;
	s_entry_points.GL_SetCanvasColor = GL_SetCanvasColor;
	s_entry_points.GL_PushCanvasColor = GL_PushCanvasColor;
	s_entry_points.GL_PopCanvasColor = GL_PopCanvasColor;
	s_entry_points.GL_Set2D = GL_Set2D;
	s_entry_points.SCR_CenterPrint = SCR_CenterPrint;
	s_entry_points.SCR_BeginLoadingPlaque = GL_SCR_BeginLoadingPlaque;
	s_entry_points.SCR_EndLoadingPlaque = GL_SCR_EndLoadingPlaque;
	s_entry_points.SCR_ModalMessage = GL_SCR_ModalMessage;
	s_entry_points.Draw_Flush = Bridge_DrawFlush;
	s_entry_points.Draw_Init = Bridge_DrawInit;
	s_entry_points.SCR_Init = SCR_Init;
}

static void REFGL_RenderView_Entry (void)
{
	static qboolean warned_missing_bridge_refdef = false;

	if (g_bridge_data && g_bridge_data->r_refdef)
	{
		/* Keep camera/FOV in lockstep with host view build, but preserve plugin-side
		 * viewport/scale ownership to avoid clobbering runtime render-target setup. */
		VectorCopy (g_bridge_data->r_refdef->vieworg, r_refdef.vieworg);
		VectorCopy (g_bridge_data->r_refdef->viewangles, r_refdef.viewangles);
		r_refdef.basefov = g_bridge_data->r_refdef->basefov;
		r_refdef.fov_x = g_bridge_data->r_refdef->fov_x;
		r_refdef.fov_y = g_bridge_data->r_refdef->fov_y;
	}
	else if (!warned_missing_bridge_refdef)
	{
		Con_DWarning ("ref_gl: missing host refdef bridge in RenderView entry; using local refdef fallback.\n");
		warned_missing_bridge_refdef = true;
	}

	R_RenderView ();
}

static qboolean IW_RendererRefGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	Con_Printf ("ref_gl: plugin register begin\n");

	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V4_SIZE)
	{
		Con_Warning ("ref_gl: host_api missing or too small (got=%u need>=%u)\n",
			host_api ? host_api->struct_size : 0u,
			IW_RENDERER_PLUGIN_HOST_API_V4_SIZE);
		return false;
	}

	if (host_api->abi_major != IW_RENDERER_PLUGIN_ABI_MAJOR)
	{
		Con_Warning ("ref_gl: ABI major mismatch host=%u plugin=%u\n",
			host_api->abi_major, IW_RENDERER_PLUGIN_ABI_MAJOR);
		return false;
	}

	if (host_api->abi_minor < IW_RENDERER_PLUGIN_ABI_MINOR)
	{
		Con_Warning ("ref_gl: ABI minor too old host=%u plugin requires >=%u\n",
			host_api->abi_minor, IW_RENDERER_PLUGIN_ABI_MINOR);
		return false;
	}

	if (IW_RENDERER_PLUGIN_HOST_HAS_FIELD (host_api, bridge) && host_api->bridge)
	{
		/* TODO_RESOURCE_BOUNDARY:
		 * Bridge use should shrink as neutral host/resource services mature. */
		Bridge_Init (host_api->bridge);
		s_bridge = host_api->bridge;
		Con_Printf ("ref_gl: bridge initialized (ABI v%u)\n", host_api->bridge->abi_version);
	}
	else
	{
		Con_DWarning ("ref_gl: host bridge not provided; using direct globals/legacy paths\n");
	}

	if (!host_api->register_backend)
	{
		Con_Warning ("ref_gl: host register_backend callback missing\n");
		return false;
	}

	const IRenderBackend *gl_backend = GL_Backend_GetInterface ();
	if (!gl_backend)
	{
		Con_Warning ("ref_gl: GL_Backend_GetInterface returned NULL\n");
		return false;
	}

	Con_Printf ("ref_gl: registering backend '%s' (caps: ts=%d compute=%d inst=%d indirect=%d)\n",
		gl_backend->name,
		gl_backend->get_caps ? gl_backend->get_caps ()->supports_timestamps : 0,
		gl_backend->get_caps ? gl_backend->get_caps ()->supports_compute : 0,
		gl_backend->get_caps ? gl_backend->get_caps ()->supports_draw_instanced : 0,
		gl_backend->get_caps ? gl_backend->get_caps ()->supports_draw_indirect : 0);

	if (!host_api->register_backend (gl_backend))
	{
		Con_Warning ("ref_gl: backend registration rejected by host\n");
		return false;
	}

	REFGL_FillEntryPoints ();

	if (IW_RENDERER_PLUGIN_HOST_HAS_FIELD (host_api, register_entry_points) && host_api->register_entry_points)
	{
		if (!host_api->register_entry_points (&s_entry_points))
		{
			Con_Warning ("ref_gl: register_entry_points callback rejected entry table\n");
			return false;
		}
	}
	else
	{
		Con_DWarning ("ref_gl: host register_entry_points callback unavailable; legacy dispatch may be incomplete\n");
	}

	Con_Printf ("ref_gl: plugin registered %d entry points\n", (int)(s_entry_points.struct_size));

	return true;
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	/* REF_GL_PRIVATE: plugin descriptor and register callback are ref_gl-owned ABI surface. */
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"ref_gl",
		IW_RendererRefGL_Register
	};

	Con_Printf ("ref_gl: plugin query '%s' abi=%u.%u\n",
		descriptor.plugin_name ? descriptor.plugin_name : "<unnamed>",
		descriptor.abi_major,
		descriptor.abi_minor);

	return &descriptor;
}
