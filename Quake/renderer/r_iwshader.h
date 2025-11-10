#pragma once
#include <stdint.h>

#define IW_MAX_STAGES       16
#define IW_MAX_TCMODS       8
#define IW_MAX_NAME         96
#define IW_MAX_PATH         96
#define IW_MAX_ANIM_FRAMES  64
#define IW_MAX_Q3MAP_DIRECTIVES 32
#define IW_MAX_DEFORM_COMMANDS 16
#define IW_MAX_DIRECTIVE_TEXT 192
#define IW_MAX_STAGE_CONDITION 96
#define IW_MAX_TCGEN_TEXT   96

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
    IW_ALPHA_FUNC_DISABLED = 0,
    IW_ALPHA_FUNC_NEVER,
    IW_ALPHA_FUNC_LESS,
    IW_ALPHA_FUNC_EQUAL,
    IW_ALPHA_FUNC_LEQUAL,
    IW_ALPHA_FUNC_GREATER,
    IW_ALPHA_FUNC_NOTEQUAL,
    IW_ALPHA_FUNC_GEQUAL,
    IW_ALPHA_FUNC_ALWAYS
} iwAlphaFunc_t;

typedef enum {
    IW_DEPTHFUNC_DEFAULT = -1,
    IW_DEPTHFUNC_NEVER = 0,
    IW_DEPTHFUNC_LESS,
    IW_DEPTHFUNC_EQUAL,
    IW_DEPTHFUNC_LEQUAL,
    IW_DEPTHFUNC_GREATER,
    IW_DEPTHFUNC_NOTEQUAL,
    IW_DEPTHFUNC_GEQUAL,
    IW_DEPTHFUNC_ALWAYS
} iwDepthFunc_t;

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
    IW_TCGEN_BASE = 0,
    IW_TCGEN_LIGHTMAP,
    IW_TCGEN_ENVIRONMENT,
    IW_TCGEN_VECTOR
} iwTCGenMode_t;

typedef enum {
    IW_TC_ALIGN_OBJECT = 0,
    IW_TC_ALIGN_WORLD,
    IW_TC_ALIGN_SCREEN
} iwTCAlign_t;

typedef enum {
    IW_MAP_TEXTURE2D = 0,
    IW_MAP_CLAMP2D,
    IW_MAP_CUBEMAP,
    IW_MAP_LIGHTMAP,
    IW_MAP_PROCEDURAL_WHITE,
    IW_MAP_PROCEDURAL_BLACK
} iwMapType_t;

typedef struct {
    float color[3];
    float depthForOpaque;
    float density;
    int hasColor;
    int hasDepthForOpaque;
    int hasDensity;
} iwFogParms_t;

typedef enum {
    IW_WAVE_SIN = 0,
    IW_WAVE_TRIANGLE,
    IW_WAVE_SQUARE,
    IW_WAVE_SAW,
    IW_WAVE_FRESNEL
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
    int isClamp;
    iwMapType_t mapType;
    iwBlendMode_t blendMode;
    iwBlendFactor_t src, dst;
    iwRGBGen_t rgbgen;
    float rgbConst[3];
    iwWave_t rgbWave;
    iwAlphaGen_t alphagen;
    float aConst;
    iwWave_t alphaWave;
    iwChannel_t mask;
    int maskExplicit;
    int emissive;
    int depthWrite;
    int depthTest;
    int depthTestExplicit;
    iwColorMask_t colorMask;
    iwTCGenMode_t tcGenMode;
    iwTCAlign_t tcAlign;
    int tcAlignExplicit;
    char tcGen[IW_MAX_TCGEN_TEXT];
    float tcGenVectors[2][3];
    char alphaFunc[32];
    iwAlphaFunc_t alphaFuncMode;
    float alphaFuncRef;
    int alphaFuncExplicit;
    char depthFunc[32];
    iwDepthFunc_t depthFuncMode;
    int depthFuncExplicit;
    char stageCondition[IW_MAX_STAGE_CONDITION];
    iwFogParms_t fogParms;
    int hasFogParms;
    int alphaToCoverage;
    int animMap;
    float animFps;
    int numAnimFrames;
    char animPaths[IW_MAX_ANIM_FRAMES][IW_MAX_PATH];
    int numTCMods;
    iwTCMod_t tcmods[IW_MAX_TCMODS];
    char normalMap[IW_MAX_PATH];
    char glossMap[IW_MAX_PATH];
    float specularScale;
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
    char fogShader[IW_MAX_PATH];
    int hasFog;
    iwFogParms_t fogParms;
    int hasFogParms;
    int portal;
    int hasSkyParms;
    char skyParms[3][IW_MAX_PATH];
    float tessSize;
    int hasTessSize;
    int numQ3MapDirectives;
    char q3mapDirectives[IW_MAX_Q3MAP_DIRECTIVES][IW_MAX_DIRECTIVE_TEXT];
    int numDeformVertexes;
    char deformVertexes[IW_MAX_DEFORM_COMMANDS][IW_MAX_DIRECTIVE_TEXT];
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

void IW_ShaderSystem_PrepareForGameDir(const char* gamedir);

void IW_LoadShaderDirectory(const char* dir);

const iwMaterial_t* IW_FindMaterial(const char* materialName);

const iwMaterial_t* IW_MaterialForTexture(const char* textureName);

void IW_TexMatrixIdentity(iwTexMatrix_t* out);
qboolean IW_StageTexMatrix(const iwStage_t* stage, float time, iwTexMatrix_t* out);
qboolean IW_MaterialTexMatrix(const iwMaterial_t* material, float time, iwTexMatrix_t* out);
float IW_GetAnimRate(void);

void IW_DumpMaterials(const char* outPath);

const iwMaterial_t* IW_DebugLastMaterial(void);
const char* IW_DebugLastTexture(void);
qboolean IW_DebugOverlayText(char* buffer, size_t bufferSize);
void IW_Debugf(const char* fmt, ...) FUNC_PRINTF(1, 2);

#ifdef IW_BUILD_TESTS
qboolean IW_ParseShaderTextForTesting(const char *text, iwMaterial_t *outMaterial);
#endif

#ifdef __cplusplus
}
#endif

