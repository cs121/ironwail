/*
 * Texture atlas loading for maps
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "quakedef.h"
#include "q_ctype.h"
#include "texture_atlas.h"
#include "gl_model.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include "stb_dxt.h"

static qboolean atlas_enabled = false;
static GLuint atlas_gl_id = 0;
static gltexture_t atlas_gltexture;
static int atlas_width = 0;
static int atlas_height = 0;

cvar_t r_use_textureatlas = {"r_use_textureatlas", "1", CVAR_ARCHIVE};

typedef struct atlas_entry_s {
    char name[64];
    atlas_rect_t rect;
    qboolean logged;
    int offset_x;
    int offset_y;
} atlas_entry_t;

static atlas_entry_t atlas_entries[ATLAS_MAX_TEXTURES];
static int atlas_entry_count = 0;

static const atlas_rect_t atlas_null_rect = {0, 0, 0, 0, 0};
static qboolean atlas_missing_for_map = false;
static qboolean atlas_attempted_build = false;
static char atlas_missing_basename[MAX_QPATH];
static cvar_t atlas_use_original_dimensions = {"atlas_use_original_dimensions", "1", CVAR_NONE};

static void Atlas_OnUseChanged(cvar_t *var);
static void Atlas_BuildTextureAtlas_f(void);

static qboolean Atlas_ParseJSONBuffer(const char *json, size_t len);

extern cvar_t gl_fullbrights;
extern unsigned int d_8to24table_opaque[256];
extern unsigned int d_8to24table_alphabright[256];
extern unsigned int d_8to24table_fbright[256];
extern unsigned int d_8to24table_fbright_fence[256];
extern unsigned int d_8to24table_nobright[256];
extern unsigned int d_8to24table_nobright_fence[256];
extern unsigned int d_8to24table_conchars[256];

static void Atlas_NormalizeTextureName(const char *name, char *out, size_t out_size)
{
    size_t len;
    const char *start;
    const char *slash;
    char original[MAX_QPATH];

    if (!name)
    {
        out[0] = '\0';
        return;
    }

    q_strlcpy(original, name, sizeof(original));

    // ignore leading whitespace so atlas entries don't need to match stray padding
    for (start = name; q_isspace((int)*start); ++start)
        ;

    q_strlcpy(out, start, out_size);

    // normalize to lowercase immediately so extensions and directories match
    for (size_t i = 0; out[i]; ++i)
        out[i] = q_tolower(out[i]);

    // strip leading special characters like *water, +anim, -alternate
    while (out[0] == '*' || out[0] == '+' || out[0] == '-')
        memmove(out, out + 1, strlen(out));

    // strip directory components so atlas entries match basename
    slash = strrchr(out, '/');
    if (slash)
        memmove(out, slash + 1, strlen(slash + 1) + 1);

    // trim trailing whitespace
    len = strlen(out);
    while (len > 0 && q_isspace((int)out[len - 1]))
    {
        out[len - 1] = '\0';
        --len;
    }

    // strip common extensions
    if (len >= 4)
    {
        if (!q_strcasecmp(out + len - 4, ".tga") || !q_strcasecmp(out + len - 4, ".png") ||
            !q_strcasecmp(out + len - 4, ".wal") || !q_strcasecmp(out + len - 4, ".bmp"))
        {
            out[len - 4] = '\0';
        }
    }

    if (q_strcasecmp(original, out))
        Con_DPrintf("Atlas: normalized '%s' -> '%s'\n", original, out);
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
        Con_Printf("Texture atlas not loaded; falling back to legacy textures\n");
}

static bool Atlas_ReadFile(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *f;
    long len;
    size_t size;

    if (!path || !out_data || !out_size)
        return false;

    *out_data = NULL;
    *out_size = 0;

    f = fopen(path, "rb");
    if (!f)
        return false;

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }

    len = ftell(f);
    if (len < 0)
    {
        fclose(f);
        return false;
    }

    size = (size_t)len;

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }

    *out_data = (uint8_t *)malloc(size ? size : 1);
    if (!*out_data)
    {
        fclose(f);
        return false;
    }

    if (size > 0 && fread(*out_data, 1, size, f) != size)
    {
        free(*out_data);
        *out_data = NULL;
        fclose(f);
        return false;
    }

    fclose(f);
    *out_size = size;
    return true;
}

static const unsigned int *Atlas_SelectPalette(const gltexture_t *glt)
{
    if (glt->flags & TEXPREF_ALPHABRIGHT)
        return gl_fullbrights.value ? d_8to24table_alphabright : d_8to24table_opaque;

    if (glt->flags & TEXPREF_FULLBRIGHT)
        return (glt->flags & TEXPREF_ALPHA) ? d_8to24table_fbright_fence : d_8to24table_fbright;

    if ((glt->flags & TEXPREF_NOBRIGHT) && gl_fullbrights.value)
        return (glt->flags & TEXPREF_ALPHA) ? d_8to24table_nobright_fence : d_8to24table_nobright;

    if (glt->flags & TEXPREF_CONCHARS)
        return d_8to24table_conchars;

    return d_8to24table;
}

static void Atlas_AlphaEdgeFix(byte *data, int width, int height)
{
    int i, j, n = 0, b, c[3] = {0, 0, 0};
    int lastrow, thisrow, nextrow, lastpix, thispix, nextpix;
    byte *dest = data;

    if (!data)
        return;

    for (i = 0; i < height; i++)
    {
        lastrow = width * 4 * ((i == 0) ? height-1 : i-1);
        thisrow = width * 4 * i;
        nextrow = width * 4 * ((i == height-1) ? 0 : i+1);

        for (j = 0; j < width; j++, dest += 4)
        {
            if (dest[3])
                continue;

            n = b = 0;
            c[0] = c[1] = c[2] = 0;

            lastpix = thispix = nextpix = j * 4;
            if (j == 0)
                lastpix = (width-1) * 4;
            else
                lastpix -= 4;
            if (j == width-1)
                nextpix = 0;
            else
                nextpix += 4;

            if ((b = data[lastrow + lastpix + 3]))
            {
                c[0] += data[lastrow + lastpix + 0];
                c[1] += data[lastrow + lastpix + 1];
                c[2] += data[lastrow + lastpix + 2];
                n++;
            }
            if ((b = data[lastrow + thispix + 3]))
            {
                c[0] += data[lastrow + thispix + 0];
                c[1] += data[lastrow + thispix + 1];
                c[2] += data[lastrow + thispix + 2];
                n++;
            }
            if ((b = data[lastrow + nextpix + 3]))
            {
                c[0] += data[lastrow + nextpix + 0];
                c[1] += data[lastrow + nextpix + 1];
                c[2] += data[lastrow + nextpix + 2];
                n++;
            }
            if ((b = data[thisrow + lastpix + 3]))
            {
                c[0] += data[thisrow + lastpix + 0];
                c[1] += data[thisrow + lastpix + 1];
                c[2] += data[thisrow + lastpix + 2];
                n++;
            }
            if ((b = data[thisrow + nextpix + 3]))
            {
                c[0] += data[thisrow + nextpix + 0];
                c[1] += data[thisrow + nextpix + 1];
                c[2] += data[thisrow + nextpix + 2];
                n++;
            }
            if ((b = data[nextrow + lastpix + 3]))
            {
                c[0] += data[nextrow + lastpix + 0];
                c[1] += data[nextrow + lastpix + 1];
                c[2] += data[nextrow + lastpix + 2];
                n++;
            }
            if ((b = data[nextrow + thispix + 3]))
            {
                c[0] += data[nextrow + thispix + 0];
                c[1] += data[nextrow + thispix + 1];
                c[2] += data[nextrow + thispix + 2];
                n++;
            }
            if ((b = data[nextrow + nextpix + 3]))
            {
                c[0] += data[nextrow + nextpix + 0];
                c[1] += data[nextrow + nextpix + 1];
                c[2] += data[nextrow + nextpix + 2];
                n++;
            }

            if (!n)
                continue;

            dest[0] = (byte) (c[0] / n);
            dest[1] = (byte) (c[1] / n);
            dest[2] = (byte) (c[2] / n);
        }
    }
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

static void Atlas_OnUseChanged(cvar_t *var)
{
    if (var->value == 0.0f)
    {
        Atlas_Invalidate();
        return;
    }

    if (isDedicated)
        return;

    if (cl.worldmodel)
        Atlas_LoadForMap(cl.worldmodel->name);
}

static void Atlas_BuildTextureAtlas_f(void)
{
    if (isDedicated)
    {
        Con_Printf("Texture atlas building is unavailable on dedicated servers.\n");
        return;
    }

    if (!cl.worldmodel || !cl.worldmodel->name[0])
    {
        Con_Printf("No map is currently loaded.\n");
        return;
    }

    if (r_use_textureatlas.value == 0.0f)
        Con_Printf("r_use_textureatlas is 0; atlas will not be used until it is enabled.\n");

    Atlas_CreateFromFallbacks(cl.worldmodel);
}

void Atlas_Init(void)
{
    Cvar_RegisterVariable(&r_use_textureatlas);
    Cvar_SetCallback(&r_use_textureatlas, Atlas_OnUseChanged);
    Cvar_RegisterVariable(&atlas_use_original_dimensions);
    Cmd_AddCommand("r_buildtextureatlas", Atlas_BuildTextureAtlas_f);
    Atlas_Invalidate();
}

void Atlas_Invalidate(void)
{
    Atlas_DeleteGLTexture();
    Atlas_ClearEntries();
    atlas_width = atlas_height = 0;
    atlas_enabled = false;
    atlas_missing_for_map = false;
    atlas_attempted_build = false;
    atlas_missing_basename[0] = '\0';
}

static qboolean Atlas_ParseJSON(const char *json, size_t len)
{
    const char *end = json + len;
    const char *textures_block;
    const char *cursor;
    int width = 0, height = 0;
    int parsed_entries = 0;

    Con_Printf("Atlas_ParseJSON: starting (%zu bytes)\n", len);

    if (!Atlas_ParseIntField(json, end, "atlas_width", &width) || !Atlas_ParseIntField(json, end, "atlas_height", &height))
    {
        Con_Printf("Atlas_ParseJSON: missing atlas dimensions\n");
        return false;
    }

    if (width <= 0 || height <= 0)
    {
        Con_Printf("Atlas_ParseJSON: invalid atlas dimensions (%d x %d)\n", width, height);
        return false;
    }

    atlas_width = width;
    atlas_height = height;

    Con_DPrintf("Atlas_ParseJSON: atlas dimensions %d x %d\n", atlas_width, atlas_height);

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

        if (!Atlas_ParseStringField(obj_start, obj_end, "name", name, sizeof(name)))
        {
            Con_Printf("Atlas_ParseJSON: malformed texture entry (missing name)\n");
            cursor = obj_end + 1;
            continue;
        }

        if (!Atlas_ParseIntField(obj_start, obj_end, "x", &x) ||
            !Atlas_ParseIntField(obj_start, obj_end, "y", &y) ||
            !Atlas_ParseIntField(obj_start, obj_end, "w", &w) ||
            !Atlas_ParseIntField(obj_start, obj_end, "h", &h))
        {
            Con_Printf("Atlas_ParseJSON: malformed texture entry for '%s' (coords missing)\n", name);
            cursor = obj_end + 1;
            continue;
        }

        if (w <= 0 || h <= 0)
        {
            Con_Printf("Atlas_ParseJSON: invalid rect size for '%s' (%d x %d)\n", name, w, h);
            cursor = obj_end + 1;
            continue;
        }

        entry = &atlas_entries[atlas_entry_count++];
        Atlas_NormalizeTextureName(name, entry->name, sizeof(entry->name));

        // Option B: store origin + normalized size, not absolute end coordinates
        entry->rect.u1 = (float)x / (float)atlas_width;
        entry->rect.v1 = (float)y / (float)atlas_height;
        entry->rect.u2 = (float)w / (float)atlas_width;   // width_norm
        entry->rect.v2 = (float)h / (float)atlas_height;  // height_norm
        if (entry->rect.u2 == 0 || entry->rect.v2 == 0)
            Con_Printf("Atlas_ParseJSON: WARNING: zero-size rect for '%s' (u2=%f, v2=%f)\n", entry->name, entry->rect.u2, entry->rect.v2);
        entry->rect.exists = 1;
        entry->logged = false;

        entry->offset_x = 0;
        entry->offset_y = 0;
        Atlas_ParseIntField(obj_start, obj_end, "ox", &entry->offset_x);
        Atlas_ParseIntField(obj_start, obj_end, "oy", &entry->offset_y);

        ++parsed_entries;
        Con_DPrintf("Atlas_ParseJSON: added entry '%s' x=%d y=%d w=%d h=%d\n", entry->name, x, y, w, h);

        cursor = obj_end + 1;
    }

    Con_DPrintf("Atlas_ParseJSON: reached end of file\n");

    if (parsed_entries == 0)
    {
        Con_Printf("Atlas_ParseJSON: no valid textures parsed\n");
        return false;
    }

    Con_Printf("Atlas_ParseJSON: parsed %d entries\n", parsed_entries);
    return true;
}

static qboolean Atlas_ParseJSONBuffer(const char *json, size_t len)
{
    char *terminated;
    qboolean ok;

    if (!json || !len)
        return false;

    terminated = (char *)malloc(len + 1);
    if (!terminated)
        return false;

    memcpy(terminated, json, len);
    terminated[len] = '\0';

    ok = Atlas_ParseJSON(terminated, len);
    free(terminated);
    return ok;
}

static void Atlas_SetupGLTexture(GLuint texnum, int width, int height, int has_mips, qboolean compressed, const char *label)
{
    atlas_gl_id = texnum;
    atlas_width = width;
    atlas_height = height;

    glBindTexture(GL_TEXTURE_2D, atlas_gl_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    memset(&atlas_gltexture, 0, sizeof(atlas_gltexture));
    atlas_gltexture.target = GL_TEXTURE_2D;
    atlas_gltexture.texnum = atlas_gl_id;
    atlas_gltexture.width = (unsigned short)atlas_width;
    atlas_gltexture.height = (unsigned short)atlas_height;
    atlas_gltexture.depth = 1;
    atlas_gltexture.compression = compressed ? 1 : 0;
    atlas_gltexture.flags = TEXPREF_BINDLESS | TEXPREF_CLAMP;
    q_strlcpy(atlas_gltexture.name, "__atlas", sizeof(atlas_gltexture.name));
    atlas_gltexture.source_format = SRC_RGBA;
    atlas_gltexture.source_width = (unsigned int)atlas_width;
    atlas_gltexture.source_height = (unsigned int)atlas_height;

    if (gl_bindless_able && GL_GetTextureHandleARBFunc && GL_MakeTextureHandleResidentARBFunc)
    {
        atlas_gltexture.bindless_handle = GL_GetTextureHandleARBFunc(atlas_gl_id);
        GL_MakeTextureHandleResidentARBFunc(atlas_gltexture.bindless_handle);
    }

    Con_Printf("[ATLAS] Loaded DDS atlas: %s (%dx%d, mips=%d)\n", label ? label : atlas_gltexture.name, atlas_width, atlas_height, has_mips);
}

static qboolean Atlas_LoadJSON(const char *path)
{
    byte *json = NULL;
    long len;
    qboolean ok;

    json = COM_LoadMallocFile(path, NULL);
    if (!json)
    {
        Con_DPrintf("Atlas_LoadJSON: could not open %s\n", path);
        return false;
    }

    len = com_filesize;
    ok = Atlas_ParseJSONBuffer((const char *)json, (size_t)len);
    if (!ok)
        Con_Printf("Atlas_LoadJSON: failed to parse %s\n", path);
    free(json);
    return ok;
}

static qboolean Atlas_LoadDDS(const char *path)
{
    int has_mips = 0;

    atlas_gl_id = GL_LoadDDS(path, &atlas_width, &atlas_height, &has_mips);
    if (!atlas_gl_id)
    {
        Con_DPrintf("Atlas_LoadDDS: failed to load %s\n", path);
        return false;
    }

    Atlas_SetupGLTexture(atlas_gl_id, atlas_width, atlas_height, has_mips, true, path);

    return true;
}

static qboolean Atlas_LoadPNG(const char *path)
{
    int channels = 0;
    unsigned char *pixels;
    byte *data;
    long len;

    data = COM_LoadMallocFile(path, NULL);
    if (!data)
    {
        Con_DPrintf("Atlas_LoadPNG: failed to load %s\n", path);
        return false;
    }

    len = com_filesize;
    pixels = stbi_load_from_memory(data, (int)len, &atlas_width, &atlas_height, &channels, 4);
    free(data);

    if (!pixels)
    {
        Con_DPrintf("Atlas_LoadPNG: failed to decode %s\n", path);
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
    atlas_gltexture.depth = 1;
    atlas_gltexture.compression = 1;
    atlas_gltexture.flags = TEXPREF_BINDLESS | TEXPREF_CLAMP;
    q_strlcpy(atlas_gltexture.name, "__atlas", sizeof(atlas_gltexture.name));
    atlas_gltexture.source_format = SRC_RGBA;
    atlas_gltexture.source_width = (unsigned int)atlas_width;
    atlas_gltexture.source_height = (unsigned int)atlas_height;

    if (gl_bindless_able && GL_GetTextureHandleARBFunc && GL_MakeTextureHandleResidentARBFunc)
    {
        atlas_gltexture.bindless_handle = GL_GetTextureHandleARBFunc(atlas_gl_id);
        GL_MakeTextureHandleResidentARBFunc(atlas_gltexture.bindless_handle);
    }

    Con_DPrintf("Atlas_LoadPNG: loaded %s (%dx%d, channels=%d)\n", path, atlas_width, atlas_height, channels);

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
    char container_path[MAX_OSPATH];
    char png_path[MAX_OSPATH];
    char json_path[MAX_OSPATH];

    Atlas_Invalidate();

    if (isDedicated)
        return 0;

    if (r_use_textureatlas.value == 0.0f)
        return 0;

    Atlas_NormalizeMapName(mapname, basename, sizeof(basename));
    if (!basename[0])
        return 0;

    q_strlcpy(atlas_missing_basename, basename, sizeof(atlas_missing_basename));
    atlas_attempted_build = false;

    Con_Printf("Atlas_LoadForMap: checking '%s'\n", basename);

    q_snprintf(container_path, sizeof(container_path), "atlas/%s.atlas", basename);
    q_snprintf(png_path, sizeof(png_path), "atlas/%s_atlas.png", basename);
    q_snprintf(json_path, sizeof(json_path), "atlas/%s_atlas.json", basename);

    if (COM_FileExists(container_path, NULL))
    {
        AtlasInfo info;
        qboolean json_ok;

        Con_Printf("[ATLAS] Using container: %s\n", container_path);

        if (!LoadAtlasContainer(container_path, &info))
        {
            Con_Printf("Atlas: failed to load atlas container %s\n", container_path);
        }
        else
        {
            json_ok = Atlas_ParseJSONBuffer(info.json_blob, info.json_size);
            free(info.json_blob);

            if (!json_ok)
            {
                Con_Printf("Atlas: failed to parse JSON from container %s\n", container_path);
                GL_DeleteNativeTexture(info.texnum);
            }
            else
            {
                Atlas_SetupGLTexture(info.texnum, info.width, info.height, info.has_mips, true, container_path);
                atlas_missing_for_map = false;
                if (atlas_width <= 0 || atlas_height <= 0)
                {
                    Con_Printf("Atlas: ERROR: invalid atlas size (%d x %d); disabling atlas\n", atlas_width, atlas_height);
                    Atlas_Invalidate();
                    return 0;
                }
                atlas_enabled = true;
                Con_DPrintf("Atlas_LoadForMap: atlas ready (%dx%d, %d entries)\n", atlas_width, atlas_height, atlas_entry_count);
                return 1;
            }
        }

        // if container load failed, try legacy assets below
    }

    {
        qboolean png_exists = COM_FileExists(png_path, NULL);
        qboolean json_exists = COM_FileExists(json_path, NULL);
        Con_DPrintf("Atlas_LoadForMap: PNG %s\n", png_exists ? "found" : "missing");
        Con_DPrintf("Atlas_LoadForMap: JSON %s\n", json_exists ? "found" : "missing");

        if (png_exists && !json_exists)
        {
            findfile_t *find;
            char corrected[MAX_OSPATH];
            qboolean corrected_case = false;
            char expected[MAX_QPATH];

            q_snprintf(expected, sizeof(expected), "%s_atlas.json", basename);

			for (find = Sys_FindFirst("atlas", "json"); find; find = Sys_FindNext(find))
			{
				if (find->attribs & FA_DIRECTORY)
					continue;
				if (!q_strcasecmp(find->name, expected))
				{
					if (strcmp(find->name, expected))
					{
						q_snprintf(corrected, sizeof(corrected), "atlas/%s", find->name);
						Con_Printf("Atlas: case mismatch corrected: using '%s'\n", corrected);
						q_strlcpy(json_path, corrected, sizeof(json_path));
						corrected_case = true;
					}
					break;
				}
			}
			if (find)
				Sys_FindClose(find);

			if (!json_exists && corrected_case == false)
			{
				Con_Printf("Atlas: JSON missing but texture exists – INVALID ATLAS\n");
				Atlas_LogMissing();
				atlas_missing_for_map = false;
				return 0;
			}
		}
		else if (!png_exists && json_exists)
        {
            Con_Printf("Atlas: texture missing but JSON exists – INVALID ATLAS\n");
            Atlas_LogMissing();
            atlas_missing_for_map = false;
            return 0;
        }
        else if (!png_exists && !json_exists)
        {
            Con_Printf("Atlas: atlas files missing for '%s'\n", basename);
            Atlas_LogMissing();
            atlas_missing_for_map = true;
            return 0;
        }
    }

    Con_Printf("Atlas_LoadForMap: loading atlas for %s\n", basename);

    {
        qboolean png_loaded = false;
        qboolean json_loaded;

        if (COM_FileExists(png_path, NULL))
            png_loaded = Atlas_LoadPNG(png_path);

        json_loaded = Atlas_LoadJSON(json_path);

        if (!png_loaded || !json_loaded)
        {
            Atlas_Invalidate();
            Atlas_LogMissing();
            atlas_missing_for_map = (!png_loaded && !json_loaded);
            if (!atlas_missing_for_map)
                Con_Printf("Atlas: atlas load aborted due to partial failure (png=%d, json=%d)\n", png_loaded, json_loaded);
            return 0;
        }
    }

    atlas_missing_for_map = false;
    if (atlas_width <= 0 || atlas_height <= 0)
    {
        Con_Printf("Atlas: ERROR: invalid atlas size (%d x %d); disabling atlas\n", atlas_width, atlas_height);
        Atlas_Invalidate();
        return 0;
    }
    atlas_enabled = true;
    Con_DPrintf("Atlas_LoadForMap: atlas ready (%dx%d, %d entries)\n", atlas_width, atlas_height, atlas_entry_count);
    return 1;
}

typedef struct atlas_build_entry_s
{
    char name[64];
    int width, height;
    int x, y;
    int offset_x, offset_y;
    byte *rgba;
} atlas_build_entry_t;

static int Atlas_NextPowerOfTwo(int value)
{
    int result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

static byte *Atlas_CopyIndexed(const byte *src, int width, int height, const gltexture_t *glt)
{
    const unsigned int *palette = Atlas_SelectPalette(glt);
    size_t total = (size_t)width * height;
    byte *rgba = (byte *)malloc(total * 4);

    if (!rgba)
        return NULL;

    for (size_t i = 0; i < total; ++i)
    {
        unsigned int color = palette[src[i]];
        rgba[i * 4 + 0] = (byte)(color);
        rgba[i * 4 + 1] = (byte)(color >> 8);
        rgba[i * 4 + 2] = (byte)(color >> 16);
        rgba[i * 4 + 3] = (byte)(color >> 24);
    }

    if (glt->flags & TEXPREF_ALPHA)
        Atlas_AlphaEdgeFix(rgba, width, height);

    return rgba;
}

static byte *Atlas_CopyRGBA(const byte *src, int width, int height)
{
    size_t total = (size_t)width * height * 4;
    byte *rgba = (byte *)malloc(total);
    if (rgba)
        memcpy(rgba, src, total);
    return rgba;
}

static qboolean Atlas_SelectDimensions(const gltexture_t *glt, int *width, int *height, qboolean *using_gl_dims)
{
    qboolean prefer_source = atlas_use_original_dimensions.value != 0.0f;

    *using_gl_dims = false;

    if (prefer_source)
    {
        if (glt->source_width == 0 || glt->source_height == 0)
        {
            Con_Printf("Atlas: ERROR: missing source dimensions for %s\n", glt->name);
            return false;
        }

        *width = (int)glt->source_width;
        *height = (int)glt->source_height;
        return true;
    }

    *width = glt->width;
    *height = glt->height;
    *using_gl_dims = true;

    if (*width <= 0 || *height <= 0)
    {
        Con_Printf("Atlas: ERROR: invalid GL dimensions for %s (%d x %d)\n", glt->name, *width, *height);
        return false;
    }

    return true;
}

static byte *Atlas_ReadTextureSource(const gltexture_t *glt, int *width, int *height, enum srcformat *fmt_out, qboolean *used_gl_dims)
{
    int mark = Hunk_LowMark();
    byte *raw = NULL;
    enum srcformat fmt = glt->source_format;
    size_t bytes_per_pixel = (fmt == SRC_RGBA) ? 4 : 1;
    size_t expected_size;

    if (!Atlas_SelectDimensions(glt, width, height, used_gl_dims))
    {
        Hunk_FreeToLowMark(mark);
        return NULL;
    }

    if (*width <= 0 || *height <= 0)
    {
        Con_Printf("Atlas: ERROR: zero dimension for %s\n", glt->name);
        Hunk_FreeToLowMark(mark);
        return NULL;
    }

    expected_size = (size_t)(*width) * (*height) * bytes_per_pixel;

    if (glt->source_file[0] && glt->source_offset)
    {
        FILE *f;

        if (COM_FOpenFile(glt->source_file, &f, NULL) >= 0 && f && expected_size > 0)
        {
            raw = (byte *)malloc(expected_size);
            fseek(f, glt->source_offset, SEEK_CUR);
            if (!raw || fread(raw, 1, expected_size, f) != expected_size)
            {
                free(raw);
                raw = NULL;
                fclose(f);
            }
            fclose(f);
        }
        else
        {
            Con_Printf("Atlas: ERROR: failed to open source file %s\n", glt->source_file);
        }
    }
    else if (glt->source_file[0])
    {
        byte *loaded = Image_LoadImage(glt->source_file, width, height, &fmt);
        if (loaded)
        {
            expected_size = (size_t)(*width) * (*height) * ((fmt == SRC_RGBA) ? 4 : 1);
            raw = (byte *)malloc(expected_size);
            if (raw)
                memcpy(raw, loaded, expected_size);
        }
        else
        {
            Con_Printf("Atlas: ERROR: could not load %s for atlas\n", glt->source_file);
        }
    }
    else if (glt->source_offset)
    {
        const byte *src = (const byte *)glt->source_offset;

        /*
         * Brush textures keep their miptex header and mip levels together in
         * memory.  The base pixel data starts at offsets[0], so copy from that
         * location instead of the beginning of the struct to avoid treating
         * the header as image data.
         */
        if (glt->source_format == SRC_INDEXED)
        {
            const miptex_t *mt = (const miptex_t *)glt->source_offset;
            src += mt->offsets[0];
        }

        if (expected_size > 0)
        {
            raw = (byte *)malloc(expected_size);
            if (raw)
                memcpy(raw, src, expected_size);
        }
    }

    if (!raw)
    {
        Con_Printf("Atlas: ERROR: missing pixel data for %s\n", glt->name);
        Hunk_FreeToLowMark(mark);
        return NULL;
    }

    if (fmt_out)
        *fmt_out = fmt;

    Hunk_FreeToLowMark(mark);
    return raw;
}

