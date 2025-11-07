#pragma once
#include <stdint.h>

#define IW_MAX_STAGES   4
#define IW_MAX_TCMODS   4
#define IW_MAX_NAME     96
#define IW_MAX_PATH     96

typedef enum {
    IW_SORT_OPAQUE = 0,
    IW_SORT_ALPHA,
    IW_SORT_ADDITIVE,
    IW_SORT_DECAL,
    IW_SORT_SKY
} iwSort_t;

typedef enum {
    IW_CULL_BACK = 0,
    IW_CULL_FRONT,
    IW_CULL_NONE
} iwCull_t;

typedef enum {
    IW_BLEND_NONE = 0,
    IW_BLEND_ALPHA,
    IW_BLEND_ADD,
    IW_BLEND_MUL,
    IW_BLEND_PREMUL,
    IW_BLEND_CUSTOM
} iwBlendMode_t;

typedef enum {
    IW_SRC_ZERO = 0, IW_SRC_ONE,
    IW_SRC_SRC_COLOR, IW_SRC_ONE_MINUS_SRC_COLOR,
    IW_SRC_DST_COLOR, IW_SRC_ONE_MINUS_DST_COLOR,
    IW_SRC_SRC_ALPHA, IW_SRC_ONE_MINUS_SRC_ALPHA,
    IW_SRC_DST_ALPHA, IW_SRC_ONE_MINUS_DST_ALPHA
} iwBlendFactor_t;

typedef enum {
    IW_RGB_VERTEX = 0,
    IW_RGB_CONST,
    IW_RGB_IDENTITY,
    IW_RGB_ENTITY
} iwRGBGen_t;

typedef enum {
    IW_A_VERTEX = 0,
    IW_A_CONST,
    IW_A_MASK
} iwAlphaGen_t;

typedef enum {
    IW_TC_SCROLL = 0,
    IW_TC_SCALE,
    IW_TC_ROTATE,
    IW_TC_ENVMAP
} iwTCOp_t;

typedef struct {
    iwTCOp_t op;
    float a, b;
} iwTCMod_t;

typedef struct {
    char mapPath[IW_MAX_PATH];
    int clamp;
    iwBlendMode_t blendMode;
    iwBlendFactor_t src, dst;
    iwRGBGen_t rgbgen;
    float rgbConst[3];
    iwAlphaGen_t alphagen;
    float aConst;
    int emissive;
    int numTCMods;
    iwTCMod_t tcmods[IW_MAX_TCMODS];
} iwStage_t;

typedef struct {
    char name[IW_MAX_NAME];
    iwSort_t sort;
    iwCull_t cull;
    int strict;
    int numStages;
    iwStage_t stages[IW_MAX_STAGES];
} iwMaterial_t;

#ifdef __cplusplus
extern "C" {
#endif

void IW_ShaderSystem_Init(void);
void IW_ShaderSystem_Shutdown(void);

void IW_LoadShaderDirectory(const char* dir);

const iwMaterial_t* IW_FindMaterial(const char* materialName);

const iwMaterial_t* IW_MaterialForTexture(const char* textureName);

void IW_DumpMaterials(const char* outPath);

const iwMaterial_t* IW_DebugLastMaterial(void);
const char* IW_DebugLastTexture(void);
qboolean IW_DebugOverlayText(char* buffer, size_t bufferSize);

#ifdef __cplusplus
}
#endif

