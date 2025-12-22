#include "quakedef.h"
#include "ktx2_write.h"
#include "r_maptex_export.h"
#include "gl_texmgr.h"
#include "bspfile.h"
#include "q_ctype.h"

/*
===============================================================================

Map texture exporter (KTX2)

When enabled via r_maptex_export, this module collects textures referenced by
world surfaces at map load time and writes them out to:

  <gamedir>/maps/<mapname>/textures/<texname>.ktx2

Export sources:
  - Embedded BSP miptex data when present.
  - WAD miptex data for missing/empty BSP entries (from worldspawn "wad").

The exporter converts 8-bit indexed miptex data through the engine palette
tables, preserves mipmaps if requested, avoids duplicates, and logs a summary.

===============================================================================
*/

static cvar_t r_maptex_export = { "r_maptex_export", "0" };
static cvar_t r_maptex_export_format = { "r_maptex_export_format", "ktx2" };
static cvar_t r_maptex_export_uastc = { "r_maptex_export_uastc", "0" };
static cvar_t r_maptex_export_mipmaps = { "r_maptex_export_mipmaps", "1" };
static cvar_t r_maptex_export_overwrite = { "r_maptex_export_overwrite", "0" };
static cvar_t r_maptex_export_verbose = { "r_maptex_export_verbose", "0" };

typedef struct maptex_wad_s
{
        char name[MAX_OSPATH];
        byte *buffer;
        size_t size;
        int numlumps;
        lumpinfo_t *lumps;
} maptex_wad_t;

typedef struct maptex_miptex_s
{
        char name[16 + 1];
        int width;
        int height;
        int offsets[MIPLEVELS];
} maptex_miptex_t;

static size_t MapTex_LevelSize(int width, int height, int level)
{
        int w = q_max(1, width >> level);
        int h = q_max(1, height >> level);
        return (size_t)w * (size_t)h;
}

static qboolean MapTex_ParseWorldspawnKey(const char *ents, const char *key, char *value, size_t valuesize)
{
        const char *data;

        if (!ents || !key || !value || valuesize == 0)
                return false;

        data = COM_Parse(ents);
        if (!data || com_token[0] != '{')
                return false;

        while (1)
        {
                data = COM_Parse(data);
                if (!data || !com_token[0])
                        return false;
                if (com_token[0] == '}')
                        return false;

                if (!strcmp(com_token, "classname"))
                {
                        data = COM_Parse(data);
                        if (!data || strcmp(com_token, "worldspawn"))
                                return false;
                        continue;
                }

                if (!strcmp(com_token, key))
                {
                        data = COM_Parse(data);
                        if (!data)
                                return false;
                        q_strlcpy(value, com_token, valuesize);
                        return true;
                }

                data = COM_Parse(data);
                if (!data)
                        return false;
        }
}

static void MapTex_SanitizeName(const char *name, char *out, size_t outsize)
{
        size_t i;
        size_t len = q_strlcpy(out, name ? name : "", outsize);
        for (i = 0; i < len; ++i)
        {
                unsigned char c = (unsigned char)out[i];
                if (!(q_isalnum(c) || c == '_' || c == '-' || c == '.'))
                        out[i] = '_';
        }
}

static qboolean MapTex_LoadWad(const char *wadpath, maptex_wad_t *wad)
{
        wadinfo_t *header;
        lumpinfo_t *lump;
        int i;

        wad->buffer = COM_LoadMallocFile(wadpath, NULL);
        if (!wad->buffer)
                return false;
        wad->size = (size_t)com_filesize;

        header = (wadinfo_t *)wad->buffer;
        if (memcmp(header->identification, "WAD2", 4) != 0)
        {
                free(wad->buffer);
                wad->buffer = NULL;
                wad->size = 0;
                return false;
        }

        wad->numlumps = LittleLong(header->numlumps);
        wad->lumps = (lumpinfo_t *)(wad->buffer + LittleLong(header->infotableofs));
        for (i = 0; i < wad->numlumps; ++i)
        {
                lump = wad->lumps + i;
                lump->filepos = LittleLong(lump->filepos);
                lump->disksize = LittleLong(lump->disksize);
                lump->size = LittleLong(lump->size);
                W_CleanupName(lump->name, lump->name);
        }

        return true;
}

