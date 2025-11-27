#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

#include <stdbool.h>
#include <stdint.h>

#include "quakedef.h"
#include "gl_texmgr.h"

#define ATLAS_MAX_TEXTURES 4096

typedef struct atlas_rect_s {
    float u1, v1, u2, v2;
    int exists; // 1 = im atlas
} atlas_rect_t;

struct qmodel_s;

void Atlas_Init(void);
int Atlas_LoadForMap(const char *mapname);
void Atlas_Invalidate(void);
void Atlas_CreateFromFallbacks(struct qmodel_s *model);
atlas_rect_t Atlas_GetUV(const char *name);
int Atlas_TextureExists(const char *name);
GLuint Atlas_GetGLTexture(void);

// Internal helper for render code
const gltexture_t *Atlas_GetGLTextureStruct(void);

bool SaveAtlasAsDDS(const char *output_path, const uint8_t *rgba_pixels, int width, int height, bool has_alpha);
bool PackAtlasContainer(const char *atlas_path, const char *json_path, const char *dds_path);

#endif // TEXTURE_ATLAS_H
