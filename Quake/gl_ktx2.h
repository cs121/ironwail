#ifndef GL_KTX2_H
#define GL_KTX2_H

#include <stddef.h>
#include <stdint.h>

#include "q_stdinc.h"

/* Forward declaration */
typedef struct gltexture_s gltexture_t;

qboolean KTX2_IsValid(const uint8_t *data, size_t size);
gltexture_t *R_LoadKTX2Texture(const char *name, const uint8_t *data, size_t size);
void KTX2_LogInfo(const char *fmt, ...);
void KTX2_LogError(const char *fmt, ...);
void R_TestKTX2(void);

#endif /* GL_KTX2_H */