static byte *Atlas_ExtractTexture(const gltexture_t *glt, int *width, int *height)
{
    enum srcformat fmt = SRC_INDEXED;
    qboolean used_gl_dims = false;
    byte *source = Atlas_ReadTextureSource(glt, width, height, &fmt, &used_gl_dims);
    byte *rgba = NULL;

    if (!source || *width <= 0 || *height <= 0)
    {
        free(source);
        return NULL;
    }

    if (glt->source_width && glt->source_height && ((int)glt->source_width != glt->width || (int)glt->source_height != glt->height))
        Con_Printf("Atlas: GL resampled %s; using disk dimensions %ux%u\n", glt->name, glt->source_width, glt->source_height);

    if (glt->width != *width || glt->height != *height)
        Con_Printf("Atlas: dimension mismatch: file=%dx%d vs GL=%dx%d for %s\n", *width, *height, glt->width, glt->height, glt->name);

    if (used_gl_dims)
        Con_Printf("Atlas: warning: using GL dimensions for %s (atlas_use_original_dimensions=0)\n", glt->name);

    Con_Printf("Atlas: %s (%dx%d src, %dx%d gl)\n", glt->name, *width, *height, glt->width, glt->height);

    if (fmt == SRC_RGBA)
        rgba = Atlas_CopyRGBA(source, *width, *height);
    else if (fmt == SRC_INDEXED)
        rgba = Atlas_CopyIndexed(source, *width, *height, glt);

    if (source != (byte *)glt->source_offset)
        free(source);

    return rgba;
}

