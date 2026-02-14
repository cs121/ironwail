#ifndef R_DECALS_H
#define R_DECALS_H

void R_Decals_Init (void);
void R_Decals_Shutdown (void);
void R_Decals_Clear (void);
void R_Decals_ReloadScripts (void);
void R_Decals_FrameBegin (void);
void R_Decals_Add (const char *name, const vec3_t org, const vec3_t normal, const float *rgba_opt, float size_opt, int flags);
void R_DrawDecals (qboolean showtris);
void R_DrawDecals_ShowTris (void);

void R_AddBulletDecal (const vec3_t point);
void R_AddBloodDecal (const vec3_t point, const vec3_t dir);
void R_AddScorchDecal (const vec3_t point);

#endif