static int MapTex_LoadWadList(const qmodel_t *mod, maptex_wad_t *wads, int maxwads)
{
        char wadlist[2048];
        char wadentry[MAX_OSPATH];
        char wadpath[MAX_OSPATH];
        char *token;
        int count = 0;

        if (!mod || !mod->entities || !wads || maxwads <= 0)
                return 0;

        if (!MapTex_ParseWorldspawnKey(mod->entities, "wad", wadlist, sizeof(wadlist)))
                return 0;

        for (token = strtok(wadlist, ";"); token && count < maxwads; token = strtok(NULL, ";"))
        {
                const char *base;

                q_strlcpy(wadentry, token, sizeof(wadentry));
                COM_DefaultExtension(wadentry, ".wad", sizeof(wadentry));

                for (char *p = wadentry; *p; ++p)
                        if (*p == '\\')
                                *p = '/';

                if (MapTex_LoadWad(wadentry, &wads[count]))
                {
                        q_strlcpy(wads[count].name, wadentry, sizeof(wads[count].name));
                        count++;
                        continue;
                }

                base = COM_SkipPath(wadentry);
                if (base && base[0])
                {
                        if (MapTex_LoadWad(base, &wads[count]))
                        {
                                q_strlcpy(wads[count].name, base, sizeof(wads[count].name));
                                count++;
                                continue;
                        }

                        q_snprintf(wadpath, sizeof(wadpath), "wads/%s", base);
                        if (MapTex_LoadWad(wadpath, &wads[count]))
                        {
                                q_strlcpy(wads[count].name, wadpath, sizeof(wads[count].name));
                                count++;
                                continue;
                        }
                }

                Con_Warning("MapTex: missing wad %s\n", wadentry);
        }

        return count;
}

static void MapTex_FreeWads(maptex_wad_t *wads, int count)
{
        int i;
        for (i = 0; i < count; ++i)
        {
                free(wads[i].buffer);
                wads[i].buffer = NULL;
                wads[i].lumps = NULL;
                wads[i].size = 0;
        }
}

static qboolean MapTex_ReadMiptexFromBuffer(const byte *buffer, size_t size, maptex_miptex_t *out)
{
        const miptex_t *mt;
        int i;

        if (!buffer || size < sizeof(miptex_t) || !out)
                return false;

        mt = (const miptex_t *)buffer;
        memset(out, 0, sizeof(*out));
        memcpy(out->name, mt->name, sizeof(mt->name));
        out->name[sizeof(mt->name)] = '\0';
        out->width = LittleLong(mt->width);
        out->height = LittleLong(mt->height);
        for (i = 0; i < MIPLEVELS; ++i)
                out->offsets[i] = LittleLong(mt->offsets[i]);

        return out->name[0] != '\0';
}

static qboolean MapTex_GetMiptexLevel(const maptex_miptex_t *mt, const byte *base, size_t base_size,
                                      int level, const byte **out_data, size_t *out_size)
{
        size_t size;
        int offset;

        if (!mt || !out_data || !out_size || level < 0 || level >= MIPLEVELS)
                return false;

        size = MapTex_LevelSize(mt->width, mt->height, level);
        offset = mt->offsets[level];
        if (offset <= 0)
                return false;

        if ((size_t)offset > base_size || size > base_size - (size_t)offset)
                return false;

        *out_data = base + offset;
        *out_size = size;
        return true;
}

static qboolean MapTex_FindMiptexInWads(const maptex_wad_t *wads, int wad_count, const char *name,
                                        const byte **out_buffer, size_t *out_size)
{
        char clean[16];
        int i, j;

        if (!wads || wad_count <= 0 || !name || !out_buffer || !out_size)
                return false;

        W_CleanupName(name, clean);

        for (i = 0; i < wad_count; ++i)
        {
                const maptex_wad_t *wad = &wads[i];
                for (j = 0; j < wad->numlumps; ++j)
                {
                        const lumpinfo_t *lump = &wad->lumps[j];
                        if (lump->type != TYP_MIPTEX)
                                continue;
                        if (strcmp(clean, lump->name))
                                continue;
                        if (lump->compression != CMP_NONE)
                                continue;
                        if (lump->size <= 0 || lump->filepos < 0)
                                continue;
                        if ((size_t)lump->filepos + (size_t)lump->size > wad->size)
                                continue;

                        *out_buffer = wad->buffer + lump->filepos;
                        *out_size = (size_t)lump->size;
                        return true;
                }
        }

        return false;
}

static byte *MapTex_ConvertIndexedToRGBA(const byte *indexed, size_t pixel_count)
{
        byte *rgba;
        size_t i;

        rgba = (byte *)malloc(pixel_count * 4u);
        if (!rgba)
                return NULL;

        for (i = 0; i < pixel_count; ++i)
        {
                const byte *src = (const byte *)&d_8to24table[indexed[i]];
                rgba[i * 4 + 0] = src[0];
                rgba[i * 4 + 1] = src[1];
                rgba[i * 4 + 2] = src[2];
                rgba[i * 4 + 3] = src[3];
        }

        return rgba;
}

