#pragma once

#include "q_stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IWTXT_TOKEN_EOF = 0,
    IWTXT_TOKEN_STRING,
    IWTXT_TOKEN_NUMBER,
    IWTXT_TOKEN_SYMBOL
} iwtxtTokenType_t;

typedef struct {
    iwtxtTokenType_t type;
    const char *text;
    size_t length;
    double number;
    int line;
    int column;
} iwtxtToken_t;

typedef struct {
    const char *filename;
    const char *buffer;
    const char *cursor;
    const char *end;
    int line;
    int column;
    qboolean has_peek;
    iwtxtToken_t peek;
} iwtxtParser_t;

qboolean IWTXT_LoadFile(const char *path, char **outBuf, int *outLen);
void IWTXT_Free(void *p);

void IWTXT_Init(iwtxtParser_t *parser, const char *buffer, int length, const char *filename);
qboolean IWTXT_NextToken(iwtxtParser_t *parser, iwtxtToken_t *out);
qboolean IWTXT_PeekToken(iwtxtParser_t *parser, iwtxtToken_t *out);

#ifdef __cplusplus
}
#endif

