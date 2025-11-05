#include "r_shadows.h"

#if R_SHADOWMAPS

#include "../Quake/gl_shadow.h"
#include "../Quake/glquake.h"

cvar_t r_shadows = { "r_shadows", "1", CVAR_ARCHIVE };

static qboolean shadowmaps_initialized = false;

static void R_ShadowEnsureInitialized(void)
{
        if (shadowmaps_initialized)
                return;

        Cvar_RegisterVariable(&r_shadows);
        R_InitShadow();
        shadowmaps_initialized = true;
}

void R_InitShadowMaps(void)
{
        R_ShadowEnsureInitialized();
        if (shadowmaps_initialized)
                R_ResizeShadowMapIfNeeded();
}

void R_BeginShadowPass(void)
{
        if (!shadowmaps_initialized)
                R_ShadowEnsureInitialized();

        if (!shadowmaps_initialized || !r_shadows.value)
                return;

        R_ResizeShadowMapIfNeeded();
        R_ShadowBeginFrame(r_framecount);
}

void R_EndShadowPass(void)
{
        if (!shadowmaps_initialized || !r_shadows.value)
                return;

        R_ShadowEndFrame();
}

void R_DrawShadowDebug(void)
{
        /* Debug rendering is not yet implemented. */
}

void R_UploadShadowMatrices(const float *matrices, int light_index)
{
        (void)matrices;
        (void)light_index;
}

#else /* !R_SHADOWMAPS */

cvar_t r_shadows = { "r_shadows", "0", CVAR_NONE };

void R_InitShadowMaps(void)
{
}

void R_BeginShadowPass(void)
{
}

void R_EndShadowPass(void)
{
}

void R_DrawShadowDebug(void)
{
}

void R_UploadShadowMatrices(const float *matrices, int light_index)
{
        (void)matrices;
        (void)light_index;
}

#endif
