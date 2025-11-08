#include "quakedef.h"
#include "glquake.h"
#include "render.h"
#include "../common/com_iwtext.h"
#include "r_iwshader.h"
#include "q_ctype.h"

#define IW_MAX_MATERIALS 512

static cvar_t r_iwshader_cvar = { "r_iwshader", "1", CVAR_ARCHIVE };
static cvar_t r_iwshader_strict_cvar = { "r_iwshader_strict", "0", CVAR_NONE };
static cvar_t r_iwshader_debug_cvar = { "r_iwshader_debug", "0", CVAR_NONE };

static cmd_function_t *iwshader_dump_cmd;
static void IW_DumpMaterials_f(void);

typedef struct
{
    iwMaterial_t material;
    char normalized[IW_MAX_NAME];
} iwMaterialEntry_t;

static iwMaterialEntry_t iw_materials[IW_MAX_MATERIALS];
static int iw_num_materials;
static qboolean iw_initialized;

static const iwMaterial_t *iw_last_material;
static char iw_last_texture[IW_MAX_PATH];
static char iw_last_texture_display[IW_MAX_PATH];

static void IW_LogWarning(const char *file, int line, int column, const char *fmt, ...)
{
    char msg[512];
    va_list args;

    va_start(args, fmt);
    q_vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    Con_Printf("iwshader: %s:%d:%d: %s\n", file ? file : "<unknown>", line, column, msg);
}

static qboolean IW_NormalizeName(const char *input, char *output, size_t size, qboolean drop_extension)
{
    size_t len = 0;
    if (!input || !output || size == 0)
        return false;

    while (*input && len + 1 < size)
    {
        char c = *input++;
        if (c == '\r' || c == '\n' || c == '\t')
            break;
        if (c == '\\')
            c = '/';
        output[len++] = (char) q_tolower(c);
    }
    output[len] = '\0';

    while (len > 0 && output[len - 1] == ' ')
    {
        output[len - 1] = '\0';
        --len;
    }

    if (drop_extension && len > 0)
    {
        char *dot = strrchr(output, '.');
        char *slash = strrchr(output, '/');
        if (dot && (!slash || slash < dot))
            *dot = '\0';
    }

    return output[0] != '\0';
}

static void IW_CopyTokenLower(const iwtxtToken_t *token, char *dst, size_t size)
{
    size_t n = q_min(token->length, size - 1);
    size_t i;
    for (i = 0; i < n; ++i)
    {
        char c = token->text[i];
        if (c == '\\')
            c = '/';
        dst[i] = (char) q_tolower(c);
    }
    dst[i] = '\0';
}

static void IW_CopyTokenText(const iwtxtToken_t *token, char *dst, size_t size)
{
    size_t n = q_min(token->length, size - 1);
    memcpy(dst, token->text, n);
    dst[n] = '\0';
}

static qboolean IW_ReadFloat(iwtxtParser_t *parser, float *out, iwtxtToken_t *token)
{
    if (!IWTXT_NextToken(parser, token))
        return false;
    if (token->type != IWTXT_TOKEN_NUMBER)
        return false;
    *out = (float)token->number;
    return true;
}

static qboolean IW_ReadInt(iwtxtParser_t *parser, int *out, iwtxtToken_t *token)
{
    if (!IWTXT_NextToken(parser, token))
        return false;
    if (token->type != IWTXT_TOKEN_NUMBER)
        return false;
    *out = (int)token->number;
    return true;
}

static qboolean IW_ParseBlendFactor(const char *text, iwBlendFactor_t *out)
{
    if (!q_strcasecmp(text, "zero")) { *out = IW_SRC_ZERO; return true; }
    if (!q_strcasecmp(text, "one")) { *out = IW_SRC_ONE; return true; }
    if (!q_strcasecmp(text, "src_color")) { *out = IW_SRC_SRC_COLOR; return true; }
    if (!q_strcasecmp(text, "one_minus_src_color")) { *out = IW_SRC_ONE_MINUS_SRC_COLOR; return true; }
    if (!q_strcasecmp(text, "dst_color")) { *out = IW_SRC_DST_COLOR; return true; }
    if (!q_strcasecmp(text, "one_minus_dst_color")) { *out = IW_SRC_ONE_MINUS_DST_COLOR; return true; }
    if (!q_strcasecmp(text, "src_alpha")) { *out = IW_SRC_SRC_ALPHA; return true; }
    if (!q_strcasecmp(text, "one_minus_src_alpha")) { *out = IW_SRC_ONE_MINUS_SRC_ALPHA; return true; }
    if (!q_strcasecmp(text, "dst_alpha")) { *out = IW_SRC_DST_ALPHA; return true; }
    if (!q_strcasecmp(text, "one_minus_dst_alpha")) { *out = IW_SRC_ONE_MINUS_DST_ALPHA; return true; }
    return false;
}