static qboolean MapTex_ExportKTX2(const char *path, const maptex_miptex_t *mt,
                                  const byte *source_base, size_t source_size,
                                  qboolean include_mips, int *out_mipcount)
{
        const uint8_t *mip_data[MIPLEVELS];
        size_t mip_sizes[MIPLEVELS];
        uint8_t *rgba_data[MIPLEVELS];
        int level;
        int mip_count = 0;
        qboolean ok = false;

        memset(rgba_data, 0, sizeof(rgba_data));

        if (mt->width <= 0 || mt->height <= 0)
        {
                if (out_mipcount)
                        *out_mipcount = 0;
                return false;
        }

        for (level = 0; level < MIPLEVELS; ++level)
        {
            const byte *level_data;
            size_t level_size;

            if (level > 0 && !include_mips)
                    break;

            if (!MapTex_GetMiptexLevel(mt, source_base, source_size, level, &level_data, &level_size))
                    break;

            rgba_data[level] = MapTex_ConvertIndexedToRGBA(level_data, level_size);
            if (!rgba_data[level])
                    break;

            mip_data[level] = rgba_data[level];
            mip_sizes[level] = level_size * 4u;
            mip_count++;
        }

        if (mip_count > 0)
                ok = KTX2_WriteRGBA8File(path, mt->width, mt->height, mip_count, mip_data, mip_sizes);

        for (level = 0; level < mip_count; ++level)
                free(rgba_data[level]);

        if (out_mipcount)
                *out_mipcount = mip_count;

        return ok;
}

static qboolean MapTex_LoadMiptexFromBsp(const byte *bsp_base, const lump_t *tex_lump, int index,
                                         maptex_miptex_t *out, const byte **out_base, size_t *out_size)
{
        dmiptexlump_t *m;
        byte *lump_base;
        size_t lump_size;
        int offset;
        int nummiptex;

        if (!bsp_base || !tex_lump || !out || !out_base || !out_size)
                return false;

        lump_base = (byte *)(bsp_base + tex_lump->fileofs);
        lump_size = (size_t)tex_lump->filelen;
        if (lump_size < sizeof(dmiptexlump_t))
                return false;

        m = (dmiptexlump_t *)lump_base;
        nummiptex = LittleLong(m->nummiptex);
        if (index < 0 || index >= nummiptex)
                return false;
        offset = LittleLong(m->dataofs[index]);
        if (offset < 0 || (size_t)offset >= lump_size)
                return false;

        if (!MapTex_ReadMiptexFromBuffer(lump_base + offset, lump_size - (size_t)offset, out))
                return false;

        *out_base = lump_base + offset;
        *out_size = lump_size - (size_t)offset;
        return true;
}

void R_MapTex_ExportInit(void)
{
        Cvar_RegisterVariable(&r_maptex_export);
        Cvar_RegisterVariable(&r_maptex_export_format);
        Cvar_RegisterVariable(&r_maptex_export_uastc);
        Cvar_RegisterVariable(&r_maptex_export_mipmaps);
        Cvar_RegisterVariable(&r_maptex_export_overwrite);
        Cvar_RegisterVariable(&r_maptex_export_verbose);
}

