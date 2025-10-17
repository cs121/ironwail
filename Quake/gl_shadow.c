#include "quakedef.h"
#include "gl_shadow.h"

#define SHADOW_POOL_SIZE 16

cvar_t gl_shadows = { "gl_shadows", "1", CVAR_ARCHIVE };
cvar_t gl_shadow_mapsize = { "gl_shadow_mapsize", "512", CVAR_ARCHIVE };
cvar_t gl_shadow_softness = { "gl_shadow_softness", "0.02", CVAR_ARCHIVE };
cvar_t gl_shadow_pcf_samples = { "gl_shadow_pcf_samples", "8", CVAR_ARCHIVE };
cvar_t gl_shadow_maxlights = { "gl_shadow_maxlights", "4", CVAR_ARCHIVE };
cvar_t gl_shadow_bias = { "gl_shadow_bias", "0.002", CVAR_ARCHIVE };
cvar_t gl_shadow_normalbias = { "gl_shadow_normalbias", "0.02", CVAR_ARCHIVE };
cvar_t gl_shadow_update_static_interval = { "gl_shadow_update_static_interval", "2", CVAR_ARCHIVE };
cvar_t gl_shadow_debug = { "gl_shadow_debug", "0", CVAR_NONE };
cvar_t r_showshadows = { "r_showshadows", "0", CVAR_NONE };

static struct shadow_manager_s {
    qboolean initialized;
    int map_size;
    int max_lights;
    dlight_shadow_t pool[SHADOW_POOL_SIZE];
    int pool_size;
} shadow_manager;

static void Shadow_DestroyCube(shadow_cube_t *cube)
{
    if (!cube)
        return;

    if (cube->color_cube_tex)
    {
        glDeleteTextures(1, &cube->color_cube_tex);
        cube->color_cube_tex = 0;
    }

    if (cube->depth_rb)
    {
        glDeleteRenderbuffers(1, &cube->depth_rb);
        cube->depth_rb = 0;
    }

    if (cube->fbo)
    {
        glDeleteFramebuffers(1, &cube->fbo);
        cube->fbo = 0;
    }

    cube->size = 0;
    cube->dirty = true;
    cube->last_update_frame = -9999;
}

static void Shadow_ResetPool(void)
{
    int i;
    for (i = 0; i < shadow_manager.pool_size; ++i)
    {
        dlight_shadow_t *slot = &shadow_manager.pool[i];
        Shadow_DestroyCube(&slot->cube);
        slot->light_id = -1;
        slot->is_static = false;
    }
    shadow_manager.pool_size = 0;
}

static void Shadow_RegisterCvars(void)
{
    static qboolean registered = false;
    if (registered)
        return;

    Cvar_RegisterVariable(&gl_shadows);
    Cvar_RegisterVariable(&gl_shadow_mapsize);
    Cvar_RegisterVariable(&gl_shadow_softness);
    Cvar_RegisterVariable(&gl_shadow_pcf_samples);
    Cvar_RegisterVariable(&gl_shadow_maxlights);
    Cvar_RegisterVariable(&gl_shadow_bias);
    Cvar_RegisterVariable(&gl_shadow_normalbias);
    Cvar_RegisterVariable(&gl_shadow_update_static_interval);
    Cvar_RegisterVariable(&gl_shadow_debug);
    Cvar_RegisterVariable(&r_showshadows);

    registered = true;
}

static int Shadow_ClampMapSize(int size)
{
    int clamped = 1;
    if (size <= 0)
        size = 1;

    while (clamped < size)
        clamped <<= 1;

    if (clamped < 16)
        clamped = 16;
    if (clamped > 4096)
        clamped = 4096;
    return clamped;
}

static void Shadow_UpdateSettings(void)
{
    shadow_manager.map_size = Shadow_ClampMapSize((int)gl_shadow_mapsize.value);
    shadow_manager.max_lights = CLAMP(0, (int)gl_shadow_maxlights.value, SHADOW_POOL_SIZE);
}

void R_InitShadow(void)
{
    memset(&shadow_manager, 0, sizeof(shadow_manager));
    Shadow_RegisterCvars();
    Shadow_UpdateSettings();
    shadow_manager.initialized = true;
}

void R_ShutdownShadow(void)
{
    if (!shadow_manager.initialized)
        return;

    Shadow_ResetPool();
    shadow_manager.initialized = false;
}

void R_ResizeShadowMapIfNeeded(void)
{
    int new_size;
    if (!shadow_manager.initialized)
        return;

    new_size = Shadow_ClampMapSize((int)gl_shadow_mapsize.value);
    if (new_size != shadow_manager.map_size)
    {
        shadow_manager.map_size = new_size;
        Shadow_ResetPool();
    }

    shadow_manager.max_lights = CLAMP(0, (int)gl_shadow_maxlights.value, SHADOW_POOL_SIZE);
}

void R_ShadowBeginFrame(int frame_num)
{
    (void)frame_num;
    if (!shadow_manager.initialized)
        return;

    Shadow_UpdateSettings();
}

void R_ShadowEndFrame(void)
{
    /* Currently a placeholder for future per-frame cleanup. */
}