static const char *IW_BlendFactorName(iwBlendFactor_t factor)
{
    switch (factor)
    {
    case IW_SRC_ZERO: return "zero";
    case IW_SRC_ONE: return "one";
    case IW_SRC_SRC_COLOR: return "src_color";
    case IW_SRC_ONE_MINUS_SRC_COLOR: return "one_minus_src_color";
    case IW_SRC_DST_COLOR: return "dst_color";
    case IW_SRC_ONE_MINUS_DST_COLOR: return "one_minus_dst_color";
    case IW_SRC_SRC_ALPHA: return "src_alpha";
    case IW_SRC_ONE_MINUS_SRC_ALPHA: return "one_minus_src_alpha";
    case IW_SRC_DST_ALPHA: return "dst_alpha";
    case IW_SRC_ONE_MINUS_DST_ALPHA: return "one_minus_dst_alpha";
    }
    return "zero";
}

static qboolean IW_ParseWaveFunc(const char *text, iwWaveFunc_t *out)
{
    if (!q_strcasecmp(text, "sin")) { *out = IW_WAVE_SIN; return true; }
    if (!q_strcasecmp(text, "triangle")) { *out = IW_WAVE_TRIANGLE; return true; }
    if (!q_strcasecmp(text, "square")) { *out = IW_WAVE_SQUARE; return true; }
    if (!q_strcasecmp(text, "saw")) { *out = IW_WAVE_SAW; return true; }
    return false;
}

static const char *IW_WaveFuncName(iwWaveFunc_t func)
{
    switch (func)
    {
    case IW_WAVE_SIN: return "sin";
    case IW_WAVE_TRIANGLE: return "triangle";
    case IW_WAVE_SQUARE: return "square";
    case IW_WAVE_SAW: return "saw";
    }
    return "sin";
}

static const char *IW_ChannelName(iwChannel_t channel)
{
    switch (channel)
    {
    case IW_CHANNEL_R: return "r";
    case IW_CHANNEL_G: return "g";
    case IW_CHANNEL_B: return "b";
    case IW_CHANNEL_A: return "a";
    }
    return "a";
}

static qboolean IW_ParseWave(iwtxtParser_t *parser, iwWave_t *wave, const char *filename, const iwtxtToken_t *modeToken)
{
    iwtxtToken_t token;
    if (!IWTXT_NextToken(parser, &token))
    {
        int line = modeToken ? modeToken->line : 0;
        int column = modeToken ? modeToken->column : 0;
        IW_LogWarning(filename, line, column, "wave requires a function");
        return false;
    }

    if (token.type != IWTXT_TOKEN_STRING)
    {
        IW_LogWarning(filename, token.line, token.column, "wave requires a function");
        return false;
    }

    char func[16];
    IW_CopyTokenLower(&token, func, sizeof(func));
    if (!IW_ParseWaveFunc(func, &wave->func))
    {
        IW_LogWarning(filename, token.line, token.column, "unknown wave func '%s'", func);
        return false;
    }

    if (!IW_ReadFloat(parser, &wave->base, &token) ||
        !IW_ReadFloat(parser, &wave->amplitude, &token) ||
        !IW_ReadFloat(parser, &wave->phase, &token) ||
        !IW_ReadFloat(parser, &wave->frequency, &token))
    {
        IW_LogWarning(filename, token.line, token.column, "wave requires base amplitude phase frequency");
        return false;
    }

    return true;
}

static void IW_SkipBraces(iwtxtParser_t *parser)
{
    iwtxtToken_t token;
    int depth = 0;
    while (IWTXT_NextToken(parser, &token))
    {
        if (token.type == IWTXT_TOKEN_SYMBOL && token.length == 1)
        {
            if (token.text[0] == '{')
                depth++;
            else if (token.text[0] == '}')
            {
                if (depth == 0)
                    break;
                depth--;
            }
        }
    }
}

static void IW_SetStageDefaults(iwStage_t *stage)
{
    memset(stage, 0, sizeof(*stage));
    q_strlcpy(stage->mapPath, "$white", sizeof(stage->mapPath));
    stage->blendMode = IW_BLEND_NONE;
    stage->src = IW_SRC_ONE;
    stage->dst = IW_SRC_ZERO;
    stage->rgbgen = IW_RGB_VERTEX;
    stage->rgbConst[0] = stage->rgbConst[1] = stage->rgbConst[2] = 1.f;
    stage->rgbWave.func = IW_WAVE_SIN;
    stage->rgbWave.base = 1.f;
    stage->rgbWave.amplitude = 0.f;
    stage->rgbWave.phase = 0.f;
    stage->rgbWave.frequency = 1.f;
    stage->alphagen = IW_A_VERTEX;
    stage->aConst = 1.f;
    stage->alphaWave.func = IW_WAVE_SIN;
    stage->alphaWave.base = 1.f;
    stage->alphaWave.amplitude = 0.f;
    stage->alphaWave.phase = 0.f;
    stage->alphaWave.frequency = 1.f;
    stage->mask = IW_CHANNEL_A;
    stage->emissive = 0;
    stage->clamp = 0;
    stage->numTCMods = 0;
}

