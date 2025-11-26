#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

#include "quakedef.h"
#include "gl_texmgr.h"

#define ATLAS_MAX_TEXTURES 4096

typedef struct atlas_rect_s {
    float u1, v1, u2, v2;
    int exists; // 1 = im atlas
} atlas_rect_t;

void Atlas_Init(void);
int Atlas_LoadForMap(const char *mapname);
void Atlas_Invalidate(void);
atlas_rect_t Atlas_GetUV(const char *name);
int Atlas_TextureExists(const char *name);
GLuint Atlas_GetGLTexture(void);

// Internal helper for render code
const gltexture_t *Atlas_GetGLTextureStruct(void);

#endif // TEXTURE_ATLAS_H
