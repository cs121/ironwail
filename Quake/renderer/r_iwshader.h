#pragma once
#include <stdint.h>

#define IW_MAX_STAGES       4
#define IW_MAX_TCMODS       4
#define IW_MAX_NAME         96
#define IW_MAX_PATH         96
#define IW_MAX_ANIM_FRAMES  16

#define IW_DEPTHWRITE_AUTO  (-1)

typedef enum {
    IW_SORT_OPAQUE = 0,
    IW_SORT_ALPHA,
    IW_SORT_ADDITIVE,
    IW_SORT_DECAL,
    IW_SORT_SKY,
    IW_SORT_CUSTOM
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
    IW_BLEND_ADD_ALPHA,
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
    IW_RGB_ENTITY,
    IW_RGB_WAVE
} iwRGBGen_t;

typedef enum {
    IW_A_VERTEX = 0,
    IW_A_CONST,
    IW_A_MASK,
    IW_A_ENTITY,
    IW_A_WAVE
} iwAlphaGen_t;

typedef enum {
    IW_TC_SCROLL = 0,
    IW_TC_SCALE,
    IW_TC_ROTATE,
    IW_TC_TRANSLATE,
    IW_TC_ENVMAP,
    IW_TC_STRETCH,
    IW_TC_TURB
} iwTCOp_t;

typedef enum {
    IW_TC_ALIGN_OBJECT = 0,
    IW_TC_ALIGN_WORLD,
    IW_TC_ALIGN_SCREEN
} iwTCAlign_t;

typedef enum {
    IW_WAVE_SIN = 0,
    IW_WAVE_TRIANGLE,
    IW_WAVE_SQUARE,
    IW_WAVE_SAW
} iwWaveFunc_t;

typedef struct {
    iwWaveFunc_t func;
    float base;
    float amplitude;
    float phase;
    float frequency;
} iwWave_t;

typedef enum {
    IW_CHANNEL_R = 0,
    IW_CHANNEL_G,
    IW_CHANNEL_B,
    IW_CHANNEL_A
} iwChannel_t;

typedef enum {
    IW_COLORMASK_NONE = 0,
    IW_COLORMASK_R = 1 << 0,
    IW_COLORMASK_G = 1 << 1,
    IW_COLORMASK_B = 1 << 2,
    IW_COLORMASK_A = 1 << 3,
    IW_COLORMASK_RGB = IW_COLORMASK_R | IW_COLORMASK_G | IW_COLORMASK_B,
    IW_COLORMASK_RGBA = IW_COLORMASK_RGB | IW_COLORMASK_A
} iwColorMask_t;

typedef enum {
    IW_SURF_SKY        = 1 << 0,
    IW_SURF_WATER      = 1 << 1,
    IW_SURF_SLIME      = 1 << 2,
    IW_SURF_LAVA       = 1 << 3,
    IW_SURF_NONSOLID   = 1 << 4,
    IW_SURF_LADDER     = 1 << 5,
    IW_SURF_SLICK      = 1 << 6,
    IW_SURF_NODRAW     = 1 << 7,
    IW_SURF_LIGHTMAPPED= 1 << 8,
    IW_SURF_FULLBRIGHT = 1 << 9
} iwSurfaceFlags_t;

typedef struct {
    iwTCOp_t op;
    float a, b;
    iwWave_t wave;
} iwTCMod_t;

typedef struct {
    char mapPath[IW_MAX_PATH];
    int clamp;
    iwBlendMode_t blendMode;
    iwBlendFactor_t src, dst;
    iwRGBGen_t rgbgen;
    float rgbConst[3];
    iwWave_t rgbWave;
    iwAlphaGen_t alphagen;
    float aConst;
    iwWave_t alphaWave;
    iwChannel_t mask;
    int emissive;
    int depthWrite;
    int depthTest;
    iwColorMask_t colorMask;
    iwTCAlign_t tcAlign;
    int tcAlignExplicit;
    int alphaToCoverage;
    int animMap;
    float animFps;
    int numAnimFrames;
    char animPaths[IW_MAX_ANIM_FRAMES][IW_MAX_PATH];
    int numTCMods;
    iwTCMod_t tcmods[IW_MAX_TCMODS];
} iwStage_t;

typedef struct {
    char name[IW_MAX_NAME];
    iwSort_t sort;
    int sortValue;
    iwCull_t cull;
    unsigned int surfaceFlags;
    int polygonOffset;
    int detail;
    char editorImage[IW_MAX_PATH];
    int strict;
    int numStages;
    iwStage_t stages[IW_MAX_STAGES];
} iwMaterial_t;

typedef struct {
    float matrix[4];
    float translate[2];
} iwTexMatrix_t;

#ifdef __cplusplus
extern "C" {
#endif

void IW_ShaderSystem_Init(void);
void IW_ShaderSystem_Shutdown(void);

void IW_LoadShaderDirectory(const char* dir);

const iwMaterial_t* IW_FindMaterial(const char* materialName);

const iwMaterial_t* IW_MaterialForTexture(const char* textureName);

void IW_TexMatrixIdentity(iwTexMatrix_t* out);
qboolean IW_StageTexMatrix(const iwStage_t* stage, float time, iwTexMatrix_t* out);
qboolean IW_MaterialTexMatrix(const iwMaterial_t* material, float time, iwTexMatrix_t* out);

void IW_DumpMaterials(const char* outPath);

const iwMaterial_t* IW_DebugLastMaterial(void);
const char* IW_DebugLastTexture(void);
qboolean IW_DebugOverlayText(char* buffer, size_t bufferSize);

#ifdef __cplusplus
}
#endif