static qboolean IW_ParseTCMod(iwtxtParser_t *parser, iwStage_t *stage, const iwtxtToken_t *firstToken, const char *filename)
{
    char op[16];
    IW_CopyTokenLower(firstToken, op, sizeof(op));

    if (stage->numTCMods >= IW_MAX_TCMODS)
    {
        IW_LogWarning(filename, firstToken->line, firstToken->column, "too many tcmods (max %d)", IW_MAX_TCMODS);
        IW_SkipBraces(parser);
        return false;
    }

    iwTCMod_t *tc = &stage->tcmods[stage->numTCMods];
    memset(tc, 0, sizeof(*tc));

    iwtxtToken_t token;
    if (!strcmp(op, "scroll"))
    {
        tc->op = IW_TC_SCROLL;
        if (!IW_ReadFloat(parser, &tc->a, &token) || !IW_ReadFloat(parser, &tc->b, &token))
        {
            IW_LogWarning(filename, token.line, token.column, "tcmod scroll requires two floats");
            return false;
        }
    }
    else if (!strcmp(op, "scale"))
    {
        tc->op = IW_TC_SCALE;
        if (!IW_ReadFloat(parser, &tc->a, &token) || !IW_ReadFloat(parser, &tc->b, &token))
        {
            IW_LogWarning(filename, token.line, token.column, "tcmod scale requires two floats");
            return false;
        }
    }
    else if (!strcmp(op, "rotate"))
    {
        tc->op = IW_TC_ROTATE;
        if (!IW_ReadFloat(parser, &tc->a, &token))
        {
            IW_LogWarning(filename, token.line, token.column, "tcmod rotate requires a float");
            return false;
        }
    }
    else if (!strcmp(op, "stretch"))
    {
        tc->op = IW_TC_STRETCH;
        if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_STRING)
        {
            IW_LogWarning(filename, token.line, token.column, "tcmod stretch requires a function");
            return false;
        }

        char func[16];
        IW_CopyTokenLower(&token, func, sizeof(func));
        if (!IW_ParseWaveFunc(func, &tc->wave.func))
        {
            IW_LogWarning(filename, token.line, token.column, "unknown stretch func '%s'", func);
            return false;
        }

        if (!IW_ReadFloat(parser, &tc->wave.base, &token) ||
            !IW_ReadFloat(parser, &tc->wave.amplitude, &token) ||
            !IW_ReadFloat(parser, &tc->wave.phase, &token) ||
            !IW_ReadFloat(parser, &tc->wave.frequency, &token))
        {
            IW_LogWarning(filename, token.line, token.column, "tcmod stretch requires base amplitude phase frequency");
            return false;
        }
    }
    else if (!strcmp(op, "turb"))
    {
        tc->op = IW_TC_TURB;
        if (!IW_ReadFloat(parser, &tc->a, &token) || !IW_ReadFloat(parser, &tc->b, &token))
        {
            IW_LogWarning(filename, token.line, token.column, "tcmod turb requires amplitude and frequency");
            return false;
        }
    }
    else if (!strcmp(op, "envmap"))
    {
        tc->op = IW_TC_ENVMAP;
        tc->a = tc->b = 0.f;
    }
    else
    {
        IW_LogWarning(filename, firstToken->line, firstToken->column, "unknown tcmod '%s'", op);
        return false;
    }

    stage->numTCMods++;
    return true;
}

