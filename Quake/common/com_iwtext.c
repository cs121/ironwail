#include "quakedef.h"
#include "com_iwtext.h"

static void IWTXT_SkipWhitespace(iwtxtParser_t *parser)
{
    while (parser->cursor < parser->end)
    {
        char c = *parser->cursor;
        if (c == '\r')
        {
            parser->cursor++;
            continue;
        }
        if (c == '\n')
        {
            parser->cursor++;
            parser->line++;
            parser->column = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v')
        {
            parser->cursor++;
            parser->column++;
            continue;
        }
        if (c == '#')
        {
            char next = (parser->cursor + 1) < parser->end ? parser->cursor[1] : '\0';
            if (!next || q_isspace((unsigned char) next))
            {
                while (parser->cursor < parser->end && *parser->cursor != '\n')
                    parser->cursor++;
                continue;
            }
        }
        if (c == '/')
        {
            if ((parser->cursor + 1) < parser->end && parser->cursor[1] == '/')
            {
                parser->cursor += 2;
                while (parser->cursor < parser->end && *parser->cursor != '\n')
                    parser->cursor++;
                continue;
            }
        }
        break;
    }
}

static qboolean IWTXT_ParseNumber(const char *text, size_t len, double *out)
{
    char temp[64];
    size_t copy = q_min(len, sizeof(temp) - 1);
    memcpy(temp, text, copy);
    temp[copy] = '\0';

    char *endptr;
    double value = strtod(temp, &endptr);
    if (endptr == temp || *endptr != '\0')
        return false;

    *out = value;
    return true;
}

qboolean IWTXT_LoadFile(const char *path, char **outBuf, int *outLen)
{
    byte *data = COM_LoadMallocFile(path, NULL);

    if (!data)
    {
        data = COM_LoadMallocFile_TextMode_OSPath(path, NULL);
        if (!data)
            return false;
    }

    if (outLen)
        *outLen = (int)strlen((const char *)data);

    *outBuf = (char *)data;
    return true;
}

void IWTXT_Free(void *p)
{
    if (p)
        free(p);
}

void IWTXT_Init(iwtxtParser_t *parser, const char *buffer, int length, const char *filename)
{
    parser->filename = filename;
    parser->buffer = buffer;
    parser->cursor = buffer;
    parser->end = buffer + length;
    parser->line = 1;
    parser->column = 1;
    parser->has_peek = false;
}

static qboolean IWTXT_InternalNext(iwtxtParser_t *parser, iwtxtToken_t *out)
{
    IWTXT_SkipWhitespace(parser);
    if (parser->cursor >= parser->end || *parser->cursor == '\0')
    {
        if (out)
        {
            out->type = IWTXT_TOKEN_EOF;
            out->text = parser->cursor;
            out->length = 0;
            out->number = 0.0;
            out->line = parser->line;
            out->column = parser->column;
        }
        return false;
    }

    const char *start = parser->cursor;
    int line = parser->line;
    int column = parser->column;
    char c = *parser->cursor;

    if (c == '{' || c == '}' || c == '(' || c == ')')
    {
        parser->cursor++;
        parser->column++;
        if (out)
        {
            out->type = IWTXT_TOKEN_SYMBOL;
            out->text = start;
            out->length = 1;
            out->number = 0.0;
            out->line = line;
            out->column = column;
        }
        return true;
    }

    if (c == '"')
    {
        parser->cursor++;
        parser->column++;
        start = parser->cursor;
        while (parser->cursor < parser->end && *parser->cursor != '"' && *parser->cursor != '\n')
        {
            parser->cursor++;
            parser->column++;
        }
        const char *end = parser->cursor;
        if (parser->cursor < parser->end && *parser->cursor == '"')
        {
            parser->cursor++;
            parser->column++;
        }
        if (out)
        {
            out->type = IWTXT_TOKEN_STRING;
            out->text = start;
            out->length = (size_t)(end - start);
            out->number = 0.0;
            out->line = line;
            out->column = column;
        }
        return true;
    }

    while (parser->cursor < parser->end)
    {
        c = *parser->cursor;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ' || c == '\f' || c == '\v' || c == '{' || c == '}')
            break;
        if (c == '/')
        {
            if ((parser->cursor + 1) < parser->end && parser->cursor[1] == '/')
                break;
        }
        parser->cursor++;
        parser->column++;
    }

    size_t len = (size_t)(parser->cursor - start);
    if (out)
    {
        out->text = start;
        out->length = len;
        out->line = line;
        out->column = column;
        if (IWTXT_ParseNumber(start, len, &out->number))
            out->type = IWTXT_TOKEN_NUMBER;
        else
            out->type = IWTXT_TOKEN_STRING;
    }
    return true;
}

qboolean IWTXT_NextToken(iwtxtParser_t *parser, iwtxtToken_t *out)
{
    if (parser->has_peek)
    {
        if (out)
            *out = parser->peek;
        parser->has_peek = false;
        return out ? (out->type != IWTXT_TOKEN_EOF) : (parser->peek.type != IWTXT_TOKEN_EOF);
    }
    return IWTXT_InternalNext(parser, out);
}

qboolean IWTXT_PeekToken(iwtxtParser_t *parser, iwtxtToken_t *out)
{
    if (!parser->has_peek)
    {
        iwtxtToken_t tmp;
        if (!IWTXT_InternalNext(parser, &tmp))
        {
            if (out)
            {
                out->type = IWTXT_TOKEN_EOF;
                out->text = parser->end;
                out->length = 0;
                out->number = 0.0;
                out->line = parser->line;
                out->column = parser->column;
            }
            return false;
        }
        parser->peek = tmp;
        parser->has_peek = true;
    }
    if (out)
        *out = parser->peek;
    return true;
}

