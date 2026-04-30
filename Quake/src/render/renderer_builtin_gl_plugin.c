#include "quakedef.h"
#include "renderer_plugin.h"
#include "gl_backend.h"
#include "glquake.h"
#include "ref_gl_bridge.h"

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
extern void R_DrawPolyblendOverlay (const float blend_rgba[4]);
extern void R_GetCanvasMetrics (int *out_x, int *out_y, int *out_width, int *out_height);
extern int R_GetSceneSampleCount (void);
extern int R_GetMaxSampleCount (void);
extern float R_GetMaxAnisotropy (void);
extern qboolean R_IsClearEnabled (void);
extern void GL_CreateFrameBuffers (void);
extern void GL_DeleteFrameBuffers (void);
extern void CL_RunParticles (void);
extern qpic_t *GL_Draw_PicFromWad2 (const char *name, unsigned int texflags);
extern qpic_t *GL_Draw_PicFromWad (const char *name);
extern qpic_t *GL_Draw_CachePic (const char *path);
extern qpic_t *GL_Draw_TryCachePic (const char *path, unsigned int texflags);
extern void GL_Draw_NewGame (void);
extern void GL_Draw_FillEx (float x, float y, float w, float h, const float *rgb, float alpha);
extern void GL_Draw_PartialFadeScreen (float x0, float x1, float y0, float y1, float alpha);
extern void GL_Draw_Character (int x, int y, int num);
extern void GL_Draw_CharacterEx (float x, float y, float dimx, float dimy, int num);
extern void GL_Draw_String (int x, int y, const char *str);
extern void GL_Draw_StringEx (float x, float y, float dim, const char *str);
extern void GL_Draw_Pic (int x, int y, qpic_t *pic);
extern void GL_Draw_SubPic (float x, float y, float w, float h, qpic_t *pic, float s1, float t1, float s2, float t2, const float *rgb, float alpha);
extern void GL_Draw_TransPicTranslate (int x, int y, qpic_t *pic, int top, int bottom);
extern void GL_Draw_ConsoleBackground (void);
extern void GL_Draw_TileClear (int x, int y, int w, int h);
extern void GL_Draw_Fill (int x, int y, int w, int h, int c, float alpha);
extern void GL_Draw_SetCanvas (canvastype newcanvas);
extern void GL_Draw_SetCanvasColor (float r, float g, float b, float a);
extern void GL_Draw_PushCanvasColor (float r, float g, float b, float a);
extern void GL_Draw_PopCanvasColor (void);
extern void GL_Draw_SetClipRect (float x, float y, float width, float height);
extern void GL_Draw_ResetClipping (void);
extern void GL_Draw_FadeScreen (float alpha);
extern void GL_Draw_SetGLCanvas (canvastype newcanvas);
extern void GL_Draw_SetGLCanvasColor (float r, float g, float b, float a);
extern void GL_Draw_PushGLCanvasColor (float r, float g, float b, float a);
extern void GL_Draw_PopGLCanvasColor (void);
extern void GL_Draw_Set2D (void);
extern void GL_SCR_CenterPrint (const char *str);
extern void GL_Draw_Flush (void);
extern void GL_Draw_Init (void);
extern void GL_SCR_Init (void);
extern void GL_SCR_UpdateScreen (void);
extern void GL_SCR_BeginLoadingPlaque (void);
extern void GL_SCR_EndLoadingPlaque (void);
extern int GL_SCR_ModalMessage (const char *text, float timeout);
extern const IRenderBackend *GL_Backend_GetInterface (void);