static qboolean IW_ParseStage(iwtxtParser_t *parser, iwMaterial_t *material, const char *filename, qboolean strict)
{
    iwStage_t stage;
    IW_SetStageDefaults(&stage);

    iwtxtToken_t token;
    qboolean valid = true;

    while (IWTXT_NextToken(parser, &token))
    {
        if (token.type == IWTXT_TOKEN_SYMBOL && token.length == 1 && token.text[0] == '}')
            break;

        if (token.type != IWTXT_TOKEN_STRING)
        {
            IW_LogWarning(filename, token.line, token.column, "unexpected token inside stage");
            if (strict)
            {
                IW_SkipBraces(parser);
                return false;
            }
            continue;
        }

        char keyword[32];
        IW_CopyTokenLower(&token, keyword, sizeof(keyword));

        if (!strcmp(keyword, "map"))
        {
            if (!IWTXT_NextToken(parser, &token) || token.type == IWTXT_TOKEN_SYMBOL)
            {
                IW_LogWarning(filename, token.line, token.column, "map requires a texture path");
                if (strict)
                    return false;
                continue;
            }
            IW_CopyTokenText(&token, stage.mapPath, sizeof(stage.mapPath));
            for (size_t i = 0; stage.mapPath[i]; ++i)
                if (stage.mapPath[i] == '\\')
                    stage.mapPath[i] = '/';
        }
        else if (!strcmp(keyword, "blend"))
        {
            iwtxtToken_t tok;
            if (!IWTXT_NextToken(parser, &tok) || tok.type != IWTXT_TOKEN_STRING)
            {
                IW_LogWarning(filename, tok.line, tok.column, "blend requires parameters");
                if (strict)
                    return false;
                continue;
            }

            char mode[32];
            IW_CopyTokenLower(&tok, mode, sizeof(mode));
            if (!strcmp(mode, "alpha"))
            {
                stage.blendMode = IW_BLEND_ALPHA;
            }
            else if (!strcmp(mode, "add"))
            {
                stage.blendMode = IW_BLEND_ADD;
            }
            else if (!strcmp(mode, "mul"))
            {
                stage.blendMode = IW_BLEND_MUL;
            }
            else if (!strcmp(mode, "premul"))
            {
                stage.blendMode = IW_BLEND_PREMUL;
            }
            else if (!strcmp(mode, "add_alpha"))
            {
                stage.blendMode = IW_BLEND_ADD_ALPHA;
                stage.src = IW_SRC_SRC_ALPHA;
                stage.dst = IW_SRC_ONE;
            }
            else
            {
                char srcbuf[32], dstbuf[32];
                IW_CopyTokenLower(&tok, srcbuf, sizeof(srcbuf));
                if (!IWTXT_NextToken(parser, &tok) || tok.type != IWTXT_TOKEN_STRING)
                {
                    IW_LogWarning(filename, tok.line, tok.column, "blend requires two blend factors");
                    if (strict)
                        return false;
                    continue;
                }
                IW_CopyTokenLower(&tok, dstbuf, sizeof(dstbuf));
                if (!IW_ParseBlendFactor(srcbuf, &stage.src) || !IW_ParseBlendFactor(dstbuf, &stage.dst))
                {
                    IW_LogWarning(filename, tok.line, tok.column, "invalid blend factors '%s %s'", srcbuf, dstbuf);
                    if (strict)
                        return false;
                    continue;
                }
                stage.blendMode = IW_BLEND_CUSTOM;
            }
        }
        else if (!strcmp(keyword, "rgbgen"))
        {
            if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_STRING)
            {
                IW_LogWarning(filename, token.line, token.column, "rgbgen requires a mode");
                if (strict)
                    return false;
                continue;
            }
            char mode[32];
            IW_CopyTokenLower(&token, mode, sizeof(mode));
            if (!strcmp(mode, "vertex"))
            {
                stage.rgbgen = IW_RGB_VERTEX;
            }
            else if (!strcmp(mode, "identity"))
            {
                stage.rgbgen = IW_RGB_IDENTITY;
            }
            else if (!strcmp(mode, "const"))
            {
                stage.rgbgen = IW_RGB_CONST;
                if (!IW_ReadFloat(parser, &stage.rgbConst[0], &token) ||
                    !IW_ReadFloat(parser, &stage.rgbConst[1], &token) ||
                    !IW_ReadFloat(parser, &stage.rgbConst[2], &token))
                {
                    IW_LogWarning(filename, token.line, token.column, "rgbgen const requires three floats");
                    if (strict)
                        return false;
                }
            }
            else if (!strcmp(mode, "entity"))
            {
                stage.rgbgen = IW_RGB_ENTITY;
            }
            else if (!strcmp(mode, "wave"))
            {
                stage.rgbgen = IW_RGB_WAVE;
                if (!IW_ParseWave(parser, &stage.rgbWave, filename, &token))
                {
                    if (strict)
                        return false;
                    stage.rgbgen = IW_RGB_VERTEX;
                }
            }
            else
            {
                IW_LogWarning(filename, token.line, token.column, "unknown rgbgen '%s'", mode);
                if (strict)
                    return false;
            }
        }
        else if (!strcmp(keyword, "alphagen"))
        {
            if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_STRING)
            {
                IW_LogWarning(filename, token.line, token.column, "alphagen requires a mode");
                if (strict)
                    return false;
                continue;
            }
            char mode[32];
            IW_CopyTokenLower(&token, mode, sizeof(mode));
            if (!strcmp(mode, "vertex"))
            {
                stage.alphagen = IW_A_VERTEX;
            }
            else if (!strcmp(mode, "mask"))
            {
                stage.alphagen = IW_A_MASK;
            }
            else if (!strcmp(mode, "const"))
            {
                stage.alphagen = IW_A_CONST;
                if (!IW_ReadFloat(parser, &stage.aConst, &token))
                {
                    IW_LogWarning(filename, token.line, token.column, "alphagen const requires a float");
                    if (strict)
                        return false;
                }
            }
            else if (!strcmp(mode, "entity"))
            {
                stage.alphagen = IW_A_ENTITY;
            }
            else if (!strcmp(mode, "wave"))
            {
                stage.alphagen = IW_A_WAVE;
                if (!IW_ParseWave(parser, &stage.alphaWave, filename, &token))
                {
                    if (strict)
                        return false;
                    stage.alphagen = IW_A_VERTEX;
                }
            }
            else
            {
                IW_LogWarning(filename, token.line, token.column, "unknown alphagen '%s'", mode);
                if (strict)
                    return false;
            }
        }
        else if (!strcmp(keyword, "mask"))
        {
            if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_STRING)
            {
                IW_LogWarning(filename, token.line, token.column, "mask requires a channel");
                if (strict)
                    return false;
                continue;
            }
            char channel[8];
            IW_CopyTokenLower(&token, channel, sizeof(channel));
            if (!strcmp(channel, "r"))
                stage.mask = IW_CHANNEL_R;
            else if (!strcmp(channel, "g"))
                stage.mask = IW_CHANNEL_G;
            else if (!strcmp(channel, "b"))
                stage.mask = IW_CHANNEL_B;
            else if (!strcmp(channel, "a"))
                stage.mask = IW_CHANNEL_A;
            else
            {
                IW_LogWarning(filename, token.line, token.column, "unknown mask channel '%s'", channel);
                if (strict)
                    return false;
            }
        }
        else if (!strcmp(keyword, "tcmod"))
        {
            if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_STRING)
            {
                IW_LogWarning(filename, token.line, token.column, "tcmod requires an operator");
                if (strict)
                    return false;
                continue;
            }
            if (!IW_ParseTCMod(parser, &stage, &token, filename) && strict)
                return false;
        }
        else if (!strcmp(keyword, "emissive"))
        {
            int value;
            if (!IW_ReadInt(parser, &value, &token))
            {
                IW_LogWarning(filename, token.line, token.column, "emissive requires 0 or 1");
                if (strict)
                    return false;
                continue;
            }
            stage.emissive = value ? 1 : 0;
        }
        else if (!strcmp(keyword, "clamp"))
        {
            int value;
            if (!IW_ReadInt(parser, &value, &token))
            {
                IW_LogWarning(filename, token.line, token.column, "clamp requires 0 or 1");
                if (strict)
                    return false;
                continue;
            }
            stage.clamp = value ? 1 : 0;
        }
        else
        {
            IW_LogWarning(filename, token.line, token.column, "unknown stage keyword '%s'", keyword);
            if (strict)
            {
                IW_SkipBraces(parser);
                return false;
            }
            valid = false;
        }
    }

    if (!valid && strict)
        return false;

    if (material->numStages >= IW_MAX_STAGES)
    {
        IW_LogWarning(filename, token.line, token.column, "too many stages (max %d)", IW_MAX_STAGES);
        return false;
    }

    material->stages[material->numStages++] = stage;
    return true;
}

