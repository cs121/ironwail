/*
 * Texture atlas loading for maps
 */

#include "quakedef.h"
#include "texture_atlas.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb_image.h"

static qboolean atlas_enabled = false;
static GLuint atlas_gl_id = 0;
static gltexture_t atlas_gltexture;
static int atlas_width = 0;
static int atlas_height = 0;

typedef struct atlas_entry_s {
    char name[64];
    atlas_rect_t rect;
    qboolean logged;
} atlas_entry_t;

static atlas_entry_t atlas_entries[ATLAS_MAX_TEXTURES];
static int atlas_entry_count = 0;

static const atlas_rect_t atlas_null_rect = {0, 0, 0, 0, 0};

static void Atlas_NormalizeTextureName(const char *name, char *out, size_t out_size)
{
    size_t len;

    q_strlcpy(out, name ? name : "", out_size);

    len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r' || out[len - 1] == '\n'))
    {
        out[len - 1] = '\0';
        --len;
    }
}

static const char *Atlas_SkipWhitespace(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
        ++p;
    return p;
}

static const char *Atlas_FindInRange(const char *start, const char *end, const char *needle)
{
    size_t len = strlen(needle);
    const char *p;

    if (end <= start || len == 0)
        return NULL;

    for (p = start; p + len <= end; ++p)
    {
        if (memcmp(p, needle, len) == 0)
            return p;
    }

    return NULL;
}

static qboolean Atlas_ParseIntField(const char *obj_start, const char *obj_end, const char *key, int *out)
{
    char pattern[64];
    const char *pos;

    q_snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    pos = Atlas_FindInRange(obj_start, obj_end, pattern);
    if (!pos)
        return false;

    pos = strchr(pos + strlen(pattern), ':');
    if (!pos || pos >= obj_end)
        return false;

    pos = Atlas_SkipWhitespace(pos + 1, obj_end);
    *out = (int)strtol(pos, NULL, 10);
    return true;
}

static qboolean Atlas_ParseStringField(const char *obj_start, const char *obj_end, const char *key, char *out, size_t out_size)
{
    char pattern[64];
    const char *pos;
    const char *value_start;
    const char *value_end;

    q_snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    pos = Atlas_FindInRange(obj_start, obj_end, pattern);
    if (!pos)
        return false;

    pos = strchr(pos + strlen(pattern), ':');
    if (!pos || pos >= obj_end)
        return false;

    value_start = strchr(pos + 1, '\"');
    if (!value_start || value_start >= obj_end)
        return false;
    ++value_start;

    value_end = strchr(value_start, '\"');
    if (!value_end || value_end > obj_end)
        return false;

    q_strlcpy(out, value_start, q_min((size_t)(value_end - value_start) + 1, out_size));
    return true;
}

static void Atlas_ClearEntries(void)
{
    atlas_entry_count = 0;
    memset(atlas_entries, 0, sizeof(atlas_entries));
}

static void Atlas_LogMissing(void)
{
    if (!atlas_enabled)
        Con_DPrintf("Texture atlas not loaded; falling back to legacy textures\n");
}

static void Atlas_DeleteGLTexture(void)
{
    if (atlas_gltexture.bindless_handle && GL_MakeTextureHandleNonResidentARBFunc)
    {
        GL_MakeTextureHandleNonResidentARBFunc(atlas_gltexture.bindless_handle);
        atlas_gltexture.bindless_handle = 0;
    }

    if (atlas_gl_id)
    {
        GL_DeleteNativeTexture(atlas_gl_id);
        atlas_gl_id = 0;
    }
    memset(&atlas_gltexture, 0, sizeof(atlas_gltexture));
}

void Atlas_Init(void)
{
    Atlas_Invalidate();
}

void Atlas_Invalidate(void)
{
    Atlas_DeleteGLTexture();
    Atlas_ClearEntries();
    atlas_width = atlas_height = 0;
    atlas_enabled = false;
}

static qboolean Atlas_ParseJSON(const char *json, size_t len)
{
    const char *end = json + len;
    const char *textures_block;
    const char *cursor;
    int width = 0, height = 0;

    if (!Atlas_ParseIntField(json, end, "atlas_width", &width) || !Atlas_ParseIntField(json, end, "atlas_height", &height))
    {
        Con_Printf("Atlas_ParseJSON: missing atlas dimensions\n");
        return false;
    }

    atlas_width = width;
    atlas_height = height;

    textures_block = Atlas_FindInRange(json, end, "\"textures\"");
    if (!textures_block)
    {
        Con_Printf("Atlas_ParseJSON: missing textures array\n");
        return false;
    }

    cursor = strchr(textures_block, '[');
    if (!cursor)
    {
        Con_Printf("Atlas_ParseJSON: malformed textures array\n");
        return false;
    }

    while (cursor && cursor < end)
    {
        const char *obj_start = strchr(cursor, '{');
        const char *obj_end;
        atlas_entry_t *entry;
        int x, y, w, h;
        char name[64];

        if (!obj_start)
            break;

        obj_end = strchr(obj_start, '}');
        if (!obj_end)
            break;

        if (atlas_entry_count >= ATLAS_MAX_TEXTURES)
        {
            Con_Printf("Atlas_ParseJSON: too many atlas entries (max %d)\n", ATLAS_MAX_TEXTURES);
            return false;
        }

        if (!Atlas_ParseStringField(obj_start, obj_end, "name", name, sizeof(name)) ||
            !Atlas_ParseIntField(obj_start, obj_end, "x", &x) ||
            !Atlas_ParseIntField(obj_start, obj_end, "y", &y) ||
            !Atlas_ParseIntField(obj_start, obj_end, "w", &w) ||
            !Atlas_ParseIntField(obj_start, obj_end, "h", &h))
        {
            Con_Printf("Atlas_ParseJSON: malformed texture entry\n");
            return false;
        }

        entry = &atlas_entries[atlas_entry_count++];
        Atlas_NormalizeTextureName(name, entry->name, sizeof(entry->name));
        entry->rect.u1 = (float)x / (float)atlas_width;
        entry->rect.v1 = (float)y / (float)atlas_height;
        entry->rect.u2 = (float)(x + w) / (float)atlas_width;
        entry->rect.v2 = (float)(y + h) / (float)atlas_height;
        entry->rect.exists = 1;
        entry->logged = false;

        cursor = obj_end + 1;
    }

    return atlas_entry_count > 0;
}