static int Atlas_SortEntries(const void *a, const void *b)
{
    const atlas_build_entry_t *ea = (const atlas_build_entry_t *)a;
    const atlas_build_entry_t *eb = (const atlas_build_entry_t *)b;
    return eb->height - ea->height;
}

static int Atlas_FilterInvalidEntries(atlas_build_entry_t *entries, int count)
{
    int i, out = 0;

    for (i = 0; i < count; ++i)
    {
        if (!entries[i].rgba || entries[i].width <= 0 || entries[i].height <= 0)
        {
            Con_Printf("Atlas: ERROR: skipping invalid entry '%s' (w=%d h=%d data=%p)\n", entries[i].name, entries[i].width, entries[i].height, (void *)entries[i].rgba);
            free(entries[i].rgba);
            continue;
        }

        if (out != i)
            entries[out] = entries[i];
        ++out;
    }

    return out;
}

static qboolean Atlas_PackEntries(atlas_build_entry_t *entries, int count, int atlas_size)
{
    int x = 0, y = 0, row_height = 0;
    int i;

    for (i = 0; i < count; ++i)
    {
        atlas_build_entry_t *e = &entries[i];
        if (e->width > atlas_size || e->height > atlas_size)
            return false;

        if (x + e->width > atlas_size)
        {
            x = 0;
            y += row_height;
            row_height = 0;
        }

        if (y + e->height > atlas_size)
            return false;

        e->x = x;
        e->y = y;
        x += e->width;
        if (e->height > row_height)
            row_height = e->height;
    }

    return true;
}