static qboolean IW_ParseGlobalKey(iwtxtParser_t *parser, iwMaterial_t *material, const char *keyword, const iwtxtToken_t *token, const char *filename)
{
    if (!strcmp(keyword, "cull"))
    {
        iwtxtToken_t value;
        if (!IWTXT_NextToken(parser, &value) || value.type != IWTXT_TOKEN_STRING)
        {
            IW_LogWarning(filename, value.line, value.column, "cull requires a value");
            return false;
        }
        char mode[16];
        IW_CopyTokenLower(&value, mode, sizeof(mode));
        if (!strcmp(mode, "back"))
            material->cull = IW_CULL_BACK;
        else if (!strcmp(mode, "front"))
            material->cull = IW_CULL_FRONT;
        else if (!strcmp(mode, "none"))
            material->cull = IW_CULL_NONE;
        else
            IW_LogWarning(filename, value.line, value.column, "unknown cull '%s'", mode);
        return true;
    }
    else if (!strcmp(keyword, "sort"))
    {
        iwtxtToken_t value;
        if (!IWTXT_NextToken(parser, &value) || value.type != IWTXT_TOKEN_STRING)
        {
            IW_LogWarning(filename, value.line, value.column, "sort requires a value");
            return false;
        }
        char mode[16];
        IW_CopyTokenLower(&value, mode, sizeof(mode));
        if (!strcmp(mode, "opaque"))
            material->sort = IW_SORT_OPAQUE;
        else if (!strcmp(mode, "alpha"))
            material->sort = IW_SORT_ALPHA;
        else if (!strcmp(mode, "additive"))
            material->sort = IW_SORT_ADDITIVE;
        else if (!strcmp(mode, "decal"))
            material->sort = IW_SORT_DECAL;
        else if (!strcmp(mode, "sky"))
            material->sort = IW_SORT_SKY;
        else
            IW_LogWarning(filename, value.line, value.column, "unknown sort '%s'", mode);
        return true;
    }
    else if (!strcmp(keyword, "strict"))
    {
        iwtxtToken_t value;
        if (!IW_ReadInt(parser, &material->strict, &value))
        {
            IW_LogWarning(filename, value.line, value.column, "strict requires 0 or 1");
            material->strict = 0;
        }
        material->strict = material->strict ? 1 : 0;
        return true;
    }

    IW_LogWarning(filename, token->line, token->column, "unknown keyword '%s'", keyword);
    return false;
}