static void IW_RendererBuiltinGL_FillEntryPoints (void)
{
	memset (&s_entry_points, 0, sizeof (s_entry_points));
	s_entry_points.struct_size = sizeof (s_entry_points);
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
	s_entry_points.SCR_UpdateScreen = GL_SCR_UpdateScreen;
	s_entry_points.CL_RunParticles = CL_RunParticles;
	s_entry_points.Draw_PicFromWad2 = (struct qpic_s *(*)(const char *, unsigned int))GL_Draw_PicFromWad2;
	s_entry_points.Draw_PicFromWad = (struct qpic_s *(*)(const char *))GL_Draw_PicFromWad;
	s_entry_points.Draw_CachePic = (struct qpic_s *(*)(const char *))GL_Draw_CachePic;
	s_entry_points.Draw_TryCachePic = (struct qpic_s *(*)(const char *, unsigned int))GL_Draw_TryCachePic;
	s_entry_points.Draw_NewGame = GL_Draw_NewGame;
	s_entry_points.Draw_FillEx = GL_Draw_FillEx;
	s_entry_points.Draw_PartialFadeScreen = GL_Draw_PartialFadeScreen;
	s_entry_points.Draw_Character = GL_Draw_Character;
	s_entry_points.Draw_CharacterEx = GL_Draw_CharacterEx;
	s_entry_points.Draw_String = GL_Draw_String;
	s_entry_points.Draw_StringEx = GL_Draw_StringEx;
	s_entry_points.Draw_Pic = (void (*)(int, int, struct qpic_s *))GL_Draw_Pic;
	s_entry_points.Draw_SubPic = (void (*)(float, float, float, float, struct qpic_s *, float, float, float, float, const float *, float))GL_Draw_SubPic;
	s_entry_points.Draw_TransPicTranslate = (void (*)(int, int, struct qpic_s *, int, int))GL_Draw_TransPicTranslate;
	s_entry_points.Draw_ConsoleBackground = GL_Draw_ConsoleBackground;
	s_entry_points.Draw_TileClear = GL_Draw_TileClear;
	s_entry_points.Draw_Fill = GL_Draw_Fill;
	s_entry_points.Draw_SetCanvas = (void (*)(int))GL_Draw_SetCanvas;
	s_entry_points.Draw_SetCanvasColor = GL_Draw_SetCanvasColor;
	s_entry_points.Draw_PushCanvasColor = GL_Draw_PushCanvasColor;
	s_entry_points.Draw_PopCanvasColor = GL_Draw_PopCanvasColor;
	s_entry_points.Draw_SetClipRect = GL_Draw_SetClipRect;
	s_entry_points.Draw_ResetClipping = GL_Draw_ResetClipping;
	s_entry_points.Draw_FadeScreen = GL_Draw_FadeScreen;
	s_entry_points.GL_SetCanvas = (void (*)(int))GL_Draw_SetGLCanvas;
	s_entry_points.GL_SetCanvasColor = GL_Draw_SetGLCanvasColor;
	s_entry_points.GL_PushCanvasColor = GL_Draw_PushGLCanvasColor;
	s_entry_points.GL_PopCanvasColor = GL_Draw_PopGLCanvasColor;
	s_entry_points.GL_Set2D = GL_Draw_Set2D;
	s_entry_points.SCR_CenterPrint = GL_SCR_CenterPrint;
	s_entry_points.SCR_BeginLoadingPlaque = GL_SCR_BeginLoadingPlaque;
	s_entry_points.SCR_EndLoadingPlaque = GL_SCR_EndLoadingPlaque;
	s_entry_points.SCR_ModalMessage = GL_SCR_ModalMessage;
	s_entry_points.Draw_Flush = GL_Draw_Flush;
	s_entry_points.Draw_Init = GL_Draw_Init;
	s_entry_points.SCR_Init = GL_SCR_Init;
}

static qboolean IW_RendererBuiltinGL_Register (const iw_renderer_plugin_host_api_t *host_api)
{
	const IRenderBackend *gl_backend;

	if (!host_api || host_api->struct_size < IW_RENDERER_PLUGIN_HOST_API_V4_SIZE)
		return false;
	if (!host_api->register_backend || !host_api->register_entry_points)
		return false;

	gl_backend = GL_Backend_GetInterface ();
	if (!gl_backend)
		return false;
	if (!host_api->register_backend (gl_backend))
		return false;

	IW_RendererBuiltinGL_FillEntryPoints ();
	return host_api->register_entry_points (&s_entry_points);
}

qboolean IW_RendererBuiltinGL_RegisterInternal (const iw_renderer_plugin_host_api_t *host_api)
{
	return IW_RendererBuiltinGL_Register (host_api);
}

IW_RENDERER_PLUGIN_EXPORT const iw_renderer_plugin_descriptor_t *IW_RendererPlugin_Query (void)
{
	static const iw_renderer_plugin_descriptor_t descriptor = {
		sizeof (iw_renderer_plugin_descriptor_t),
		IW_RENDERER_PLUGIN_ABI_MAJOR,
		IW_RENDERER_PLUGIN_ABI_MINOR,
		"builtin-opengl",
		IW_RendererBuiltinGL_Register
	};

	return &descriptor;
}