void R_MapTex_ExportFromBsp(qmodel_t *mod, const byte *bsp_base, const lump_t *tex_lump)
{
        int nummiptex;
        uint32_t *used;
        maptex_wad_t wads[32];
        int wad_count = 0;
        int exported = 0;
        int skipped = 0;
        int failed = 0;
        char mapname[MAX_QPATH];
        char **seen_names = NULL;
        size_t seen_count = 0;

        if (!mod || mod->type != mod_brush || !bsp_base || !tex_lump)
                return;

        if (r_maptex_export.value <= 0)
                return;

        if (q_strcasecmp(r_maptex_export_format.string, "ktx2"))
        {
                Con_Warning("MapTex: unsupported export format '%s'\n", r_maptex_export_format.string);
                return;
        }

        if (tex_lump->filelen <= 0)
        {
                Con_Printf("MapTex: exported %d, skipped %d, failed %d\n", exported, skipped, failed);
                return;
        }

        if (r_maptex_export_uastc.value > 0 && r_maptex_export_verbose.value)
                Con_Printf("MapTex: UASTC requested but encoder unavailable, exporting RGBA8 KTX2\n");

        nummiptex = LittleLong(((dmiptexlump_t *)(bsp_base + tex_lump->fileofs))->nummiptex);
        if (nummiptex <= 0)
        {
                Con_Printf("MapTex: exported %d, skipped %d, failed %d\n", exported, skipped, failed);
                return;
        }

        used = (uint32_t *)calloc(BITARRAY_DWORDS(nummiptex), sizeof(uint32_t));
        if (!used)
                Sys_Error("MapTex: out of memory for texture set");

        for (int i = 0; i < mod->numsurfaces; ++i)
        {
                msurface_t *surf = &mod->surfaces[i];
                int miptex = surf->texinfo ? surf->texinfo->miptex : -1;
                if (miptex >= 0 && miptex < nummiptex)
                        SetBit(used, miptex);
        }

        wad_count = MapTex_LoadWadList(mod, wads, (int)(sizeof(wads) / sizeof(wads[0])));

        COM_FileBase(mod->name, mapname, sizeof(mapname));

        for (int i = 0; i < nummiptex; ++i)
        {
                char sanitized[MAX_QPATH];
                char outpath[MAX_OSPATH];
                maptex_miptex_t mt;
                const byte *source_base = NULL;
                size_t source_size = 0;
                qboolean ok;

                if (!GetBit(used, i))
                        continue;

                if (!MapTex_LoadMiptexFromBsp(bsp_base, tex_lump, i, &mt, &source_base, &source_size))
                {
                        if (r_maptex_export_verbose.value)
                                Con_Warning("MapTex: missing texture index %d in BSP\n", i);
                        failed++;
                        continue;
                }

                if (!mt.name[0])
                {
                        if (r_maptex_export_verbose.value)
                                Con_Warning("MapTex: empty texture name at index %d\n", i);
                        failed++;
                        continue;
                }

                {
                        const byte *level_data = NULL;
                        size_t level_size = 0;
                        qboolean use_bsp = MapTex_GetMiptexLevel(&mt, source_base, source_size, 0,
                                                                &level_data, &level_size);
                        if (!use_bsp)
                        {
                                const byte *wad_buffer = NULL;
                                size_t wad_size = 0;

                                if (!MapTex_FindMiptexInWads(wads, wad_count, mt.name, &wad_buffer, &wad_size))
                                {
                                        if (r_maptex_export_verbose.value)
                                                Con_Warning("MapTex: wad texture %s missing\n", mt.name);
                                        failed++;
                                        continue;
                                }

                                if (!MapTex_ReadMiptexFromBuffer(wad_buffer, wad_size, &mt))
                                {
                                        if (r_maptex_export_verbose.value)
                                                Con_Warning("MapTex: invalid wad miptex %s\n", mt.name);
                                        failed++;
                                        continue;
                                }

                                source_base = wad_buffer;
                                source_size = wad_size;
                        }
                }

                MapTex_SanitizeName(mt.name, sanitized, sizeof(sanitized));
                {
                        qboolean seen = false;
                        for (size_t s = 0; s < seen_count; ++s)
                        {
                                if (!q_strcasecmp(seen_names[s], sanitized))
                                {
                                        seen = true;
                                        break;
                                }
                        }
                        if (seen)
                                continue;

                        seen_names = (char **)realloc(seen_names, sizeof(*seen_names) * (seen_count + 1));
                        if (!seen_names)
                                Sys_Error("MapTex: out of memory tracking names");
                        seen_names[seen_count] = strdup(sanitized);
                        if (!seen_names[seen_count])
                                Sys_Error("MapTex: out of memory tracking names");
                        seen_count++;
                }
                q_snprintf(outpath, sizeof(outpath), "%s/maps/%s/textures/%s.ktx2", com_gamedir, mapname, sanitized);

                if (!r_maptex_export_overwrite.value)
                {
                        time_t filetime;
                        if (Sys_GetFileTime(outpath, &filetime))
                        {
                                skipped++;
                                if (r_maptex_export_verbose.value)
                                        Con_Printf("MapTex: skipped existing %s\n", sanitized);
                                continue;
                        }
                }

                COM_CreatePath(outpath);

                ok = MapTex_ExportKTX2(outpath, &mt, source_base, source_size, r_maptex_export_mipmaps.value > 0, NULL);
                if (!ok)
                {
                        const byte *wad_buffer = NULL;
                        size_t wad_size = 0;
                        maptex_miptex_t wad_mt;

                        if (MapTex_FindMiptexInWads(wads, wad_count, mt.name, &wad_buffer, &wad_size) &&
                            MapTex_ReadMiptexFromBuffer(wad_buffer, wad_size, &wad_mt))
                        {
                                ok = MapTex_ExportKTX2(outpath, &wad_mt, wad_buffer, wad_size,
                                                       r_maptex_export_mipmaps.value > 0, NULL);
                        }
                }

                if (ok)
                {
                        exported++;
                        if (r_maptex_export_verbose.value)
                                Con_Printf("MapTex: exported %s\n", sanitized);
                }
                else
                {
                        failed++;
                        if (r_maptex_export_verbose.value)
                                Con_Warning("MapTex: failed to export %s\n", sanitized);
                }
        }

        free(used);
        for (size_t s = 0; s < seen_count; ++s)
                free(seen_names[s]);
        free(seen_names);
        MapTex_FreeWads(wads, wad_count);

        Con_Printf("MapTex: exported %d, skipped %d, failed %d\n", exported, skipped, failed);
}