static void IW_RegisterMaterial(const iwMaterial_t *material)
{
    char normalized[IW_MAX_NAME];
    if (!IW_NormalizeName(material->name, normalized, sizeof(normalized), false))
        return;

    for (int i = 0; i < iw_num_materials; ++i)
    {
        if (!strcmp(iw_materials[i].normalized, normalized))
        {
            iw_materials[i].material = *material;
            return;
        }
    }

    if (iw_num_materials >= IW_MAX_MATERIALS)
    {
        Con_Warning("iwshader: material limit (%d) reached\n", IW_MAX_MATERIALS);
        return;
    }

    iw_materials[iw_num_materials].material = *material;
    q_strlcpy(iw_materials[iw_num_materials].normalized, normalized, sizeof(normalized));
    iw_num_materials++;
}

static void IW_ParseShaderDefinition(iwtxtParser_t *parser, const iwtxtToken_t *nameToken, const char *filename)
{
    iwMaterial_t material;
    memset(&material, 0, sizeof(material));
    material.sort = IW_SORT_OPAQUE;
    material.cull = IW_CULL_BACK;
    material.strict = 0;
    material.numStages = 0;
    IW_CopyTokenText(nameToken, material.name, sizeof(material.name));

    iwtxtToken_t token;
    if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_SYMBOL || token.length != 1 || token.text[0] != '{')
    {
        IW_LogWarning(filename, token.line, token.column, "expected '{' after shader name");
        return;
    }

    while (IWTXT_NextToken(parser, &token))
    {
        if (token.type == IWTXT_TOKEN_SYMBOL && token.length == 1 && token.text[0] == '}')
            break;

        if (token.type == IWTXT_TOKEN_STRING)
        {
            char keyword[32];
            IW_CopyTokenLower(&token, keyword, sizeof(keyword));
            if (!strcmp(keyword, "stage"))
            {
                if (!IWTXT_NextToken(parser, &token) || token.type != IWTXT_TOKEN_SYMBOL || token.length != 1 || token.text[0] != '{')
                {
                    IW_LogWarning(filename, token.line, token.column, "expected '{' after stage");
                    IW_SkipBraces(parser);
                    continue;
                }
                qboolean strict = material.strict || (r_iwshader_strict_cvar.value != 0.f);
                if (!IW_ParseStage(parser, &material, filename, strict))
                    continue;
            }
            else
            {
                IW_ParseGlobalKey(parser, &material, keyword, &token, filename);
            }
        }
        else
        {
            IW_LogWarning(filename, token.line, token.column, "unexpected token in shader");
        }
    }

    if (material.numStages == 0)
    {
        iwStage_t stage;
        IW_SetStageDefaults(&stage);
        material.stages[0] = stage;
        material.numStages = 1;
    }

    IW_RegisterMaterial(&material);
}

static void IW_ParseShaderFile(const char *path)
{
    char *buffer;
    int length;
    if (!IWTXT_LoadFile(path, &buffer, &length))
        return;

    iwtxtParser_t parser;
    IWTXT_Init(&parser, buffer, length, path);

    iwtxtToken_t token;
    while (IWTXT_NextToken(&parser, &token))
    {
        if (token.type == IWTXT_TOKEN_EOF)
            break;
        if (token.type != IWTXT_TOKEN_STRING)
            continue;

        char keyword[32];
        IW_CopyTokenLower(&token, keyword, sizeof(keyword));
        if (!strcmp(keyword, "shader"))
        {
            if (!IWTXT_NextToken(&parser, &token))
                break;
            if (token.type != IWTXT_TOKEN_STRING)
            {
                IW_LogWarning(path, token.line, token.column, "shader name expected");
                continue;
            }
            IW_ParseShaderDefinition(&parser, &token, path);
        }
        else
        {
            IW_LogWarning(path, token.line, token.column, "unexpected token '%s'", keyword);
        }
    }

    IWTXT_Free(buffer);
}

void IW_ShaderSystem_Init(void)
{
    if (iw_initialized)
        return;

    Cvar_RegisterVariable(&r_iwshader_cvar);
    Cvar_RegisterVariable(&r_iwshader_strict_cvar);
    Cvar_RegisterVariable(&r_iwshader_debug_cvar);
    iwshader_dump_cmd = Cmd_AddCommand("r_iwshader_dump", IW_DumpMaterials_f);

    iw_num_materials = 0;
    iw_last_material = NULL;
    iw_last_texture[0] = '\0';
    iw_last_texture_display[0] = '\0';
    iw_initialized = true;
}