static qboolean Atlas_LoadJSON(const char *path)
{
    long len = 0;
    char *json = (char *)COM_LoadMallocFile_TextMode_OSPath(path, &len);
    qboolean ok;

    if (!json)
    {
        Con_DPrintf("Atlas_LoadJSON: could not open %s\n", path);
        return false;
    }

    ok = Atlas_ParseJSON(json, (size_t)len);
    free(json);
    return ok;
}

static qboolean Atlas_LoadPNG(const char *path)
{
    int channels = 0;
    unsigned char *pixels;

    pixels = stbi_load(path, &atlas_width, &atlas_height, &channels, 4);
    if (!pixels)
    {
        Con_DPrintf("Atlas_LoadPNG: failed to load %s\n", path);
        return false;
    }

    glGenTextures(1, &atlas_gl_id);
    glBindTexture(GL_TEXTURE_2D, atlas_gl_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_width, atlas_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    free(pixels);

    memset(&atlas_gltexture, 0, sizeof(atlas_gltexture));
    atlas_gltexture.target = GL_TEXTURE_2D;
    atlas_gltexture.texnum = atlas_gl_id;
    atlas_gltexture.width = (unsigned short)atlas_width;
    atlas_gltexture.height = (unsigned short)atlas_height;
    atlas_gltexture.flags = TEXPREF_BINDLESS;

    if (gl_bindless_able && GL_GetTextureHandleARBFunc && GL_MakeTextureHandleResidentARBFunc)
    {
        atlas_gltexture.bindless_handle = GL_GetTextureHandleARBFunc(atlas_gl_id);
        GL_MakeTextureHandleResidentARBFunc(atlas_gltexture.bindless_handle);
    }

    return true;
}

static void Atlas_NormalizeMapName(const char *mapname, char *out, size_t out_size)
{
    char base[MAX_QPATH];
    if (!mapname)
    {
        out[0] = '\0';
        return;
    }

    COM_FileBase(mapname, base, sizeof(base));
    q_strlcpy(out, base, out_size);
}

int Atlas_LoadForMap(const char *mapname)
{
    char basename[MAX_QPATH];
    char png_path[MAX_OSPATH];
    char json_path[MAX_OSPATH];

    Atlas_Invalidate();

    if (isDedicated)
        return 0;

    Atlas_NormalizeMapName(mapname, basename, sizeof(basename));
    if (!basename[0])
        return 0;

    q_snprintf(png_path, sizeof(png_path), "%s/atlas/%s_atlas.png", com_gamedir, basename);
    q_snprintf(json_path, sizeof(json_path), "%s/atlas/%s_atlas.json", com_gamedir, basename);

    if (!Sys_FileExists(png_path) || !Sys_FileExists(json_path))
    {
        Atlas_LogMissing();
        return 0;
    }

    Con_Printf("Atlas_LoadForMap: loading atlas for %s\n", basename);

    if (!Atlas_LoadPNG(png_path) || !Atlas_LoadJSON(json_path))
    {
        Atlas_Invalidate();
        return 0;
    }

    atlas_enabled = true;
    return 1;
}

atlas_rect_t Atlas_GetUV(const char *name)
{
    int i;
    char normalized[sizeof(atlas_entries[0].name)];

    if (!atlas_enabled || !name)
        return atlas_null_rect;

    Atlas_NormalizeTextureName(name, normalized, sizeof(normalized));
    if (!normalized[0])
        return atlas_null_rect;

    for (i = 0; i < atlas_entry_count; i++)
    {
        if (!q_strcasecmp(atlas_entries[i].name, normalized))
        {
            if (!atlas_entries[i].logged)
            {
                Con_Printf("Atlas: mapped texture %s to atlas entry\n", normalized);
                atlas_entries[i].logged = true;
            }
            return atlas_entries[i].rect;
        }
    }

    return atlas_null_rect;
}

int Atlas_TextureExists(const char *name)
{
    atlas_rect_t r = Atlas_GetUV(name);
    return r.exists;
}

GLuint Atlas_GetGLTexture(void)
{
    if (!atlas_enabled)
        return 0;
    return atlas_gl_id;
}

const gltexture_t *Atlas_GetGLTextureStruct(void)
{
    if (!atlas_enabled)
        return NULL;
    return &atlas_gltexture;
}

