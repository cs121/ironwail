#include "quakedef.h"
#include "draw.h"
#include "glquake.h"
#include "screen.h"
#include "render_dispatch.h"

#define CANVAS_ALIGN_LEFT		0.f
#define CANVAS_ALIGN_CENTERX	0.5f
#define CANVAS_ALIGN_RIGHT		1.f
#define CANVAS_ALIGN_TOP		0.f
#define CANVAS_ALIGN_CENTERY	0.5f
#define CANVAS_ALIGN_BOTTOM		1.f

const iw_renderer_entry_points_t *g_rend = NULL;

static iw_renderer_entry_points_t s_entries;

static void RenderDispatch_ClearEntryPoints (iw_renderer_entry_points_t *entries)
{
	if (!entries)
		return;

	memset (entries, 0, sizeof (*entries));
	entries->struct_size = sizeof (*entries);
}

void RenderDispatch_Init (void)
{
	RenderDispatch_ClearEntryPoints (&s_entries);
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

	RenderDispatch_ClearEntryPoints (&s_entries);
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
	IW_RENDERER_ENTRY_COPY_FN (R_DrawPolyblendOverlay);
	IW_RENDERER_ENTRY_COPY_FN (R_GetCanvasMetrics);
	IW_RENDERER_ENTRY_COPY_FN (R_GetSceneSampleCount);
	IW_RENDERER_ENTRY_COPY_FN (R_GetMaxSampleCount);
	IW_RENDERER_ENTRY_COPY_FN (R_GetMaxAnisotropy);
	IW_RENDERER_ENTRY_COPY_FN (R_IsClearEnabled);
	IW_RENDERER_ENTRY_COPY_FN (R_NewGame);
	IW_RENDERER_ENTRY_COPY_FN (R_CreateFrameBuffers);
	IW_RENDERER_ENTRY_COPY_FN (R_DeleteFrameBuffers);
	IW_RENDERER_ENTRY_COPY_FN (R_ResetDRSState);
	IW_RENDERER_ENTRY_COPY_FN (R_ResetGodraysStabilization);
	IW_RENDERER_ENTRY_COPY_FN (SCR_UpdateScreen);
	IW_RENDERER_ENTRY_COPY_FN (CL_RunParticles);
	IW_RENDERER_ENTRY_COPY_FN (Draw_PicFromWad2);
	IW_RENDERER_ENTRY_COPY_FN (Draw_PicFromWad);
	IW_RENDERER_ENTRY_COPY_FN (Draw_CachePic);
	IW_RENDERER_ENTRY_COPY_FN (Draw_TryCachePic);
	IW_RENDERER_ENTRY_COPY_FN (Draw_NewGame);
	IW_RENDERER_ENTRY_COPY_FN (Draw_FillEx);
	IW_RENDERER_ENTRY_COPY_FN (Draw_PartialFadeScreen);
	IW_RENDERER_ENTRY_COPY_FN (Draw_Character);
	IW_RENDERER_ENTRY_COPY_FN (Draw_CharacterEx);
	IW_RENDERER_ENTRY_COPY_FN (Draw_String);
	IW_RENDERER_ENTRY_COPY_FN (Draw_StringEx);
	IW_RENDERER_ENTRY_COPY_FN (Draw_Pic);
	IW_RENDERER_ENTRY_COPY_FN (Draw_SubPic);
	IW_RENDERER_ENTRY_COPY_FN (Draw_TransPicTranslate);
	IW_RENDERER_ENTRY_COPY_FN (Draw_ConsoleBackground);
	IW_RENDERER_ENTRY_COPY_FN (Draw_TileClear);
	IW_RENDERER_ENTRY_COPY_FN (Draw_Fill);
	IW_RENDERER_ENTRY_COPY_FN (Draw_SetCanvas);
	IW_RENDERER_ENTRY_COPY_FN (Draw_SetCanvasColor);
	IW_RENDERER_ENTRY_COPY_FN (Draw_PushCanvasColor);
	IW_RENDERER_ENTRY_COPY_FN (Draw_PopCanvasColor);
	IW_RENDERER_ENTRY_COPY_FN (Draw_SetClipRect);
	IW_RENDERER_ENTRY_COPY_FN (Draw_ResetClipping);
	IW_RENDERER_ENTRY_COPY_FN (Draw_FadeScreen);
	IW_RENDERER_ENTRY_COPY_FN (GL_SetCanvas);
	IW_RENDERER_ENTRY_COPY_FN (GL_SetCanvasColor);
	IW_RENDERER_ENTRY_COPY_FN (GL_PushCanvasColor);
	IW_RENDERER_ENTRY_COPY_FN (GL_PopCanvasColor);
	IW_RENDERER_ENTRY_COPY_FN (GL_Set2D);
	IW_RENDERER_ENTRY_COPY_FN (SCR_CenterPrint);
	IW_RENDERER_ENTRY_COPY_FN (SCR_BeginLoadingPlaque);
	IW_RENDERER_ENTRY_COPY_FN (SCR_EndLoadingPlaque);
	IW_RENDERER_ENTRY_COPY_FN (SCR_ModalMessage);
	IW_RENDERER_ENTRY_COPY_FN (Draw_Flush);
	IW_RENDERER_ENTRY_COPY_FN (Draw_Init);
	IW_RENDERER_ENTRY_COPY_FN (SCR_Init);
	g_rend = &s_entries;

#undef IW_RENDERER_ENTRY_COPY_FN
#undef IW_RENDERER_ENTRY_HAS_FIELD
}