void IW_ShaderSystem_Shutdown(void)
{
    if (!iw_initialized)
        return;

    if (iwshader_dump_cmd)
    {
        Cmd_RemoveCommand(iwshader_dump_cmd);
        iwshader_dump_cmd = NULL;
    }

    iw_num_materials = 0;
    iw_last_material = NULL;
    iw_last_texture[0] = '\0';
    iw_last_texture_display[0] = '\0';
    iw_initialized = false;
}

void IW_LoadShaderDirectory(const char *dir)
{
    if (!dir || !*dir)
        return;

    findfile_t *find;
    char base[MAX_OSPATH];
    q_strlcpy(base, dir, sizeof(base));

    size_t len = strlen(base);
    if (len > 0 && base[len - 1] == '\\')
    {
        base[len - 1] = '\0';
        --len;
    }
    if (len > 0 && base[len - 1] != '/')
    {
        if (len + 1 < sizeof(base))
        {
            base[len] = '/';
            base[len + 1] = '\0';
            ++len;
        }
    }

    find = Sys_FindFirst(base, "iwshader");
    for (; find; find = Sys_FindNext(find))
    {
        if (find->attribs & FA_DIRECTORY)
            continue;

        char path[MAX_OSPATH];
        q_strlcpy(path, base, sizeof(path));
        q_strlcat(path, find->name, sizeof(path));
        IW_ParseShaderFile(path);
    }
}

const iwMaterial_t *IW_FindMaterial(const char *materialName)
{
    char normalized[IW_MAX_NAME];
    if (!IW_NormalizeName(materialName, normalized, sizeof(normalized), false))
        return NULL;

    for (int i = 0; i < iw_num_materials; ++i)
    {
        if (!strcmp(iw_materials[i].normalized, normalized))
            return &iw_materials[i].material;
    }
    return NULL;
}

static const iwMaterial_t *IW_FindMaterialNormalized(const char *name)
{
    for (int i = 0; i < iw_num_materials; ++i)
    {
        if (!strcmp(iw_materials[i].normalized, name))
            return &iw_materials[i].material;
    }
    return NULL;
}

const iwMaterial_t *IW_MaterialForTexture(const char *textureName)
{
    iw_last_material = NULL;
    iw_last_texture[0] = '\0';
    iw_last_texture_display[0] = '\0';

    if (!iw_initialized || !r_iwshader_cvar.value)
        return NULL;

    if (!textureName || !*textureName)
        return NULL;

    q_strlcpy(iw_last_texture, textureName, sizeof(iw_last_texture));

    char normalized[IW_MAX_NAME];
    if (IW_NormalizeName(textureName, normalized, sizeof(normalized), false))
    {
        iw_last_material = IW_FindMaterialNormalized(normalized);
    }

    if (!iw_last_material && IW_NormalizeName(textureName, normalized, sizeof(normalized), true))
    {
        iw_last_material = IW_FindMaterialNormalized(normalized);
    }

    if (IW_NormalizeName(textureName, iw_last_texture_display, sizeof(iw_last_texture_display), true))
        ;
    else
        q_strlcpy(iw_last_texture_display, textureName, sizeof(iw_last_texture_display));

    return iw_last_material;
}

