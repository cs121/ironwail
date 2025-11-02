#include "quakedef.h"
#include "r_shadows.h"

#ifndef R_SHADOWMAPS
#define R_SHADOWMAPS 0
#endif

#if !R_SHADOWMAPS

void R_InitShadowMaps(void) { }
void R_ShutdownShadowMaps(void) { }
void R_BeginShadowPass(void) { }
void R_EndShadowPass(void) { }
void R_UploadShadowMatrices(const float *viewProj, int lightIndex)
{
	(void)viewProj;
	(void)lightIndex;
}
void R_DrawShadowDebug(void) { }

#else /* R_SHADOWMAPS */

shadow_context_t r_shadow_context;

cvar_t r_shadows = {"r_shadows", "0", CVAR_ARCHIVE};
cvar_t r_shadows_debug = {"r_shadows_debug", "0", 0};
cvar_t r_shadows_size = {"r_shadows_size", "2048", CVAR_ARCHIVE};

static qboolean shadow_cvars_registered = false;
static qboolean shadow_initialized = false;

static void R_ShadowMaps_CreateContext(void);
static void R_ShadowMaps_Reload_f(void);

static void R_ShadowMaps_Register(void)
{
	if (shadow_cvars_registered)
		return;

	Cvar_RegisterVariable(&r_shadows);
	Cvar_RegisterVariable(&r_shadows_debug);
	Cvar_RegisterVariable(&r_shadows_size);
	Cmd_AddCommand("r_shadows_reload", R_ShadowMaps_Reload_f);

	shadow_cvars_registered = true;
}

static void R_ShadowMaps_Log(const char *message)
{
	if (!r_shadows_debug.value)
		return;

	Con_Printf("[ShadowMaps] %s\n", message);
}

static int R_ShadowMaps_ClampSize(int size)
{
	int clamped = 1;

	if (size <= 0)
		size = 1;

	while (clamped < size)
		clamped <<= 1;

	if (clamped < 16)
		clamped = 16;

	return clamped;
}

static void R_ShadowMaps_CreateContext(void)
{
	if (!r_shadows.value)
	{
		R_ShadowMaps_Log("Init skipped (r_shadows is 0)");
		return;
	}

	memset(&r_shadow_context, 0, sizeof(r_shadow_context));

	r_shadow_context.size = R_ShadowMaps_ClampSize((int)r_shadows_size.value);
	r_shadow_context.atlas_cols = 1;
	r_shadow_context.atlas_rows = 1;
	r_shadow_context.backend_data = NULL;
#if defined(GLQUAKE_VERSION)
	r_shadow_context.gl_backend_data = NULL;
#endif
#if defined(VKQUAKE)
	r_shadow_context.vk_backend_data = NULL;
#endif

	shadow_initialized = true;

	R_ShadowMaps_Log("Initialized stub context");

	// TODO(backlog): create depth atlas, per-light FBO, pipeline state, sampler, and layout bindings.
}

void R_InitShadowMaps(void)
{
	R_ShadowMaps_Register();
	R_ShutdownShadowMaps();
	R_ShadowMaps_CreateContext();
}

void R_ShutdownShadowMaps(void)
{
	if (!shadow_initialized)
		return;

	shadow_initialized = false;
	memset(&r_shadow_context, 0, sizeof(r_shadow_context));

	R_ShadowMaps_Log("Shutdown stub context");
}

static void R_ShadowMaps_Reload_f(void)
{
	Con_Printf("[ShadowMaps] Reload requested\n");
	R_ShutdownShadowMaps();
	R_ShadowMaps_CreateContext();
}

void R_BeginShadowPass(void)
{
	if (!shadow_initialized || !r_shadows.value)
		return;

	R_ShadowMaps_Log("Begin shadow pass");
}

void R_EndShadowPass(void)
{
	if (!shadow_initialized || !r_shadows.value)
		return;

	R_ShadowMaps_Log("End shadow pass");
}

void R_UploadShadowMatrices(const float *viewProj, int lightIndex)
{
	if (!shadow_initialized || !r_shadows.value)
		return;

	(void)viewProj;
	(void)lightIndex;

	if (r_shadows_debug.value)
		Con_Printf("[ShadowMaps] Upload shadow matrices (light %d, TODO)\n", lightIndex);

	// TODO: matrix packing & cascade scheme.
}

void R_DrawShadowDebug(void)
{
	if (!shadow_initialized || !r_shadows.value || !r_shadows_debug.value)
		return;

	Con_Printf("[ShadowMaps] Debug draw stub (atlas %dx%d, size %d)\n",
			r_shadow_context.atlas_cols,
			r_shadow_context.atlas_rows,
			r_shadow_context.size);

	// TODO: PCF/EVSM options.
}

#endif /* R_SHADOWMAPS */
