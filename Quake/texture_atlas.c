/*
 * Texture atlas loading for maps
 */

#include "quakedef.h"
#include "q_ctype.h"
#include "texture_atlas.h"
#include "gl_model.h"

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
static qboolean atlas_missing_for_map = false;
static qboolean atlas_attempted_build = false;
static char atlas_missing_basename[MAX_QPATH];

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

    if (!name)
    {
        out[0] = '\0';
        return;
    }

    // ignore leading whitespace so atlas entries don't need to match stray padding
    for (start = name; *start == ' ' || *start == '\t' || *start == '\r' || *start == '\n'; ++start)
        ;

    q_strlcpy(out, start, out_size);

    // trim trailing whitespace
    len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r' || out[len - 1] == '\n'))
    {
        out[len - 1] = '\0';
        --len;
    }

    // normalize to lowercase so atlas lookups are consistent on case-sensitive filesystems
    for (size_t i = 0; i < len; ++i)
        out[i] = q_tolower(out[i]);
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
    ok = Atlas_ParseJSON((char *)json, (size_t)len);
    free(json);
    return ok;
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
    atlas_gltexture.flags = TEXPREF_BINDLESS;

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
    char png_path[MAX_OSPATH];
    char json_path[MAX_OSPATH];

    Atlas_Invalidate();

    if (isDedicated)
        return 0;

    Atlas_NormalizeMapName(mapname, basename, sizeof(basename));
    if (!basename[0])
        return 0;

    q_strlcpy(atlas_missing_basename, basename, sizeof(atlas_missing_basename));
    atlas_attempted_build = false;

    q_snprintf(png_path, sizeof(png_path), "atlas/%s_atlas.png", basename);
    q_snprintf(json_path, sizeof(json_path), "atlas/%s_atlas.json", basename);

    if (!COM_FileExists(png_path, NULL) || !COM_FileExists(json_path, NULL))
    {
        Atlas_LogMissing();
        atlas_missing_for_map = true;
        return 0;
    }

    Con_Printf("Atlas_LoadForMap: loading atlas for %s\n", basename);

    if (!Atlas_LoadPNG(png_path) || !Atlas_LoadJSON(json_path))
    {
        Atlas_Invalidate();
        return 0;
    }

    atlas_missing_for_map = false;
    atlas_enabled = true;
    Con_DPrintf("Atlas_LoadForMap: atlas ready (%dx%d, %d entries)\n", atlas_width, atlas_height, atlas_entry_count);
    return 1;
}

typedef struct atlas_build_entry_s
{
    char name[64];
    int width, height;
    int x, y;
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

static byte *Atlas_ReadTextureSource(const gltexture_t *glt, int *width, int *height, enum srcformat *fmt_out)
{
    int mark = Hunk_LowMark();
    byte *raw = NULL;
    enum srcformat fmt = glt->source_format;
    size_t bytes_per_pixel = (fmt == SRC_RGBA) ? 4 : 1;
    size_t expected_size;

    *width = glt->source_width ? glt->source_width : glt->width;
    *height = glt->source_height ? glt->source_height : glt->height;
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
                Hunk_FreeToLowMark(mark);
                return NULL;
            }
            fclose(f);
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
    }
    else if (glt->source_offset)
    {
        if (expected_size > 0)
        {
            raw = (byte *)malloc(expected_size);
            if (raw)
                memcpy(raw, (const void *)glt->source_offset, expected_size);
        }
    }

    if (fmt_out)
        *fmt_out = fmt;

    Hunk_FreeToLowMark(mark);
    return raw;
}

static byte *Atlas_ExtractTexture(const gltexture_t *glt, int *width, int *height)
{
    enum srcformat fmt = SRC_INDEXED;
    byte *source = Atlas_ReadTextureSource(glt, width, height, &fmt);
    byte *rgba = NULL;

    if (!source || *width <= 0 || *height <= 0)
    {
        free(source);
        return NULL;
    }

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
        fprintf(f, "        {\"name\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}%s\n",
            e->name, e->x, e->y, e->width, e->height, (i == count - 1) ? "" : ",");
    }

    fprintf(f, "    ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

static qboolean Atlas_WritePNGFile(const char *path, const byte *data, int width, int height)
{
    const char *relative = path;
    size_t prefix = strlen(com_gamedir);

    if (!strncmp(path, com_gamedir, prefix) && path[prefix] == '/')
        relative = path + prefix + 1;

    return Image_WritePNG(relative, (byte *)data, width, height, 32, true);
}

void Atlas_CreateFromFallbacks(qmodel_t *model)
{
    atlas_build_entry_t *entries = NULL;
    int entry_count = 0;
    int max_dim = 0;
    int atlas_size;
    int i;
    char basename[MAX_QPATH];
    char png_path[MAX_OSPATH];
    char json_path[MAX_OSPATH];
    byte *atlas_pixels = NULL;

    if (!atlas_missing_for_map || atlas_attempted_build || !model || isDedicated)
        return;

    Atlas_NormalizeMapName(model->name, basename, sizeof(basename));
    if (q_strcasecmp(basename, atlas_missing_basename))
        return;

    atlas_attempted_build = true;

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
        goto cleanup;

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
        goto cleanup;

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

    q_snprintf(png_path, sizeof(png_path), "%s/atlas/%s_atlas.png", com_gamedir, basename);
    q_snprintf(json_path, sizeof(json_path), "%s/atlas/%s_atlas.json", com_gamedir, basename);

    COM_CreatePath(png_path);
    COM_CreatePath(json_path);

    if (!Atlas_WritePNGFile(png_path, atlas_pixels, atlas_size, atlas_size))
    {
        Con_Printf("Failed to write atlas texture to %s\n", png_path);
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

