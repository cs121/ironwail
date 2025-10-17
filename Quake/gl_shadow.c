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

typedef struct shadow_program_uniforms_s {
    GLuint program;
    GLint active_lights_loc;
    GLint show_shadows_loc;
    GLint light_pos_loc[MAX_SHADOW_LIGHTS];
    GLint light_radius_loc[MAX_SHADOW_LIGHTS];
    GLint light_color_loc[MAX_SHADOW_LIGHTS];
    GLint light_intensity_loc[MAX_SHADOW_LIGHTS];
    GLint light_bias_loc[MAX_SHADOW_LIGHTS];
    GLint light_normal_bias_loc[MAX_SHADOW_LIGHTS];
    GLint light_softness_loc[MAX_SHADOW_LIGHTS];
    GLint light_pcf_samples_loc[MAX_SHADOW_LIGHTS];
    GLint light_shadow_cube_loc[MAX_SHADOW_LIGHTS];
    qboolean sampler_units_initialized;
} shadow_program_uniforms_t;

#define SHADOW_PROGRAM_CACHE_MAX 32
#define SHADOW_TEXTURE_UNIT_BASE 8

static shadow_program_uniforms_t shadow_uniform_cache[SHADOW_PROGRAM_CACHE_MAX];
static int shadow_uniform_cache_count;

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

static void Shadow_ClearUniformCache(void)
{
    shadow_uniform_cache_count = 0;
    memset(shadow_uniform_cache, 0, sizeof(shadow_uniform_cache));
}

static shadow_program_uniforms_t *Shadow_GetProgramUniforms(GLuint program)
{
    int i;
    shadow_program_uniforms_t *entry = NULL;

    for (i = 0; i < shadow_uniform_cache_count; ++i)
    {
        if (shadow_uniform_cache[i].program == program)
            return &shadow_uniform_cache[i];
    }

    if (shadow_uniform_cache_count >= SHADOW_PROGRAM_CACHE_MAX)
        return NULL;

    entry = &shadow_uniform_cache[shadow_uniform_cache_count++];
    memset(entry, 0, sizeof(*entry));
    entry->program = program;
    entry->active_lights_loc = -1;
    entry->show_shadows_loc = -1;
    for (i = 0; i < MAX_SHADOW_LIGHTS; ++i)
    {
        entry->light_pos_loc[i] = -1;
        entry->light_radius_loc[i] = -1;
        entry->light_color_loc[i] = -1;
        entry->light_intensity_loc[i] = -1;
        entry->light_bias_loc[i] = -1;
        entry->light_normal_bias_loc[i] = -1;
        entry->light_softness_loc[i] = -1;
        entry->light_pcf_samples_loc[i] = -1;
        entry->light_shadow_cube_loc[i] = -1;
    }

    entry->active_lights_loc = GL_GetUniformLocationFunc(program, "uActiveLights");
    entry->show_shadows_loc = GL_GetUniformLocationFunc(program, "uShowShadows");

    for (i = 0; i < MAX_SHADOW_LIGHTS; ++i)
    {
        char name[64];

        q_snprintf(name, sizeof(name), "uLights[%d].pos", i);
        entry->light_pos_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].radius", i);
        entry->light_radius_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].color", i);
        entry->light_color_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].intensity", i);
        entry->light_intensity_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].bias", i);
        entry->light_bias_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].normalBias", i);
        entry->light_normal_bias_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].softness", i);
        entry->light_softness_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].pcfSamples", i);
        entry->light_pcf_samples_loc[i] = GL_GetUniformLocationFunc(program, name);

        q_snprintf(name, sizeof(name), "uLights[%d].shadowCube", i);
        entry->light_shadow_cube_loc[i] = GL_GetUniformLocationFunc(program, name);
    }

    return entry;
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
    Shadow_ClearUniformCache();
    shadow_manager.initialized = true;
}

void R_ShutdownShadow(void)
{
    if (!shadow_manager.initialized)
        return;

    Shadow_ResetPool();
    Shadow_ClearUniformCache();
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

void R_ShadowApplyWorldUniforms(GLuint program)
{
    shadow_program_uniforms_t *uniforms;
    int show = (r_showshadows.value != 0.f) ? 1 : 0;
    int i;

    uniforms = Shadow_GetProgramUniforms(program);
    if (!uniforms)
        return;

    if (uniforms->show_shadows_loc >= 0)
        GL_Uniform1iFunc(uniforms->show_shadows_loc, show);

    if (uniforms->active_lights_loc >= 0)
        GL_Uniform1iFunc(uniforms->active_lights_loc, 0);

    for (i = 0; i < MAX_SHADOW_LIGHTS; ++i)
    {
        if (uniforms->light_shadow_cube_loc[i] >= 0)
            GL_Uniform1iFunc(uniforms->light_shadow_cube_loc[i], SHADOW_TEXTURE_UNIT_BASE + i);
    }
}

void R_ShadowEndFrame(void)
{
    /* Currently a placeholder for future per-frame cleanup. */
}