static qboolean Atlas_WriteJSON(const char *path, const atlas_build_entry_t *entries, int count, int width, int height)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f)
        return false;

    fprintf(f, "{\n");
    fprintf(f, "    \"atlas_width\": %d,\n", width);
    fprintf(f, "    \"atlas_height\": %d,\n", height);
    fprintf(f, "    \"textures\": [\n");

    for (i = 0; i < count; ++i)
    {
        const atlas_build_entry_t *e = &entries[i];
        // Option B: JSON stays in pixels; normalization happens when parsing
        fprintf(f,
            "        {\"name\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"ox\":%d,\"oy\":%d}%s\n",
            e->name, e->x, e->y, e->width, e->height, e->offset_x, e->offset_y, (i == count - 1) ? "" : ",");
    }

    fprintf(f, "    ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

static qboolean Atlas_HasAlpha(const byte *data, int width, int height)
{
    size_t total = (size_t)width * height;

    for (size_t i = 0; i < total; ++i)
        if (data[i * 4 + 3] != 255)
            return true;

    return false;
}

typedef struct dds_pixelformat_s
{
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
} dds_pixelformat_t;

typedef struct dds_header_s
{
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    dds_pixelformat_t ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
} dds_header_t;

#define DDS_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

bool SaveAtlasAsDDS(const char *output_path, const uint8_t *rgba_pixels, int width, int height, bool has_alpha)
{
    static const uint32_t DDS_MAGIC = 0x20534444; // "DDS "
    static const uint32_t DDSD_CAPS = 0x1;
    static const uint32_t DDSD_HEIGHT = 0x2;
    static const uint32_t DDSD_WIDTH = 0x4;
    static const uint32_t DDSD_PIXELFORMAT = 0x1000;
    static const uint32_t DDSD_LINEARSIZE = 0x80000;
    static const uint32_t DDSCAPS_TEXTURE = 0x1000;
    static const uint32_t DDPF_FOURCC = 0x4;
    const uint32_t fourcc = has_alpha ? DDS_FOURCC('D','X','T','5') : DDS_FOURCC('D','X','T','1');
    const int block_size = has_alpha ? 16 : 8;
    const int blocks_x = (width + 3) / 4;
    const int blocks_y = (height + 3) / 4;
    const size_t compressed_size = (size_t)blocks_x * blocks_y * (size_t)block_size;
    uint8_t *compressed = NULL;
    FILE *f = NULL;

    if (!output_path || !rgba_pixels || width <= 0 || height <= 0)
        return false;

    compressed = (uint8_t *)malloc(compressed_size);
    if (!compressed)
        return false;

    for (int by = 0; by < blocks_y; ++by)
    {
        for (int bx = 0; bx < blocks_x; ++bx)
        {
            uint8_t block[16 * 4];

            for (int py = 0; py < 4; ++py)
            {
                for (int px = 0; px < 4; ++px)
                {
                    int sx = bx * 4 + px;
                    int sy = by * 4 + py;
                    uint8_t *dst = &block[(py * 4 + px) * 4];

                    if (sx < width && sy < height)
                    {
                        const uint8_t *src = &rgba_pixels[(size_t)(sy * width + sx) * 4];
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = src[3];
                    }
                    else
                    {
                        dst[0] = dst[1] = dst[2] = 0;
                        dst[3] = 255;
                    }
                }
            }

            stb_compress_dxt_block(compressed + ((by * blocks_x + bx) * block_size), block, has_alpha ? 1 : 0, STB_DXT_HIGHQUAL);
        }
    }

    dds_header_t header;
    memset(&header, 0, sizeof(header));
    header.size = LittleLong(sizeof(header));
    header.flags = LittleLong(DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE);
    header.height = LittleLong((uint32_t)height);
    header.width = LittleLong((uint32_t)width);
    header.pitchOrLinearSize = LittleLong((uint32_t)compressed_size);
    header.mipMapCount = LittleLong(1);
    header.ddspf.size = LittleLong((uint32_t)sizeof(header.ddspf));
    header.ddspf.flags = LittleLong(DDPF_FOURCC);
    header.ddspf.fourCC = LittleLong(fourcc);
    header.caps = LittleLong(DDSCAPS_TEXTURE);

    f = fopen(output_path, "wb");
    if (!f)
    {
        free(compressed);
        return false;
    }

    if (fwrite(&DDS_MAGIC, 1, sizeof(DDS_MAGIC), f) != sizeof(DDS_MAGIC)
        || fwrite(&header, 1, sizeof(header), f) != sizeof(header)
        || fwrite(compressed, 1, compressed_size, f) != compressed_size)
    {
        fclose(f);
        free(compressed);
        return false;
    }

    fclose(f);
    free(compressed);
    return true;
}

bool PackAtlasContainer(const char *atlas_path, const char *json_path, const char *dds_path)
{
    uint8_t *json_data = NULL;
    uint8_t *dds_data = NULL;
    size_t json_size = 0;
    size_t dds_size = 0;
    uint8_t header[16] = {0};
    uint32_t json_offset = (uint32_t)sizeof(header);
    uint32_t dds_offset;
    FILE *f = NULL;

    if (!atlas_path || !json_path || !dds_path)
        return false;

    if (!Atlas_ReadFile(json_path, &json_data, &json_size))
        goto fail;

    if (json_size > UINT32_MAX - json_offset)
        goto fail;

    dds_offset = json_offset + (uint32_t)json_size;

    if (!Atlas_ReadFile(dds_path, &dds_data, &dds_size))
        goto fail;

    if (dds_size > UINT32_MAX - dds_offset)
        goto fail;

    memcpy(header, "ATLAS01", 7);
    header[7] = 1; // version
    memcpy(header + 8, &json_offset, sizeof(json_offset));
    memcpy(header + 12, &dds_offset, sizeof(dds_offset));

    f = fopen(atlas_path, "wb");
    if (!f)
        goto fail;

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header))
        goto fail;

    if (json_size && fwrite(json_data, 1, json_size, f) != json_size)
        goto fail;

    if (dds_size && fwrite(dds_data, 1, dds_size, f) != dds_size)
        goto fail;

    fclose(f);
    free(json_data);
    free(dds_data);
    return true;

