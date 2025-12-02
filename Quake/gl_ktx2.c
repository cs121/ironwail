#include "quakedef.h"
#include "gl_local.h"
#include "gl_ktx2.h"

/* Dummy constants */
static const uint8_t KTX2_MAGIC[12] = {0};
static const uint32_t KTX2_VERSION = 0;

typedef struct {
    int valid;
    int width;
    int height;
    int mip_count;
    int supercompression;
} ktx2_header_t;

static qboolean KTX2_ParseHeader(ktx2_header_t *out, const uint8_t *data, size_t size) {
    memset(out, 0, sizeof(*out));
    KTX2_LogInfo("KTX2_ParseHeader: stub");
    return false; // noch keine Implementierung
}

qboolean KTX2_IsValid(const uint8_t *data, size_t size)
{
    KTX2_LogInfo("KTX2_IsValid: stub");
    return false;
}

gltexture_t *R_LoadKTX2Texture(const char *name, const uint8_t *data, size_t size)
{
    KTX2_LogInfo("R_LoadKTX2Texture(%s): stub", name);
    return NULL;
}

void KTX2_LogInfo(const char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    Con_Printf("KTX2-INFO: ");
    Con_VPrintf(fmt, argptr);
    Con_Printf("\n");
    va_end(argptr);
}

void KTX2_LogError(const char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    Con_Printf("KTX2-ERROR: ");
    Con_VPrintf(fmt, argptr);
    Con_Printf("\n");
    va_end(argptr);
}

void R_TestKTX2(void)
{
    KTX2_LogInfo("R_TestKTX2: stub");
}

// In r_textures.c oder gl_model.c einbauen:
//
// if (COM_HasExtension(name, ".ktx2")) {
//     return R_LoadKTX2Texture(name, rawbuf, filesize);
// }
// (derzeit nur Stub, lädt nichts)
