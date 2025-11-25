#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

#include "quakedef.h"

#define ATLAS_MAX_TEXTURES 4096
#define ATLAS_SIZE 4096

typedef struct atlas_texture_s {
    char name[64];
    int width;
    int height;
    unsigned char *rgba; // 4x8-bit
} atlas_texture_t;

typedef struct atlas_rect_s {
    int x, y, w, h;
} atlas_rect_t;

typedef struct world_atlas_s {
    atlas_texture_t textures[ATLAS_MAX_TEXTURES];
    atlas_rect_t rects[ATLAS_MAX_TEXTURES];

    int texture_count;

    int atlas_w;
    int atlas_h;

    unsigned char *pixels; // size atlas_w * atlas_h * 4
    int built;
} world_atlas_t;

void Atlas_Init(void);
void Atlas_Reset(void);
void Atlas_RegisterTexture(const char *name, int w, int h, const unsigned char *rgba);
void Atlas_Build(const char *mapname);
void Atlas_SavePNG(const char *mapname);
void Atlas_SaveJSON(const char *mapname);
void Atlas_OnMapStart(const char *mapname);

#endif // TEXTURE_ATLAS_H