fail:
    if (f)
        fclose(f);
    free(json_data);
    free(dds_data);
    return false;
}

bool LoadAtlasContainer(const char *atlas_path, AtlasInfo *out)
{
    uint8_t *data = NULL;
    uint8_t *dds_copy = NULL;
    char *json_copy = NULL;
    size_t size = 0;
    uint32_t json_offset = 0;
    uint32_t dds_offset = 0;
    size_t json_size;
    size_t dds_size;
    GLuint texnum = 0;
    int width = 0;
    int height = 0;
    int has_mips = 0;
    bool ok = false;

    if (!atlas_path || !out)
        return false;

    memset(out, 0, sizeof(*out));

    if (!Atlas_ReadFile(atlas_path, &data, &size))
        return false;

    if (size < 16)
        goto fail;

    if (memcmp(data, "ATLAS01", 7) != 0 || data[7] != 1)
        goto fail;

    memcpy(&json_offset, data + 8, sizeof(json_offset));
    memcpy(&dds_offset, data + 12, sizeof(dds_offset));

    json_offset = LittleLong(json_offset);
    dds_offset = LittleLong(dds_offset);

    if (json_offset < 16 || json_offset > size || dds_offset > size || json_offset > dds_offset)
        goto fail;

    json_size = (size_t)(dds_offset - json_offset);
    dds_size = size - dds_offset;

    json_copy = (char *)malloc(json_size + 1);
    if (!json_copy)
        goto fail;

    if (json_size)
        memcpy(json_copy, data + json_offset, json_size);
    json_copy[json_size] = '\0';

    dds_copy = (uint8_t *)malloc(dds_size ? dds_size : 1);
    if (!dds_copy)
        goto fail;

    if (dds_size)
        memcpy(dds_copy, data + dds_offset, dds_size);

    texnum = GL_LoadDDS_Memory(dds_copy, dds_size, &width, &height, &has_mips);

    if (!texnum)
        goto fail;

    out->json_blob = json_copy;
    out->json_size = json_size;
    out->texnum = texnum;
    out->width = width;
    out->height = height;
    out->has_mips = has_mips;
    ok = true;

fail:
    free(dds_copy);
    free(data);
    if (!ok)
    {
        free(json_copy);
    }

    return ok;
}

