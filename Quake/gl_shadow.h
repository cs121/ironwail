#ifndef GL_SHADOW_H
#define GL_SHADOW_H

#include "quakedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SHADOW_CUBE_FACES 6
#define MAX_SHADOW_LIGHTS 4

typedef struct shadow_cube_s {
    GLuint fbo;
    GLuint color_cube_tex;
    GLuint depth_rb;
    int size;
    vec3_t light_pos;
    float radius;
    qboolean dirty;
    int last_update_frame;
} shadow_cube_t;

typedef struct dlight_shadow_s {
    int light_id;
    shadow_cube_t cube;
    qboolean is_static;
} dlight_shadow_t;

void R_InitShadow(void);
void R_ShutdownShadow(void);
void R_ResizeShadowMapIfNeeded(void);
void R_ShadowBeginFrame(int frame_num);
void R_ShadowEndFrame(void);
void R_ShadowApplyWorldUniforms(GLuint program);
void R_ShadowSyncWorldLights(const gpulight_t *lights, int numlights);

extern cvar_t gl_shadows;
extern cvar_t gl_shadow_mapsize;
extern cvar_t gl_shadow_softness;
extern cvar_t gl_shadow_pcf_samples;
extern cvar_t gl_shadow_maxlights;
extern cvar_t gl_shadow_bias;
extern cvar_t gl_shadow_normalbias;
extern cvar_t gl_shadow_update_static_interval;
extern cvar_t gl_shadow_debug;
extern cvar_t r_showshadows;

#ifdef __cplusplus
}
#endif

#endif /* GL_SHADOW_H */