void RenderDispatch_UpdateScreen (void)
{
	if (g_rend && g_rend->SCR_UpdateScreen)
	{
		g_rend->SCR_UpdateScreen ();
		return;
	}
	/* Early startup can print through Con_Printf before ref_gl.dll is loaded.
	 * Treat that as "not ready yet" instead of hard-failing the process. */
}

#ifdef IW_RENDERER_HOST_FRONTEND
static void RenderDispatch_Unavailable (const char *name)
{
	Sys_Error ("Renderer entry point unavailable: %s", name);
}

#define RENDER_DISPATCH_CALL_VOID(field, ...) \
	do { \
		if (g_rend && g_rend->field) \
		{ \
			g_rend->field (__VA_ARGS__); \
			return; \
		} \
		RenderDispatch_Unavailable (#field); \
	} while (0)

#define RENDER_DISPATCH_CALL_RET(field, rettype, ...) \
	do { \
		if (g_rend && g_rend->field) \
			return (rettype)g_rend->field (__VA_ARGS__); \
		RenderDispatch_Unavailable (#field); \
		return (rettype)0; \
	} while (0)

qpic_t *Draw_PicFromWad2 (const char *name, unsigned int texflags)
{
	RENDER_DISPATCH_CALL_RET (Draw_PicFromWad2, qpic_t *, name, texflags);
}

qpic_t *Draw_PicFromWad (const char *name)
{
	RENDER_DISPATCH_CALL_RET (Draw_PicFromWad, qpic_t *, name);
}

qpic_t *Draw_CachePic (const char *path)
{
	RENDER_DISPATCH_CALL_RET (Draw_CachePic, qpic_t *, path);
}

qpic_t *Draw_TryCachePic (const char *path, unsigned int texflags)
{
	RENDER_DISPATCH_CALL_RET (Draw_TryCachePic, qpic_t *, path, texflags);
}

void Draw_NewGame (void)
{
	RENDER_DISPATCH_CALL_VOID (Draw_NewGame);
}

void Draw_Init (void)
{
	RENDER_DISPATCH_CALL_VOID (Draw_Init);
}

void Draw_Flush (void)
{
	RENDER_DISPATCH_CALL_VOID (Draw_Flush);
}

void Draw_Character (int x, int y, int num)
{
	RENDER_DISPATCH_CALL_VOID (Draw_Character, x, y, num);
}

void Draw_CharacterEx (float x, float y, float dimx, float dimy, int num)
{
	RENDER_DISPATCH_CALL_VOID (Draw_CharacterEx, x, y, dimx, dimy, num);
}

void Draw_String (int x, int y, const char *str)
{
	RENDER_DISPATCH_CALL_VOID (Draw_String, x, y, str);
}

void Draw_StringEx (float x, float y, float dim, const char *str)
{
	RENDER_DISPATCH_CALL_VOID (Draw_StringEx, x, y, dim, str);
}

void Draw_Pic (int x, int y, qpic_t *pic)
{
	RENDER_DISPATCH_CALL_VOID (Draw_Pic, x, y, (struct qpic_s *)pic);
}

void Draw_SubPic (float x, float y, float w, float h, qpic_t *pic, float s1, float t1, float s2, float t2, const float *rgb, float alpha)
{
	RENDER_DISPATCH_CALL_VOID (Draw_SubPic, x, y, w, h, (struct qpic_s *)pic, s1, t1, s2, t2, rgb, alpha);
}

void Draw_TransPicTranslate (int x, int y, qpic_t *pic, int top, int bottom)
{
	RENDER_DISPATCH_CALL_VOID (Draw_TransPicTranslate, x, y, (struct qpic_s *)pic, top, bottom);
}

void Draw_ConsoleBackground (void)
{
	RENDER_DISPATCH_CALL_VOID (Draw_ConsoleBackground);
}

void Draw_TileClear (int x, int y, int w, int h)
{
	RENDER_DISPATCH_CALL_VOID (Draw_TileClear, x, y, w, h);
}

void Draw_Fill (int x, int y, int w, int h, int c, float alpha)
{
	RENDER_DISPATCH_CALL_VOID (Draw_Fill, x, y, w, h, c, alpha);
}

void Draw_SetCanvas (canvastype newcanvas)
{
	RENDER_DISPATCH_CALL_VOID (Draw_SetCanvas, (int)newcanvas);
}

void Draw_SetCanvasColor (float r, float g, float b, float a)
{
	RENDER_DISPATCH_CALL_VOID (Draw_SetCanvasColor, r, g, b, a);
}

void Draw_PushCanvasColor (float r, float g, float b, float a)
{
	RENDER_DISPATCH_CALL_VOID (Draw_PushCanvasColor, r, g, b, a);
}

void Draw_PopCanvasColor (void)
{
	RENDER_DISPATCH_CALL_VOID (Draw_PopCanvasColor);
}

void Draw_SetClipRect (float x, float y, float width, float height)
{
	RENDER_DISPATCH_CALL_VOID (Draw_SetClipRect, x, y, width, height);
}

void Draw_ResetClipping (void)
{
	RENDER_DISPATCH_CALL_VOID (Draw_ResetClipping);
}

void Draw_FadeScreen (float alpha)
{
	RENDER_DISPATCH_CALL_VOID (Draw_FadeScreen, alpha);
}

void SCR_CenterPrint (const char *str)
{
	RENDER_DISPATCH_CALL_VOID (SCR_CenterPrint, str);
}

void SCR_BeginLoadingPlaque (void)
{
	RENDER_DISPATCH_CALL_VOID (SCR_BeginLoadingPlaque);
}

void SCR_EndLoadingPlaque (void)
{
	RENDER_DISPATCH_CALL_VOID (SCR_EndLoadingPlaque);
}

int SCR_ModalMessage (const char *text, float timeout)
{
	if (g_rend && g_rend->SCR_ModalMessage)
		return g_rend->SCR_ModalMessage (text, timeout);
	RenderDispatch_Unavailable ("SCR_ModalMessage");
	return 0;
}

void SCR_Init (void)
{
	RENDER_DISPATCH_CALL_VOID (SCR_Init);
}

void SCR_UpdateScreen (void)
{
	RenderDispatch_UpdateScreen ();
}

void SCR_PixelAspect_f (cvar_t *cvar)
{
	(void)cvar;
	VID_RecalcInterfaceSize ();
}

void SCR_UpdateZoom (void)
{
	float speed = scr_zoomspeed.value > 0.f ? scr_zoomspeed.value : 1e6f;
	float delta = cl.zoomdir * speed * (cl.time - cl.oldtime);
	if (!delta)
		return;

	cl.zoom += delta;
	if (cl.zoom >= 1.f)
	{
		cl.zoom = 1.f;
		cl.zoomdir = 0.f;
	}
	else if (cl.zoom <= 0.f)
	{
		cl.zoom = 0.f;
		cl.zoomdir = 0.f;
	}

	vid.recalc_refdef = 1;
}

void Draw_FillEx (float x, float y, float w, float h, const float *rgb, float alpha)
{
	RENDER_DISPATCH_CALL_VOID (Draw_FillEx, x, y, w, h, rgb, alpha);
}

void Draw_PartialFadeScreen (float x0, float x1, float y0, float y1, float alpha)
{
	RENDER_DISPATCH_CALL_VOID (Draw_PartialFadeScreen, x0, x1, y0, y1, alpha);
}

void GL_SetCanvas (canvastype newcanvas)
{
	RENDER_DISPATCH_CALL_VOID (GL_SetCanvas, (int)newcanvas);
}

void GL_SetCanvasColor (float r, float g, float b, float a)
{
	RENDER_DISPATCH_CALL_VOID (GL_SetCanvasColor, r, g, b, a);
}

void GL_PushCanvasColor (float r, float g, float b, float a)
{
	RENDER_DISPATCH_CALL_VOID (GL_PushCanvasColor, r, g, b, a);
}

void GL_PopCanvasColor (void)
{
	RENDER_DISPATCH_CALL_VOID (GL_PopCanvasColor);
}

void GL_Set2D (void)
{
	RENDER_DISPATCH_CALL_VOID (GL_Set2D);
}

static void RenderDispatch_Transform2 (float width, float height, float scalex, float scaley, float alignx, float aligny, drawtransform_t *out)
{
	float scrwidth = vid.guiwidth;
	float scrheight = vid.guiheight;
	out->scale[0] = scalex * 2.f / scrwidth;
	out->scale[1] = scaley * -2.f / scrheight;
	out->offset[0] = (scrwidth - width * scalex) * alignx / scrwidth * 2.f - 1.f;
	out->offset[1] = (scrheight - height * scaley) * aligny / scrheight * -2.f + 1.f;
	out->offset[0] += 0.61803399f / 2.f / glwidth;
	out->offset[1] += 0.61803399f / 2.f / glheight;
}

static void RenderDispatch_Transform (float width, float height, float scale, float alignx, float aligny, drawtransform_t *out)
{
	RenderDispatch_Transform2 (width, height, scale, scale, alignx, aligny, out);
}

void Draw_GetCanvasTransform (canvastype type, drawtransform_t *transform)
{
	extern vrect_t scr_vrect;
	float s, s2;

	switch (type)
	{
	case CANVAS_DEFAULT:
		RenderDispatch_Transform (vid.guiwidth, vid.guiheight, 1.f, CANVAS_ALIGN_CENTERX, CANVAS_ALIGN_CENTERY, transform);
		break;
	case CANVAS_CONSOLE:
		s = (float)vid.guiwidth / vid.conwidth;
		s2 = (float)vid.guiheight / vid.conheight;
		RenderDispatch_Transform2 (vid.conwidth, vid.conheight, s, s2, CANVAS_ALIGN_CENTERX, CANVAS_ALIGN_CENTERY, transform);
		transform->offset[1] += (1.f - scr_con_current / glheight) * 2.f;
		break;
	case CANVAS_MENU:
		s = q_min ((float)vid.guiwidth / 320.0f, (float)vid.guiheight / 200.0f);
		s = CLAMP (1.0f, scr_menuscale.value, s);
		RenderDispatch_Transform (320, 200, s, CANVAS_ALIGN_CENTERX, CANVAS_ALIGN_CENTERY, transform);
		break;
	case CANVAS_CSQC:
		s = CLAMP (1.0f, scr_sbarscale.value, vid.guiwidth / 320.0f);
		RenderDispatch_Transform (vid.guiwidth / s, vid.guiheight / s, s, CANVAS_ALIGN_CENTERX, CANVAS_ALIGN_CENTERY, transform);
		break;
	case CANVAS_SBAR:
		if (hudstyle == HUD_QUAKEWORLD)
			s = CLAMP (1.0f, scr_sbarscale.value, (float)vid.guiheight / 240.0f);
		else
			s = CLAMP (1.0f, scr_sbarscale.value, (float)vid.guiwidth / 320.0f);
		if (cl.gametype == GAME_DEATHMATCH && (hudstyle == HUD_CLASSIC || hudstyle == HUD_QUAKEWORLD))
			RenderDispatch_Transform (320, 48, s, CANVAS_ALIGN_LEFT, CANVAS_ALIGN_BOTTOM, transform);
		else
			RenderDispatch_Transform (320, 48, s, CANVAS_ALIGN_CENTERX, CANVAS_ALIGN_BOTTOM, transform);
		break;
	case CANVAS_SBAR_QW_INV:
		s = CLAMP (1.0f, scr_sbarscale.value, (float)vid.guiheight / 240.0f);
		RenderDispatch_Transform (48, 48, s, CANVAS_ALIGN_RIGHT, CANVAS_ALIGN_BOTTOM, transform);
		break;
	case CANVAS_SBAR2:
		s = q_min (vid.guiwidth / 400.0f, vid.guiheight / 225.0f);
		s = CLAMP (1.0f, scr_sbarscale.value, s);
		RenderDispatch_Transform (vid.guiwidth / s, vid.guiheight / s, s, CANVAS_ALIGN_CENTERX, CANVAS_ALIGN_CENTERY, transform);
		break;
	case CANVAS_CROSSHAIR:
		s = CLAMP (1.0f, scr_crosshairscale.value, 10.0f);
		RenderDispatch_Transform (vid.guiwidth / s / 2, vid.guiheight / s / 2, s, CANVAS_ALIGN_LEFT, CANVAS_ALIGN_BOTTOM, transform);
		transform->offset[0] += 1.f;
		transform->offset[1] += 1.f - ((scr_vrect.y + scr_vrect.height / 2) * 2 / (float)glheight);
		break;
	case CANVAS_BOTTOMLEFT:
		s = (float)vid.guiwidth / vid.conwidth;
		RenderDispatch_Transform (320, 200, s, CANVAS_ALIGN_LEFT, CANVAS_ALIGN_BOTTOM, transform);
		break;
	case CANVAS_BOTTOMRIGHT:
		s = (float)vid.guiwidth / vid.conwidth;
		RenderDispatch_Transform (320, 200, s, CANVAS_ALIGN_RIGHT, CANVAS_ALIGN_BOTTOM, transform);
		break;
	case CANVAS_TOPRIGHT:
		s = (float)vid.guiwidth / vid.conwidth;
		RenderDispatch_Transform (320, 200, s, CANVAS_ALIGN_RIGHT, CANVAS_ALIGN_TOP, transform);
		break;
	default:
		Sys_Error ("Draw_GetCanvasTransform: bad canvas type");
	}
}

void Draw_GetTransformBounds (const drawtransform_t *transform, float *left, float *top, float *right, float *bottom)
{
	*left = (-1.f - transform->offset[0]) / transform->scale[0];
	*right = (1.f - transform->offset[0]) / transform->scale[0];
	*bottom = (-1.f - transform->offset[1]) / transform->scale[1];
	*top = (1.f - transform->offset[1]) / transform->scale[1];
}

#undef RENDER_DISPATCH_CALL_RET
#undef RENDER_DISPATCH_CALL_VOID
#endif