void Atlas_CreateFromFallbacks(qmodel_t *model)
{
    atlas_build_entry_t *entries = NULL;
    int entry_count = 0;
    int max_dim = 0;
    int atlas_size;
    int i;
    char basename[MAX_QPATH];
    char dds_path[MAX_OSPATH];
    char json_path[MAX_OSPATH];
    byte *atlas_pixels = NULL;
    qboolean has_alpha = false;

    if (atlas_attempted_build || !model || isDedicated)
        return;

    Atlas_NormalizeMapName(model->name, basename, sizeof(basename));
    if (!atlas_missing_basename[0])
        q_strlcpy(atlas_missing_basename, basename, sizeof(atlas_missing_basename));
    else if (q_strcasecmp(basename, atlas_missing_basename))
        return;

    atlas_attempted_build = true;

    q_snprintf(dds_path, sizeof(dds_path), "%s/atlas/%s_atlas.dds", com_gamedir, basename);
    q_snprintf(json_path, sizeof(json_path), "%s/atlas/%s_atlas.json", com_gamedir, basename);

    if (COM_FileExists(dds_path, NULL) || COM_FileExists(json_path, NULL))
    {
        Con_Printf("Atlas_CreateFromFallbacks: skipping build; atlas already exists for '%s'\n", basename);
        return;
    }

    Con_Printf("Atlas_CreateFromFallbacks: building fallback atlas for '%s'\n", basename);

    entries = (atlas_build_entry_t *)calloc((size_t)model->numtextures, sizeof(*entries));
    if (!entries)
        return;

    for (i = 0; i < model->numtextures; ++i)
    {
        texture_t *tx = model->textures[i];
        gltexture_t *glt;
        char normalized[sizeof(entries->name)];

        if (!tx || !tx->name[0])
            continue;

        if (i >= model->numtextures - 2)
            continue;

        glt = tx->gltexture;
        if (!glt)
            continue;

        Atlas_NormalizeTextureName(tx->name, normalized, sizeof(normalized));
        if (!normalized[0])
            continue;

        // skip duplicates
        {
            int j;
            qboolean duplicate = false;
            for (j = 0; j < entry_count; ++j)
            {
                if (!q_strcasecmp(entries[j].name, normalized))
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
        }

        entries[entry_count].rgba = Atlas_ExtractTexture(glt, &entries[entry_count].width, &entries[entry_count].height);
        if (!entries[entry_count].rgba)
            continue;

        q_strlcpy(entries[entry_count].name, normalized, sizeof(entries[entry_count].name));
        entries[entry_count].x = entries[entry_count].y = 0;
        max_dim = q_max(max_dim, entries[entry_count].width);
        max_dim = q_max(max_dim, entries[entry_count].height);
        ++entry_count;

        if (entry_count >= ATLAS_MAX_TEXTURES)
            break;
    }

    if (entry_count == 0 || max_dim == 0)
    {
        Con_Printf("Atlas_CreateFromFallbacks: no entries found for '%s'\n", basename);
        goto cleanup;
    }

    entry_count = Atlas_FilterInvalidEntries(entries, entry_count);

    if (entry_count == 0)
    {
        Con_Printf("Atlas_CreateFromFallbacks: all entries invalid for '%s'\n", basename);
        goto cleanup;
    }

    max_dim = 0;
    for (i = 0; i < entry_count; ++i)
    {
        max_dim = q_max(max_dim, entries[i].width);
        max_dim = q_max(max_dim, entries[i].height);
    }

    qsort(entries, entry_count, sizeof(*entries), Atlas_SortEntries);

    atlas_size = Atlas_NextPowerOfTwo(max_dim);
    while (!Atlas_PackEntries(entries, entry_count, atlas_size))
    {
        if (atlas_size >= gl_max_texture_size)
        {
            Con_Printf("Failed to create atlas for %s: textures too large (>%d)\n", basename, gl_max_texture_size);
            goto cleanup;
        }
        atlas_size <<= 1;
    }

    atlas_pixels = (byte *)calloc((size_t)atlas_size * atlas_size * 4, 1);
    if (!atlas_pixels)
    {
        Con_Printf("Atlas_CreateFromFallbacks: failed to allocate atlas pixels for '%s'\n", basename);
        goto cleanup;
    }

    for (i = 0; i < entry_count; ++i)
    {
        atlas_build_entry_t *e = &entries[i];
        int row;
        for (row = 0; row < e->height; ++row)
        {
            byte *dst = atlas_pixels + ((size_t)(e->y + row) * atlas_size + e->x) * 4;
            const byte *src = e->rgba + (size_t)row * e->width * 4;
            memcpy(dst, src, (size_t)e->width * 4);
        }
    }

    COM_CreatePath(dds_path);
    COM_CreatePath(json_path);

    has_alpha = Atlas_HasAlpha(atlas_pixels, atlas_size, atlas_size);

    if (!SaveAtlasAsDDS(dds_path, atlas_pixels, atlas_size, atlas_size, has_alpha))
    {
        Con_Printf("Failed to write atlas texture to %s\n", dds_path);
        goto cleanup;
    }

    if (!Atlas_WriteJSON(json_path, entries, entry_count, atlas_size, atlas_size))
    {
        Con_Printf("Failed to write atlas description to %s\n", json_path);
        goto cleanup;
    }

    Con_Printf("Created fallback atlas for %s with %d textures (%dx%d)\n", basename, entry_count, atlas_size, atlas_size);
    atlas_missing_for_map = false;
    Atlas_LoadForMap(model->name);

cleanup:
    if (entries)
    {
        for (i = 0; i < entry_count; ++i)
            free(entries[i].rgba);
        free(entries);
    }

    free(atlas_pixels);
}

atlas_rect_t Atlas_GetUV(const char *name)
{
    int i;
    char normalized[sizeof(atlas_entries[0].name)];
    static qboolean missing_warned = false;

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
            if (atlas_entries[i].rect.u2 == 0 || atlas_entries[i].rect.v2 == 0)
                Con_Printf("Atlas: ERROR: zero-size UV rect for '%s' \xe2\x86\x92 stretching likely\n", normalized);
            return atlas_entries[i].rect;
        }
    }

    {
        if (!missing_warned)
        {
            Con_Printf("Atlas: warning: texture '%s' missing (UV=0, fallback)\n", normalized);
            missing_warned = true;
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