void IW_DumpMaterials(const char *outPath)
{
    FILE *f = Sys_fopen(outPath, "w");
    if (!f)
    {
        Con_Printf("iwshader: could not open %s for writing\n", outPath);
        return;
    }

    for (int i = 0; i < iw_num_materials; ++i)
    {
        const iwMaterial_t *mat = &iw_materials[i].material;
        fprintf(f, "shader %s\n{\n", mat->name);
        if (mat->cull != IW_CULL_BACK)
        {
            const char *cull = mat->cull == IW_CULL_FRONT ? "front" : "none";
            fprintf(f, "    cull %s\n", cull);
        }
        if (mat->sort != IW_SORT_OPAQUE)
        {
            const char *sort = "opaque";
            switch (mat->sort)
            {
            case IW_SORT_ALPHA: sort = "alpha"; break;
            case IW_SORT_ADDITIVE: sort = "additive"; break;
            case IW_SORT_DECAL: sort = "decal"; break;
            case IW_SORT_SKY: sort = "sky"; break;
            default: break;
            }
            fprintf(f, "    sort %s\n", sort);
        }
        if (mat->strict)
            fprintf(f, "    strict 1\n");

        for (int s = 0; s < mat->numStages; ++s)
        {
            const iwStage_t *stage = &mat->stages[s];
            fprintf(f, "    stage {\n");
            fprintf(f, "        map %s\n", stage->mapPath);
            switch (stage->blendMode)
            {
            case IW_BLEND_ALPHA: fprintf(f, "        blend alpha\n"); break;
            case IW_BLEND_ADD: fprintf(f, "        blend add\n"); break;
            case IW_BLEND_MUL: fprintf(f, "        blend mul\n"); break;
            case IW_BLEND_PREMUL: fprintf(f, "        blend premul\n"); break;
            case IW_BLEND_ADD_ALPHA: fprintf(f, "        blend add_alpha\n"); break;
            case IW_BLEND_CUSTOM:
                if (stage->src == IW_SRC_SRC_ALPHA && stage->dst == IW_SRC_ONE)
                    fprintf(f, "        blend add_alpha\n");
                else
                    fprintf(f, "        blend %s %s\n", IW_BlendFactorName(stage->src), IW_BlendFactorName(stage->dst));
                break;
            default:
                break;
            }
            switch (stage->rgbgen)
            {
            case IW_RGB_VERTEX:
                fprintf(f, "        rgbgen vertex\n");
                break;
            case IW_RGB_CONST:
                fprintf(f, "        rgbgen const %g %g %g\n", stage->rgbConst[0], stage->rgbConst[1], stage->rgbConst[2]);
                break;
            case IW_RGB_IDENTITY:
                fprintf(f, "        rgbgen identity\n");
                break;
            case IW_RGB_ENTITY:
                fprintf(f, "        rgbgen entity\n");
                break;
            case IW_RGB_WAVE:
                fprintf(f, "        rgbgen wave %s %g %g %g %g\n", IW_WaveFuncName(stage->rgbWave.func), stage->rgbWave.base, stage->rgbWave.amplitude, stage->rgbWave.phase, stage->rgbWave.frequency);
                break;
            default:
                break;
            }
            switch (stage->alphagen)
            {
            case IW_A_VERTEX:
                fprintf(f, "        alphagen vertex\n");
                break;
            case IW_A_CONST:
                fprintf(f, "        alphagen const %g\n", stage->aConst);
                break;
            case IW_A_MASK:
                fprintf(f, "        alphagen mask\n");
                break;
            case IW_A_ENTITY:
                fprintf(f, "        alphagen entity\n");
                break;
            case IW_A_WAVE:
                fprintf(f, "        alphagen wave %s %g %g %g %g\n", IW_WaveFuncName(stage->alphaWave.func), stage->alphaWave.base, stage->alphaWave.amplitude, stage->alphaWave.phase, stage->alphaWave.frequency);
                break;
            }
            if (stage->mask != IW_CHANNEL_A)
                fprintf(f, "        mask %s\n", IW_ChannelName(stage->mask));
            if (stage->emissive)
                fprintf(f, "        emissive 1\n");
            if (stage->clamp)
                fprintf(f, "        clamp 1\n");
            for (int t = 0; t < stage->numTCMods; ++t)
            {
                const iwTCMod_t *tc = &stage->tcmods[t];
                switch (tc->op)
                {
                case IW_TC_SCROLL:
                    fprintf(f, "        tcmod scroll %g %g\n", tc->a, tc->b);
                    break;
                case IW_TC_SCALE:
                    fprintf(f, "        tcmod scale %g %g\n", tc->a, tc->b);
                    break;
                case IW_TC_ROTATE:
                    fprintf(f, "        tcmod rotate %g\n", tc->a);
                    break;
                case IW_TC_STRETCH:
                    fprintf(f, "        tcmod stretch %s %g %g %g %g\n", IW_WaveFuncName(tc->wave.func), tc->wave.base, tc->wave.amplitude, tc->wave.phase, tc->wave.frequency);
                    break;
                case IW_TC_TURB:
                    fprintf(f, "        tcmod turb %g %g\n", tc->a, tc->b);
                    break;
                case IW_TC_ENVMAP:
                    fprintf(f, "        tcmod envmap\n");
                    break;
                }
            }
            fprintf(f, "    }\n");
        }
        fprintf(f, "}\n\n");
    }

    fclose(f);
    Con_Printf("iwshader: wrote %d materials to %s\n", iw_num_materials, outPath);
}

static void IW_DumpMaterials_f(void)
{
    if (Cmd_Argc() < 2)
    {
        Con_Printf("usage: r_iwshader_dump <path>\n");
        return;
    }

    IW_DumpMaterials(Cmd_Argv(1));
}

const iwMaterial_t *IW_DebugLastMaterial(void)
{
    return iw_last_material;
}

const char *IW_DebugLastTexture(void)
{
    return iw_last_texture_display[0] ? iw_last_texture_display : NULL;
}

qboolean IW_DebugOverlayText(char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return false;
    if (!r_iwshader_debug_cvar.value)
        return false;
    const char *tex = IW_DebugLastTexture();
    if (!tex)
        return false;
    if (iw_last_material)
        q_snprintf(buffer, bufferSize, "iwshader: %s -> %s", tex, iw_last_material->name);
    else
        q_snprintf(buffer, bufferSize, "iwshader: %s (no material)", tex);
    return true;
}

