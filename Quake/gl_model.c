/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "quakedef.h"
#include "gl_lightgrid.h"
#include "gl_ktx2.h"
#include "mat_material.h"
#include "../common/lightgrid.h"

#define INVALID_LIGHTSTYLE_OLD 255


static qmodel_t*	loadmodel;
static char	loadname[32];	// for hunk tags

extern vec3_t lightcolor;

static void Mod_LoadSpriteModel (qmodel_t *mod, void *buffer);
static void Mod_LoadBrushModel (qmodel_t *mod, void *buffer);
static void Mod_LoadAliasModel (qmodel_t *mod, void *buffer);
static void Mod_LoadMD5MeshModel (qmodel_t *mod, const char *buffer);
static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash);
static qboolean Mod_ParseWorldspawnKey (qmodel_t *mod, const char *key, char *value, size_t valuesize);
static qboolean Mod_IsInlineSubmodel (const qmodel_t *mod);
static qboolean Mod_IsExternalBrushModel (const qmodel_t *mod);
static qboolean Mod_ShouldSkipBrushExtras (const qmodel_t *mod);
static qboolean BSPX_LightGridLoad (qmodel_t *mod, void *lump, int lumpsize);
static qboolean LightgridOctree_LoadBSPX (qmodel_t *mod, void *data, int size);
static void Lightgrid_Info_f (void);
static qboolean Mod_LoadLightgridOctreeFromMap (qmodel_t *mod, const char *mapname);
static void Mod_LoadLightgridOctree_f (cvar_t *var);
static void Mod_PrintLightgridDebug (const char *op, const qmodel_t *mod, const char *source_path, qboolean applied, const char *reason);
void Mod_LoadRGBLightingBSPX (qmodel_t *mod, void *buffer, int size);
void Mod_LoadFaceNormalsBSPX (qmodel_t *mod, void *buffer, int size);

static void Mod_Print (void);

// Forward declarations fÃ¼r Lightgrid-Funktionen
void Lightgrid_Clear(void);
const lightgrid_t *Lightgrid_Get(void);
const char *Lightgrid_GetSource(void);
void Lightgrid_Free(lightgrid_t *lg);

static cvar_t	external_ents = {"external_ents", "1", CVAR_ARCHIVE};
static cvar_t	external_vis = {"external_vis", "1", CVAR_ARCHIVE};
static cvar_t	gl_loadlitfiles = {"gl_loadlitfiles", "1", CVAR_ARCHIVE};
static cvar_t	external_lits_dir = {"external_lits_dir", "", CVAR_ARCHIVE};
static cvar_t	gl_bsp_lightmap_bgr = {"gl_bsp_lightmap_bgr", "-1", CVAR_ARCHIVE};
static cvar_t	mod_ignorelmscale = {"mod_ignorelmscale", "0", 0};
cvar_t			load_lightgrid_octree = {"load_lightgrid_octree", "", CVAR_NONE};
cvar_t			r_md5 = {"r_md5", "1", CVAR_ARCHIVE};

static byte	*mod_novis;
static int	mod_novis_capacity;

static byte	*mod_decompressed;
static int	mod_decompressed_capacity;

static qboolean Mod_IsInlineSubmodel (const qmodel_t *mod)
{
	return (mod && mod->name[0] == '*');
}

static qboolean Mod_IsExternalBrushModel (const qmodel_t *mod)
{
	if (!mod || mod->type != mod_brush)
		return false;

	if (Mod_IsInlineSubmodel (mod))
		return false;

	if (sv.modelname[0])
		return strcmp(mod->name, sv.modelname);

	return (cl.worldmodel && mod != cl.worldmodel);
}

static qboolean Mod_ShouldSkipBrushExtras (const qmodel_t *mod)
{
        return Mod_IsInlineSubmodel (mod) || Mod_IsExternalBrushModel (mod);
}

#define	MAX_MOD_KNOWN	4096 /*johnfitz -- was 512 */
static qmodel_t	mod_known[MAX_MOD_KNOWN];
static int		mod_numknown;

texture_t	*r_notexture_mip; //johnfitz -- moved here from r_main.c
texture_t	*r_notexture_mip2; //johnfitz -- used for non-lightmapped surfs with a missing texture

/*
===============
R_MD5_f -- called when r_md5 changes
===============
*/
static void R_MD5_f (cvar_t *cvar)
			{
	int			i;
	qmodel_t	*mod;

	//ericw -- free alias model VBOs
	GLMesh_DeleteVertexBuffers ();

	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (mod->type == mod_alias)
		{
			if (Cache_Check (&mod->cache))
				Cache_Free (&mod->cache, true);
			mod->needload = true;
		}
	}

	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
		if (mod->type == mod_alias)
			Mod_LoadModel (mod, false);

	if (cls.state == ca_connected && cls.signon == SIGNONS)
		for (i = 0; i < cl.maxclients; i++)
			R_TranslateNewPlayerSkin (i);
}

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
				{
	Cvar_RegisterVariable (&external_vis);
	Cvar_RegisterVariable (&external_ents);
	Cvar_RegisterVariable (&gl_loadlitfiles);
	Cvar_RegisterVariable (&gl_bsp_lightmap_bgr);
	Cvar_RegisterVariable (&external_lits_dir);
	Cvar_RegisterVariable (&mod_ignorelmscale);
	Cvar_RegisterVariable (&r_md5);
	Cvar_RegisterVariable (&load_lightgrid_octree);
	Cvar_SetCallback (&r_md5, R_MD5_f);
	Cvar_SetCallback (&load_lightgrid_octree, Mod_LoadLightgridOctree_f);

        Cmd_AddCommand ("mcache", Mod_Print);
        Cmd_AddCommand ("lightgrid_info", Lightgrid_Info_f);

        //johnfitz -- create notexture miptex
        r_notexture_mip = (texture_t *) Hunk_AllocName (sizeof(texture_t), "r_notexture_mip");
        q_strlcpy (r_notexture_mip->name, "notexture", sizeof (r_notexture_mip->name));
        r_notexture_mip->height = r_notexture_mip->width = 32;

	r_notexture_mip2 = (texture_t *) Hunk_AllocName (sizeof(texture_t), "r_notexture_mip2");
	q_strlcpy (r_notexture_mip2->name, "notexture2", sizeof (r_notexture_mip2->name));
	r_notexture_mip2->height = r_notexture_mip2->width = 32;
	//johnfitz
}

/*
===============
Mod_IsKnownModel
===============
*/
qboolean Mod_IsKnownModel (const qmodel_t *mod)
{
	if (!mod)
		return false;

	return (mod >= mod_known && mod < mod_known + mod_numknown);
}

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *Mod_Extradata (qmodel_t *mod)
				{
	void	*r;

	r = Cache_Check (&mod->cache);
	if (r)
		return r;

	Mod_LoadModel (mod, true);

	if (!mod->cache.data)
		Sys_Error ("Mod_Extradata: caching failed");
	return mod->cache.data;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, qmodel_t *model)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	if (!model || !model->nodes)
		Sys_Error ("Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return NULL;	// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
static byte *Mod_DecompressVis (byte *in, qmodel_t *model)
{
	int		c;
	byte	*out;
	byte	*outend;
	int		row;

	row = (model->numleafs+7)>>3;
	if (mod_decompressed == NULL || row > mod_decompressed_capacity)
	{
		mod_decompressed_capacity = (row + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK;
		mod_decompressed = (byte *) q_realloc(mod_decompressed, mod_decompressed_capacity);
		if (!mod_decompressed)
			Sys_Error ("Mod_DecompressVis: q_realloc() failed on %d bytes", mod_decompressed_capacity);
	}
	out = mod_decompressed;
	outend = mod_decompressed + row;

	if (!in)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return mod_decompressed;
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}

		c = in[1];
		in += 2;
		if (c > row - (out - mod_decompressed))
			c = row - (out - mod_decompressed);	//now that we're dynamically allocating pvs buffers, we have to be more careful to avoid heap overflows with buggy maps.
		while (c)
		{
			if (out == outend)
			{
				if(!model->viswarn) {
					model->viswarn = true;
					Con_Warning("Mod_DecompressVis: output overrun on model \"%s\"\n", model->name);
				}
				return mod_decompressed;
			}
			*out++ = 0;
			c--;
		}
	} while (out - mod_decompressed < row);

	return mod_decompressed;
}

byte *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model)
{
	if (leaf == model->leafs)
		return Mod_NoVisPVS (model);
	return Mod_DecompressVis (leaf->compressed_vis, model);
}

byte *Mod_NoVisPVS (qmodel_t *model)
{
	int pvsbytes;
 
	pvsbytes = (model->numleafs+7)>>3;
	pvsbytes = (pvsbytes + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK; // round up
	if (mod_novis == NULL || pvsbytes > mod_novis_capacity)
	{
		mod_novis_capacity = pvsbytes;
		mod_novis = (byte *) q_realloc(mod_novis, mod_novis_capacity);
		if (!mod_novis)
			Sys_Error ("Mod_NoVisPVS: q_realloc() failed on %d bytes", mod_novis_capacity);
		
		memset(mod_novis, 0xff, mod_novis_capacity);
	}
	return mod_novis;
}

/*
===================
Mod_ClearAll
===================
*/
void Mod_ClearAll (void)
{
	int		i;
	qmodel_t	*mod;

	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (mod->type != mod_alias)
		{
			mod->needload = true;
			TexMgr_FreeTexturesForOwner (mod); //johnfitz
		}
	}

	Material_Shutdown ();
}

void Mod_ResetAll (void)
{
	int		i;
	qmodel_t	*mod;

	//ericw -- free alias model VBOs
	GLMesh_DeleteVertexBuffers ();

	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!mod->needload) //otherwise Mod_ClearAll() did it already
			TexMgr_FreeTexturesForOwner (mod);
		memset(mod, 0, sizeof(qmodel_t));
	}
	mod_numknown = 0;
}

/*
==================
Mod_FindName

==================
*/
static qmodel_t *Mod_FindName (const char *name)
{
	int		i;
	qmodel_t	*mod;

	if (!name[0])
		Sys_Error ("Mod_FindName: NULL name"); //johnfitz -- was "Mod_ForName"

//
// search the currently loaded models
//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!strcmp (mod->name, name) )
			break;
	}

	if (i == mod_numknown)
	{
		if (mod_numknown == MAX_MOD_KNOWN)
			Sys_Error ("mod_numknown == MAX_MOD_KNOWN");
		q_strlcpy (mod->name, name, MAX_QPATH);
		mod->needload = true;
		mod_numknown++;
	}

	return mod;
}

/*
==================
Mod_TouchModel

==================
*/
void Mod_TouchModel (const char *name)
{
	qmodel_t	*mod;

	mod = Mod_FindName (name);

	if (!mod->needload)
	{
		if (mod->type == mod_alias)
			Cache_Check (&mod->cache);
	}
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash)
{
	byte	*buf;
	int		mod_type;

	if (!mod->needload)
	{
		if (mod->type == mod_alias)
		{
			if (Cache_Check (&mod->cache))
				return mod;
		}
		else
			return mod;		// not cached at all
	}

//
// because the world is so huge, load it one piece at a time
//
	if (!crash)
	{

	}

//
// load the file
//
	Host_BeginAssetLoading ();
	buf = COM_LoadMallocFile (mod->name, &mod->path_id);
	if (!buf)
	{
		Host_EndAssetLoading ();
		if (crash)
			Host_Error ("Mod_LoadModel: %s not found", mod->name); //johnfitz -- was "Mod_NumForName"
		return NULL;
	}

//
// allocate a new model
//
	COM_FileBase (mod->name, loadname, sizeof(loadname));

	loadmodel = mod;

//
// fill it in
//

// call the apropriate loader
	mod->needload = false;

	mod->facenormals = NULL;
	mod->facenormals_count = 0;
	mod->has_facenormals = false;

	mod_type = (buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
	switch (mod_type)
	{
	case IDPOLYHEADER:
		Mod_LoadAliasModel (mod, buf);
		break;

	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buf);
		break;

	default:
		Mod_LoadBrushModel (mod, buf);
		break;
	}

	q_free(buf);
	Host_EndAssetLoading ();

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
qmodel_t *Mod_ForName (const char *name, qboolean crash)
{
	qmodel_t	*mod;

	mod = Mod_FindName (name);

	return Mod_LoadModel (mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static byte	*mod_base;


typedef int32_t (*index_reader_t)(byte *base, int offset);

typedef struct {
    index_reader_t   read_edge_index;
    index_reader_t   read_face_numedges;
    index_reader_t   read_face_planenum;
    index_reader_t   read_face_side;
    index_reader_t   read_face_texinfo;
    size_t           edge_stride;
    size_t           edge_vertex_step;
    size_t           face_stride;
    size_t           face_side_offset;
    size_t           face_firstedge_offset;
    size_t           face_numedges_offset;
    size_t           face_texinfo_offset;
    size_t           face_styles_offset;
    size_t           face_lightofs_offset;
    qboolean         is_bsp2;
    qboolean         is_bsp29;
} bsp_loader_t;

static bsp_loader_t bsp_loader;

typedef struct {
    char lumpname[24]; // up to 23 chars, zero-padded
    int fileofs;  // from file start
    int filelen;
} bspx_lump_disk_t;
typedef struct {
    char id[4];  // 'BSPX'
    int numlumps;
        bspx_lump_disk_t lumps[1];
} bspx_header_disk_t;
typedef struct
{
        lump_t          lumps[HEADER_LUMPS];
        size_t          header_size;
        qboolean        has_bspx_header;
        qboolean        is_bsp2;
        qboolean        is_bsp29;
        int                     bspversion;
} bsp_header_info_t;
static bspx_header_t *BSPX_Setup(qmodel_t *mod, const bsp_header_info_t *header, size_t filelen);
static void *BSPX_FindLump(bspx_header_t *bspxheader, void *base, const char *lumpname, size_t *lumpsize);

/*
 * BSPX lump names are fixed 24-byte, zero-padded fields (max 23 chars + NUL).
 * Compare against the full field so handler aliases never match by prefix.
 */
static qboolean BSPX_LumpNameEquals(const char entry_name[24], const char *name)
{
	char expected_name[24];
	size_t len;

	len = strlen(name);
	if (len >= sizeof(expected_name))
		return false;

	memset(expected_name, 0, sizeof(expected_name));
	memcpy(expected_name, name, len);

	return !memcmp(entry_name, expected_name, sizeof(expected_name));
}

typedef struct
{
        char lumpname[sizeof(((bspx_lump_disk_t *)0)->lumpname) + 1];
        qboolean used;
        qboolean unsupported;
} bspx_lump_usage_t;
#define MAX_BSPX_LUMPS 1024
#define MAX_BSPX_LUMP_USAGE 256
static bspx_lump_usage_t bspx_lump_usage[MAX_BSPX_LUMP_USAGE];
static int bspx_lump_usage_index[MAX_BSPX_LUMP_USAGE];
static int bspx_lump_usage_count;

static int Q1BSPX_UsageIndexCompare(const void *a, const void *b)
{
	const int index_a = *(const int *)a;
	const int index_b = *(const int *)b;
	const int cmp = strncmp(
		bspx_lump_usage[index_a].lumpname,
		bspx_lump_usage[index_b].lumpname,
		sizeof(((bspx_lump_disk_t *)0)->lumpname));

	if (cmp)
		return cmp;

	/* Keep deterministic order for duplicate lump names. */
	if (index_a < index_b)
		return -1;
	if (index_a > index_b)
		return 1;

	return 0;
}
//supported lumps:
//RGBLIGHTING (.lit)
//LIGHTING_E5BGR9 (hdr lighting)
//LMSHIFT (.lit2)
//LMOFFSET (LMSHIFT helper)
//LMSTYLE (LMSHIFT helper)
//LIGHTINGDIR (.lux)
//VERTEXNORMALS (smooth shading with dlights/rtlights)

//unsupported lumps ('documented' elsewhere):
//BRUSHLIST (because hulls suck)
static void Q1BSPX_ResetUsage(void)
{
        bspx_lump_usage_count = 0;
        memset(bspx_lump_usage, 0, sizeof(bspx_lump_usage));
	memset(bspx_lump_usage_index, 0, sizeof(bspx_lump_usage_index));
}
static void Q1BSPX_RecordEntries(qmodel_t *mod)
{
        int i;

        bspx_lump_usage_count = q_min(mod->bspx_entries_count, MAX_BSPX_LUMP_USAGE);
        for (i = 0; i < bspx_lump_usage_count; i++)
        {
                q_strlcpy(bspx_lump_usage[i].lumpname, mod->bspx_entries[i].name, sizeof(bspx_lump_usage[i].lumpname));
		bspx_lump_usage_index[i] = i;
        }

	qsort(bspx_lump_usage_index, bspx_lump_usage_count, sizeof(bspx_lump_usage_index[0]), Q1BSPX_UsageIndexCompare);
}
static bspx_lump_usage_t *Q1BSPX_FindUsage(const char *lumpname)
{
	int low = 0;
	int high = bspx_lump_usage_count - 1;
	int hit = -1;

	while (low <= high)
	{
		const int mid = low + (high - low) / 2;
		const int usage_index = bspx_lump_usage_index[mid];
		const int cmp = strncmp(
			bspx_lump_usage[usage_index].lumpname,
			lumpname,
			sizeof(((bspx_lump_disk_t *)0)->lumpname));

		if (cmp < 0)
			low = mid + 1;
		else if (cmp > 0)
			high = mid - 1;
		else
		{
			hit = mid;
			high = mid - 1;
		}
	}

	if (hit < 0)
		return NULL;

	return &bspx_lump_usage[bspx_lump_usage_index[hit]];
}

static qboolean Q1BSPX_IsProcessed(const char *lumpname)
{
        bspx_lump_usage_t *usage = Q1BSPX_FindUsage(lumpname);

        return usage && (usage->used || usage->unsupported);
}

static void *Q1BSPX_FindLump(const char *lumpname, int *lumpsize);

static void Q1BSPX_DecodeE5BGR9Lighting(byte *dst, const unsigned int *src, int samples)
{
        /* E5BGR9 bit layout: [31:27]=exp [26:18]=R [17:9]=G [8:0]=B.
         * Decode into dst as RGB to match the engine lightdata convention. */
        for (int i = 0; i < samples; i++)
        {
                uint32_t packed = LittleLong(src[i]);

                int e = packed >> 27;
                int b = packed & 0x1ff;
                int g = (packed >> 9) & 0x1ff;
                int r = (packed >> 18) & 0x1ff;

                float scale = exp2f((float)e) / 512.0f;

                float R = r * scale;
                float G = g * scale;
                float B = b * scale;

                dst[0] = (unsigned char)CLAMP(0, (int)(R * 255.0f), 255);
                dst[1] = (unsigned char)CLAMP(0, (int)(G * 255.0f), 255);
                dst[2] = (unsigned char)CLAMP(0, (int)(B * 255.0f), 255);
                dst += 3;
        }
}

static void Mod_DecodeRgbeLighting(byte *dst, const byte *src, int samples, qboolean bgr)
{
        /* Radiance RGBE stores channels as R, G, B, exponent.
         * Some KEX/rerelease assets may instead store B, G, R, exponent. */
        for (int i = 0; i < samples; i++, dst += 3, src += 4)
        {
                int e = src[3];
                if (!e)
                {
                        dst[0] = dst[1] = dst[2] = 0;
                        continue;
                }

                float scale = exp2f((float)(e - 128));

                float R = ((bgr ? src[2] : src[0]) / 256.0f) * scale;
                float G = (src[1] / 256.0f) * scale;
                float B = ((bgr ? src[0] : src[2]) / 256.0f) * scale;

                dst[0] = CLAMP(0, (int)(R * 255.0f), 255);
                dst[1] = CLAMP(0, (int)(G * 255.0f), 255);
                dst[2] = CLAMP(0, (int)(B * 255.0f), 255);
        }
}

static qboolean Mod_ShouldAutoSwapBspLighting(const qmodel_t *mod)
{
	if (!mod || mod->bspversion != BSPVERSION)
		return false;

	/* Quake remaster/KEX maps use an IBSP wrapper with version 29. */
	if (com_filesize >= 8 && !strncmp((const char *)mod_base, "IBSP", 4))
		return true;

	/* Fallback for custom installs that mount rerelease content directly. */
	if (strstr(com_gamedir, "rerelease") || strstr(com_gamedir, "RERELEASE"))
		return true;

	/* Heuristic: 4-byte LIGHTING data without BSPX RGBLIGHTING likely uses BGRA/BGRX bytes. */
	if (mod->lightdatasize > 0 && (mod->lightdatasize % 4) == 0 && !Q1BSPX_FindLump("RGBLIGHTING", NULL))
		return true;

	return false;
}

static qboolean Mod_RgbExponentLooksLikeAlpha(const byte *src, int samples)
{
	int full_alpha = 0;

	for (int i = 0; i < samples; i++)
	{
		if (src[i * 4 + 3] >= 250)
			full_alpha++;
	}

	return samples > 0 && full_alpha > (samples * 9) / 10;
}

static void Q1BSPX_MarkUsed(const char *lumpname)
{
        bspx_lump_usage_t *usage = Q1BSPX_FindUsage(lumpname);

        if (usage)
                usage->used = true;
}
static void Q1BSPX_MarkUnsupported(const char *lumpname)
{
        bspx_lump_usage_t *usage = Q1BSPX_FindUsage(lumpname);

        if (usage)
                usage->unsupported = true;
}
static void Q1BSPX_LogUsage(const char *modelname)
{
        int i;

        if (!loadmodel || !loadmodel->bspx_entries_count || !bspx_lump_usage_count)
        {
                Con_Printf("%s: no BSPX lumps present\n", modelname);
                return;
        }
	Con_Printf("%s BSPX lumps:\n", modelname);
	for (i = 0; i < bspx_lump_usage_count; i++)
	{
		if (bspx_lump_usage[i].used)
			Con_Printf("  %s: used\n", bspx_lump_usage[i].lumpname);
		else if (bspx_lump_usage[i].unsupported)
			Con_Printf("  %s: present (unsupported)\n", bspx_lump_usage[i].lumpname);
		else
			Con_Printf("  %s: present (not used)\n", bspx_lump_usage[i].lumpname);
	}
}
static void *Q1BSPX_FindLump(const char *lumpname, int *lumpsize)
{
        size_t size = 0;
        void *data;

        data = BSPX_FindLump(loadmodel ? loadmodel->bspx_header : NULL, mod_base, lumpname, &size);

        if (lumpsize)
                *lumpsize = (int)size;

        return data;
}

void Mod_LoadRGBLightingBSPX (qmodel_t *mod, void *buffer, int size)
{
        int expected_size;
        byte *memptr;

        if (!mod)
                return;

        expected_size = mod->lightdatasamples ? (mod->lightdatasamples * 3) : (mod->lightdatasize * 3);
        if (!expected_size)
                return;

        if (size != expected_size)
        {
                Q1BSPX_MarkUnsupported("RGBLIGHTING");
                Con_DWarning("BSPX: RGBLIGHTING size mismatch (%d bytes, expected %d)\n", size, expected_size);
                return;
        }

        memptr = (byte *)Hunk_AllocName(size, loadname);
        memcpy(memptr, buffer, size);

        mod->lightdata_rgb = memptr;
        mod->lightdata_rgb_size = size;
        mod->has_lightdata_rgb = true;

        Con_Printf("BSPX: loaded RGBLIGHTING (%d bytes)\n", size);
        Q1BSPX_MarkUsed("RGBLIGHTING");
}

void Mod_LoadFaceNormalsBSPX (qmodel_t *mod, void *buffer, int size)
{
        int expected_size;
        vec3_t *memptr;
        int i;

        if (!mod)
                return;

        expected_size = mod->numsurfaces * (int)sizeof(vec3_t);

        if (size != expected_size)
        {
                Con_Warning("BSPX: FACENORMALS size mismatch (%d bytes, expected %d)\n", size, expected_size);
                return;
        }

        memptr = (vec3_t *)Hunk_Alloc(size);
        memcpy(memptr, buffer, size);

        for (i = 0; i < mod->numsurfaces; i++)
                VectorNormalize(memptr[i]);

        mod->facenormals = memptr;
        mod->facenormals_count = mod->numsurfaces;
        mod->has_facenormals = true;

        Con_Printf("BSPX: loaded FACENORMALS (%d normals)\n", mod->facenormals_count);
        Q1BSPX_MarkUsed("FACENORMALS");
}

static qboolean Mod_ValidateLumpRange(const char *modelname, const char *lumpname, int fileofs, int filelen, size_t filelen_total)
{
        if (fileofs < 0 || filelen < 0)
        {
                Con_Warning("%s: invalid %s lump range (ofs=%d len=%d total=%zu)\n", modelname, lumpname, fileofs, filelen, filelen_total);
                return false;
        }

        if ((size_t)fileofs > filelen_total || (size_t)filelen > filelen_total - (size_t)fileofs)
        {
                Con_Warning("%s: invalid %s lump range (ofs=%d len=%d total=%zu)\n", modelname, lumpname, fileofs, filelen, filelen_total);
                return false;
        }

        return true;
}

static const char *Mod_StandardLumpName(int lumpindex)
{
        static const char *const names[HEADER_LUMPS] = {
                "ENTITIES",
                "PLANES",
                "TEXTURES",
                "VERTEXES",
                "VISIBILITY",
                "NODES",
                "TEXINFO",
                "FACES",
                "LIGHTING",
                "CLIPNODES",
                "LEAFS",
                "MARKSURFACES",
                "EDGES",
                "SURFEDGES",
                "MODELS"
        };

        if (lumpindex < 0 || lumpindex >= HEADER_LUMPS)
                return "UNKNOWN";

        return names[lumpindex];
}

static qboolean Mod_FindEndOfStandardLumps(const qmodel_t *mod, const lump_t *lumps, int numlumps, size_t filelen_total, qboolean *misaligned, size_t *end_offset)
{
        size_t  offs = 0;
        int             i;

        if (misaligned)
                *misaligned = false;

        if (end_offset)
                *end_offset = 0;

        for (i = 0; i < numlumps; i++)
        {
                if (!Mod_ValidateLumpRange(mod->name, Mod_StandardLumpName(i), lumps[i].fileofs, lumps[i].filelen, filelen_total))
                        return false;

                if (misaligned && (lumps[i].fileofs & 3) && i != LUMP_ENTITIES)
                        *misaligned = true;

                offs = q_max(offs, (size_t)lumps[i].fileofs + (size_t)lumps[i].filelen);
        }

        if (end_offset)
                *end_offset = (offs + 3) & ~((size_t)3);

        return true;
}

static qboolean Mod_ParseBSPHeader(const byte *buffer, size_t length, bsp_header_info_t *out)
{
        const lump_t *raw_lumps;
        size_t lump_start = sizeof(int);
        int version;
        int i;

        memset(out, 0, sizeof(*out));

        if (length < sizeof(int))
                return false;

        if (length >= 8 && !strncmp((const char *)buffer, "IBSP", 4))
        {
                version = LittleLong(*(int *)(buffer + sizeof(int)));
                lump_start = sizeof(int) * 2;
        }
        else if (length >= 8 && !strncmp((const char *)buffer, "BSP2X", 5))
        {
                version = LittleLong(*(int *)(buffer + sizeof(int)));
                lump_start = sizeof(int) * 2;
                out->has_bspx_header = true;
        }
        else
        {
                version = LittleLong(*(int *)buffer);
        }

        out->bspversion = version;
        out->is_bsp2 = (version == BSP2VERSION_2PSB || version == BSP2VERSION_BSP2);
        out->is_bsp29 = (version == BSPVERSION || version == BSPVERSION_QUAKE64);

        if (length < lump_start + sizeof(lump_t) * HEADER_LUMPS)
                return false;

        raw_lumps = (const lump_t *)(buffer + lump_start);
        for (i = 0; i < HEADER_LUMPS; i++)
        {
                out->lumps[i].fileofs = LittleLong(raw_lumps[i].fileofs);
                out->lumps[i].filelen = LittleLong(raw_lumps[i].filelen);
        }
        out->header_size = lump_start;

        return true;
}

static qboolean Mod_ParseBSPXDirectory(qmodel_t *mod, const bsp_header_info_t *header, size_t filelen)
{
        size_t                  offs;
        size_t                  header_offset;
        const bspx_header_disk_t        *h = NULL;
        qboolean                misaligned = false;
        int                             i, numlumps;

        Q1BSPX_ResetUsage();
        mod->bspx_entries = NULL;
        mod->bspx_entries_count = 0;
        mod->bspx_header = NULL;

        if (!Mod_FindEndOfStandardLumps(mod, header->lumps, HEADER_LUMPS, filelen, &misaligned, &offs))
                return false;

        header_offset = offs;

        if (misaligned)
                Con_DWarning("%s contains misaligned lumps\n", mod->name);

        if (offs + sizeof(*h) <= filelen)
        {
                const char *candidate = (const char *)mod_base + offs;

                if (!strncmp(candidate, "BSPX", 4) || !strncmp(candidate, "BSP2X", 5))
                        h = (const bspx_header_disk_t *)candidate;
        }

        if (!h && header->has_bspx_header && header->header_size + sizeof(*h) <= filelen)
        {
                const char *candidate = (const char *)mod_base + header->header_size;

                if (!strncmp(candidate, "BSPX", 4) || !strncmp(candidate, "BSP2X", 5))
                {
                        h = (const bspx_header_disk_t *)candidate;
                        header_offset = header->header_size;
                }
        }

        if (!h)
                return false;

        numlumps = LittleLong(h->numlumps);
        if (numlumps <= 0 || numlumps > MAX_BSPX_LUMPS)
        {
                Con_Warning("%s has invalid BSPX lump count %d (allowed 1..%d)\n", mod->name, numlumps, MAX_BSPX_LUMPS);
                return false;
        }

        if (header_offset + sizeof(*h) + sizeof(h->lumps[0]) * (numlumps - 1) > filelen)
                return false;

        for (i = 0; i < numlumps; i++)
        {
                int fileofs = LittleLong(h->lumps[i].fileofs);
                int filelen_lump = LittleLong(h->lumps[i].filelen);

                if (!Mod_ValidateLumpRange(mod->name, h->lumps[i].lumpname, fileofs, filelen_lump, filelen))
                        return false;

                if (fileofs & 3)
                        Con_DWarning("%s contains misaligned bspx lump %s\n", mod->name, h->lumps[i].lumpname);
        }

        mod->bspx_entries = (bspx_entry_t *)Hunk_AllocName(numlumps * sizeof(bspx_entry_t), loadname);
        mod->bspx_entries_count = numlumps;

        for (i = 0; i < numlumps; i++)
        {
                mod->bspx_entries[i].offset = LittleLong(h->lumps[i].fileofs);
                mod->bspx_entries[i].size = LittleLong(h->lumps[i].filelen);
                q_strlcpy(mod->bspx_entries[i].name, h->lumps[i].lumpname, sizeof(mod->bspx_entries[i].name));
        }

        Q1BSPX_RecordEntries(mod);

        return true;
}

static bspx_header_t *BSPX_Setup(qmodel_t *mod, const bsp_header_info_t *header, size_t filelen)
{
        if (Mod_ShouldSkipBrushExtras (mod))
                return NULL;

        if (!Mod_ParseBSPXDirectory(mod, header, filelen))
                return NULL;

	if (!mod->bspx_entries_count)
		return NULL;

	bspx_header_t *out = (bspx_header_t *)Hunk_AllocName(sizeof(*out), loadname);
	out->numlumps = mod->bspx_entries_count;
	out->lumps = mod->bspx_entries;
	return out;
}

static void *BSPX_FindLump(bspx_header_t *bspxheader, void *base, const char *lumpname, size_t *lumpsize)
{
        int i;

        if (lumpsize)
                *lumpsize = 0;

        if (!bspxheader || !bspxheader->lumps || !bspxheader->numlumps)
                return NULL;

        for (i = 0; i < bspxheader->numlumps; i++)
        {
                const bspx_entry_t *entry = &bspxheader->lumps[i];

                if (strncmp(entry->name, lumpname, sizeof(entry->name)))
                        continue;

                if (lumpsize)
                        *lumpsize = entry->size;

                return (byte *)base + entry->offset;
        }

        return NULL;
}

static qboolean BSPX_MarkKnown(qmodel_t *mod, void *data, int size)
{
        return true;
}

static qboolean BSPX_ApplyLumpOverride(qmodel_t *mod, const char *lumpname, lump_t *target, size_t filelen_total)
{
        size_t          size;
        void            *data;
        int                     offset;

        data = BSPX_FindLump(mod ? mod->bspx_header : NULL, mod_base, lumpname, &size);
        if (!data || !size)
                return false;

        offset = (int)((byte *)data - mod_base);
        if (!Mod_ValidateLumpRange(mod ? mod->name : "<unknown>", lumpname, offset, (int)size, filelen_total))
                return false;

        target->fileofs = offset;
        target->filelen = (int)size;
        Q1BSPX_MarkUsed(lumpname);
        return true;
}

typedef qboolean (*bspx_handler_t)(qmodel_t *mod, void *data, int size);
typedef struct
{
        const char *name;
        bspx_handler_t handler;
} bspx_handler_reg_t;

static void Mod_DispatchBSPXLumps(qmodel_t *mod)
{
        static const bspx_handler_reg_t bspx_handlers[] = {
                { "LIGHTRID", BSPX_LightGridLoad },
                { "LGHTGRID", BSPX_LightGridLoad },
                { "LIGHTGRID", BSPX_LightGridLoad },
                { "CUBEMAPS", BSPX_MarkKnown },
                { "MATERIALS", BSPX_MarkKnown },
                { "DECALS", BSPX_MarkKnown },
                { "MAPSCRIPTS", BSPX_MarkKnown },
                { "QC_EMBED", BSPX_MarkKnown },
                { "BSPX_SURFPROPS", BSPX_MarkKnown },
                { "BSPX_MODELNAMES", BSPX_MarkKnown },
                { NULL, NULL }
        };
        int i;

        if (!mod->bspx_entries_count)
                return;

        for (i = 0; i < mod->bspx_entries_count; i++)
        {
                const bspx_entry_t *entry = &mod->bspx_entries[i];
                const bspx_handler_reg_t *handler;
                qboolean handled = false;

                if (Q1BSPX_IsProcessed(entry->name))
                        continue;

                for (handler = bspx_handlers; handler->handler; handler++)
                {
                        /*
                         * Must be exact BSPX name equality (24-byte field semantics),
                         * not prefix matching.
                         */
                        if (BSPX_LumpNameEquals(entry->name, handler->name))
                        {
                                handled = true;
                                if (handler->handler(mod, mod_base + entry->offset, entry->size))
                                        Q1BSPX_MarkUsed(entry->name);
                                else
                                        Q1BSPX_MarkUnsupported(entry->name);
                                break;
                        }
                }

                if (!handled)
                        Q1BSPX_MarkUnsupported(entry->name);
        }
}

/*
=================
Mod_CheckFullbrights -- johnfitz
=================
*/
static qboolean Mod_CheckFullbrights (byte *pixels, int count)
{
	extern uint32_t is_fullbright[];
	int i;
	for (i = 0; i < count; i++)
	{
		if (GetBit (is_fullbright, *pixels++))
			return true;
	}
	return false;
}

/*
=================
Mod_CheckAnimTextureArrayQ64

Quake64 bsp
Check if we have any missing textures in the array
=================
*/
static qboolean Mod_CheckAnimTextureArrayQ64(texture_t *anims[], int numTex)
{
	int i;

	for (i = 0; i < numTex; i++)
	{
		if (!anims[i])
			return false;
	}
	return true;
}


/*
================
Mod_TextureTypeFromName
================
*/
static textype_t Mod_TextureTypeFromName (const char *texname)
{
        if (texname[0] == '*')
        {
		if (!strncmp (texname + 1, "lava",  4))	return TEXTYPE_LAVA;
		if (!strncmp (texname + 1, "slime", 5))	return TEXTYPE_SLIME;
		if (!strncmp (texname + 1, "tele",  4))	return TEXTYPE_TELE;
		return TEXTYPE_WATER;
	}

	if (texname[0] == '{')
		return TEXTYPE_CUTOUT;

	if (!q_strncasecmp (texname,"sky",3))
		return TEXTYPE_SKY;

        return TEXTYPE_DEFAULT;
}

static gltexture_t *Mod_LoadKTX2Texture(const char *name)
{
        byte *rawbuf;
        qfileofs_t filesize;
        gltexture_t *gltex;
        char ktx2path[MAX_OSPATH];

        q_snprintf(ktx2path, sizeof(ktx2path), "%s.ktx2", name);
        rawbuf = COM_LoadMallocFile(ktx2path, NULL);
        filesize = com_filesize;
        if (!rawbuf)
                return NULL;

        if (COM_HasExtension(ktx2path, ".ktx2"))
        {
                gltex = R_LoadKTX2Texture(ktx2path, rawbuf, (size_t)filesize);
                if (gltex)
                {
                        q_free(rawbuf);
                        return gltex;
                }
                Con_Printf("KTX2 load failed, falling back.\n");
        }

        q_free(rawbuf);
        return NULL;
}

/*
=================
Mod_LoadTextures
=================
*/
static void Mod_LoadTextures (lump_t *l)
{
	int		i, j, pixels, num, maxanim, altmax;
	miptex_t	*mt;
	texture_t	*tx, *tx2;
	texture_t	*anims[10];
	texture_t	*altanims[10];
	dmiptexlump_t	*m;
//johnfitz -- more variables
	char		texturename[64];
	int			nummiptex;
	int			mark, fwidth = 0, fheight = 0;
	char		filename[MAX_OSPATH], mapname[MAX_OSPATH];
	byte		*data;
	gltexture_t	*ktx2tex;
	enum srcformat fmt = SRC_RGBA;
	qboolean	malloced;
//johnfitz

	//johnfitz -- don't return early if no textures; still need to create dummy texture
	if (!l->filelen)
	{
		Con_Printf ("Mod_LoadTextures: no textures in bsp file\n");
		nummiptex = 0;
		m = NULL; // avoid bogus compiler warning
	}
	else
	{
		m = (dmiptexlump_t *)(mod_base + l->fileofs);
		m->nummiptex = LittleLong (m->nummiptex);
		nummiptex = m->nummiptex;
	}
	//johnfitz

	loadmodel->numtextures = nummiptex + 2; //johnfitz -- need 2 dummy texture chains for missing textures
	loadmodel->textures = (texture_t **) Hunk_AllocName (loadmodel->numtextures * sizeof(*loadmodel->textures) , loadname);

	for (i=0 ; i<nummiptex ; i++)
	{
		m->dataofs[i] = LittleLong(m->dataofs[i]);
		if (m->dataofs[i] == -1)
			continue;
		mt = (miptex_t *)((byte *)m + m->dataofs[i]);
		mt->width = LittleLong (mt->width);
		mt->height = LittleLong (mt->height);
		for (j=0 ; j<MIPLEVELS ; j++)
			mt->offsets[j] = LittleLong (mt->offsets[j]);

		if (mt->width == 0 || mt->height == 0)
		{
			Con_Warning ("Zero sized texture %s in %s!\n", mt->name, loadmodel->name);
			continue;
		}

		if ( (mt->width & 15) || (mt->height & 15) )
		{
			if (loadmodel->bspversion != BSPVERSION_QUAKE64)
				Con_Warning ("Texture %s (%d x %d) is not 16 aligned\n", mt->name, mt->width, mt->height);
		}

		pixels = mt->width*mt->height; // only copy the first mip, the rest are auto-generated
		tx = (texture_t *) Hunk_AllocNameNoFill (sizeof(texture_t) +pixels, loadname );
		// only clear the texture struct, not the pixel buffer following it
		memset (tx, 0, sizeof (*tx));
		loadmodel->textures[i] = tx;

		memcpy (tx->name, mt->name, sizeof(tx->name));
		if (!tx->name[0])
		{
			q_snprintf (tx->name, sizeof(tx->name), "unnamed%d", i);
			Con_Warning ("unnamed texture in %s, renaming to %s\n", loadmodel->name, tx->name);
		}
		tx->width = mt->width;
		tx->height = mt->height;
		// the pixels immediately follow the structures

		// ericw -- check for pixels extending past the end of the lump.
		// appears in the wild; e.g. jam2_tronyn.bsp (func_mapjam2),
		// kellbase1.bsp (quoth), and can lead to a segfault if we read past
		// the end of the .bsp file buffer
		if (((byte*)(mt+1) + pixels) > (mod_base + l->fileofs + l->filelen))
		{
			Con_DPrintf("Texture %s extends past end of lump\n", mt->name);
			pixels = q_max(0L, (long)((mod_base + l->fileofs + l->filelen) - (byte*)(mt+1)));
		}

			tx->fullbright = NULL; //johnfitz
			tx->emissive = NULL;
			tx->material = NULL;
			tx->material_map = NULL;
			tx->material_flags = 0u;
		tx->shift = 0;	// Q64 only
		tx->type = Mod_TextureTypeFromName (tx->name);

		if (loadmodel->bspversion != BSPVERSION_QUAKE64)
		{
			memcpy ( tx+1, mt+1, pixels);
		}
		else
		{ // Q64 bsp
			miptex64_t *mt64 = (miptex64_t *)mt;
			tx->shift = LittleLong (mt64->shift);
			memcpy ( tx+1, mt64+1, pixels);
		}

                //johnfitz -- lots of changes
                malloced = false;
                ktx2tex = NULL;
                if (!isDedicated) //no texture uploading for dedicated server
                {
                        if (!q_strncasecmp (loadmodel->name, "maps/", 5))
                                COM_StripExtension (loadmodel->name + 5, mapname, sizeof (mapname));
                        else
                                mapname[0] = '\0';
                        Material_ApplyToTexture (tx, mapname[0] ? mapname : NULL);

                        if (tx->type == TEXTYPE_SKY)
                        {
                                if (loadmodel->bspversion == BSPVERSION_QUAKE64)
					Sky_LoadTextureQ64 (loadmodel, tx);
				else
					Sky_LoadTexture (loadmodel, tx);
			}
                        else if (TEXTYPE_ISLIQUID (tx->type))
                        {
                                //external textures -- first look in "textures/mapname/" then look in "textures/"
                                mark = Hunk_LowMark();
                                if (tx->material_map && tx->material_map[0])
                                {
                                        COM_StripExtension (tx->material_map, filename, sizeof (filename));
                                        ktx2tex = Mod_LoadKTX2Texture (filename);
                                        data = ktx2tex ? NULL : Image_LoadImage (filename, &fwidth, &fheight, &fmt);
                                }
                                else
                                {
                                        if (mapname[0])
                                                q_snprintf (filename, sizeof(filename), "textures/%s/#%s", mapname, tx->name+1); //this also replaces the '*' with a '#'
                                        else
                                                q_snprintf (filename, sizeof(filename), "textures/#%s", tx->name+1);
                                        ktx2tex = Mod_LoadKTX2Texture (filename);
                                        data = ktx2tex ? NULL : Image_LoadImage (filename, &fwidth, &fheight, &fmt);
                                        if (!ktx2tex && !data && mapname[0])
                                        {
                                                q_snprintf (filename, sizeof(filename), "textures/#%s", tx->name+1);
                                                ktx2tex = Mod_LoadKTX2Texture (filename);
                                                if (!ktx2tex)
                                                        data = Image_LoadImage (filename, &fwidth, &fheight, &fmt);
                                        }
                                }

                                //now load whatever we found
                                if (ktx2tex)
                                {
                                        tx->gltexture = ktx2tex;
                                }
                                else if (data) //load external image
                                {
                                        tx->gltexture = TexMgr_LoadImage (loadmodel, filename, fwidth, fheight,
                                                fmt, data, filename, 0, TEXPREF_MIPMAP | TEXPREF_BINDLESS );
                                }
                                else //use the texture from the bsp file
                                {
                                        qboolean has_fullbright = Mod_CheckFullbrights ((byte *)(tx + 1), pixels);
                                        qboolean is_cutout = (tx->type == TEXTYPE_CUTOUT);
                                        texmgr_palmode_t palmode = TEXMGR_PALMODE_STANDARD;
                                        byte *rgba = (byte *) Hunk_AllocNameNoFill (pixels * 4, "tex_rgba");

                                        if (has_fullbright)
                                                palmode = is_cutout ? TEXMGR_PALMODE_NOBRIGHT : TEXMGR_PALMODE_ALPHABRIGHT;

                                        TexMgr_DecodeIndexedToRGBA ((byte *)(tx + 1), pixels, is_cutout ? 255 : -1, palmode, rgba);

                                        q_snprintf (texturename, sizeof(texturename), "%s:%s", loadmodel->name, tx->name);
                                        tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
                                                SRC_RGBA, rgba, "", (src_offset_t)rgba, TEXPREF_MIPMAP | TEXPREF_BINDLESS | (is_cutout ? TEXPREF_ALPHA : 0));

                                        if (has_fullbright && is_cutout)
                                        {
                                                byte *fb_rgba = (byte *) Hunk_AllocNameNoFill (pixels * 4, "tex_fbright");
                                                TexMgr_BuildFullbrightRGBA ((byte *)(tx + 1), pixels, 255, fb_rgba);
                                                q_snprintf (texturename, sizeof(texturename), "%s:%s_glow", loadmodel->name, tx->name);
                                                tx->fullbright = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
                                                        SRC_RGBA, fb_rgba, "", (src_offset_t)fb_rgba, TEXPREF_MIPMAP | TEXPREF_BINDLESS);
                                        }
                                }
                                if (malloced)
                                        q_free(data);
                                Hunk_FreeToLowMark (mark);
                        }
                        else //regular texture
                        {
                                int     extraflags = TEXPREF_BINDLESS;
                                if (tx->type == TEXTYPE_CUTOUT)
                                        extraflags |= TEXPREF_ALPHA;
                                // ericw

                                //external textures -- first look in "textures/mapname/" then look in "textures/"
                                mark = Hunk_LowMark ();
                                if (tx->material_map && tx->material_map[0])
                                {
                                        COM_StripExtension (tx->material_map, filename, sizeof (filename));
                                        ktx2tex = Mod_LoadKTX2Texture (filename);
                                        data = ktx2tex ? NULL : Image_LoadImage (filename, &fwidth, &fheight, &fmt);
                                }
                                else
                                {
                                        if (mapname[0])
                                                q_snprintf (filename, sizeof(filename), "textures/%s/%s", mapname, tx->name);
                                        else
                                                q_snprintf (filename, sizeof(filename), "textures/%s", tx->name);
                                        ktx2tex = Mod_LoadKTX2Texture (filename);
                                        data = ktx2tex ? NULL : Image_LoadImage (filename, &fwidth, &fheight, &fmt);
                                        if (!ktx2tex && !data && mapname[0])
                                        {
                                                q_snprintf (filename, sizeof(filename), "textures/%s", tx->name);
                                                ktx2tex = Mod_LoadKTX2Texture (filename);
                                                if (!ktx2tex)
                                                        data = Image_LoadImage (filename, &fwidth, &fheight, &fmt);
                                        }
                                }

                                //now load whatever we found
                                if (ktx2tex)
                                {
                                        tx->gltexture = ktx2tex;
                                }
                                else if (data) //load external image
                                {
                                        tx->gltexture = TexMgr_LoadImage (loadmodel, filename, fwidth, fheight,
                                                fmt, data, filename, 0, TEXPREF_MIPMAP | extraflags );
                                }
                                else //use the texture from the bsp file
                                {
                                        qboolean has_fullbright = Mod_CheckFullbrights ((byte *)(tx + 1), pixels);
                                        qboolean is_cutout = (tx->type == TEXTYPE_CUTOUT);
                                        texmgr_palmode_t palmode = TEXMGR_PALMODE_STANDARD;
                                        byte *rgba = (byte *) Hunk_AllocNameNoFill (pixels * 4, "tex_rgba");

                                        if (has_fullbright)
                                                palmode = is_cutout ? TEXMGR_PALMODE_NOBRIGHT : TEXMGR_PALMODE_ALPHABRIGHT;

                                        TexMgr_DecodeIndexedToRGBA ((byte *)(tx + 1), pixels, is_cutout ? 255 : -1, palmode, rgba);

                                        q_snprintf (texturename, sizeof(texturename), "%s:%s", loadmodel->name, tx->name);
                                        tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
                                                SRC_RGBA, rgba, "", (src_offset_t)rgba, TEXPREF_MIPMAP | extraflags | (is_cutout ? TEXPREF_ALPHA : 0));

                                        if (has_fullbright && is_cutout)
                                        {
                                                byte *fb_rgba = (byte *) Hunk_AllocNameNoFill (pixels * 4, "tex_fbright");
                                                TexMgr_BuildFullbrightRGBA ((byte *)(tx + 1), pixels, 255, fb_rgba);
                                                q_snprintf (texturename, sizeof(texturename), "%s:%s_glow", loadmodel->name, tx->name);
                                                tx->fullbright = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
                                                        SRC_RGBA, fb_rgba, "", (src_offset_t)fb_rgba, TEXPREF_MIPMAP | extraflags);
                                        }
                                }
                                if (malloced)
                                        q_free(data);
                                Hunk_FreeToLowMark (mark);
                        }
                }
                //johnfitz
        }

	//johnfitz -- last 2 slots in array should be filled with dummy textures
	loadmodel->textures[loadmodel->numtextures-2] = r_notexture_mip; //for lightmapped surfs
	loadmodel->textures[loadmodel->numtextures-1] = r_notexture_mip2; //for SURF_DRAWTILED surfs

//
// sequence the animations
//
	for (i=0 ; i<nummiptex ; i++)
	{
		tx = loadmodel->textures[i];
		if (!tx || tx->name[0] != '+')
			continue;
		if (tx->anim_next)
			continue;	// already sequenced

	// find the number of frames in the animation
		memset (anims, 0, sizeof(anims));
		memset (altanims, 0, sizeof(altanims));

		maxanim = tx->name[1];
		altmax = 0;
		if (maxanim >= 'a' && maxanim <= 'z')
			maxanim -= 'a' - 'A';
		if (maxanim >= '0' && maxanim <= '9')
		{
			maxanim -= '0';
			altmax = 0;
			anims[maxanim] = tx;
			maxanim++;
		}
		else if (maxanim >= 'A' && maxanim <= 'J')
		{
			altmax = maxanim - 'A';
			maxanim = 0;
			altanims[altmax] = tx;
			altmax++;
		}
		else
			Sys_Error ("Bad animating texture %s", tx->name);

		for (j=i+1 ; j<nummiptex ; j++)
		{
			tx2 = loadmodel->textures[j];
			if (!tx2 || tx2->name[0] != '+')
				continue;
			if (strcmp (tx2->name+2, tx->name+2))
				continue;

			num = tx2->name[1];
			if (num >= 'a' && num <= 'z')
				num -= 'a' - 'A';
			if (num >= '0' && num <= '9')
			{
				num -= '0';
				anims[num] = tx2;
				if (num+1 > maxanim)
					maxanim = num + 1;
			}
			else if (num >= 'A' && num <= 'J')
			{
				num = num - 'A';
				altanims[num] = tx2;
				if (num+1 > altmax)
					altmax = num+1;
			}
			else
				Sys_Error ("Bad animating texture %s", tx->name);
		}

		if (loadmodel->bspversion == BSPVERSION_QUAKE64 && !Mod_CheckAnimTextureArrayQ64(anims, maxanim))
			continue; // Just pretend this is a normal texture

#define	ANIM_CYCLE	2
	// link them all together
		for (j=0 ; j<maxanim ; j++)
		{
			tx2 = anims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s",j, tx->name);
			tx2->anim_total = maxanim * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = anims[ (j+1)%maxanim ];
			if (altmax)
				tx2->alternate_anims = altanims[0];
		}
		for (j=0 ; j<altmax ; j++)
		{
			tx2 = altanims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s",j, tx->name);
			tx2->anim_total = altmax * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = altanims[ (j+1)%altmax ];
			if (maxanim)
				tx2->alternate_anims = anims[0];
		}
	}
}

/*
=================
Mod_LoadLighting -- johnfitz -- replaced with lit support code via lordhavoc
=================
*/
static void Mod_LoadLighting (lump_t *l)
{
	int i, mark;
	byte *in, *out, *data;
	byte *bspx_lighting;
	byte *bspx_rgblighting;
	byte *bspx_dlit;
	byte d, q64_b0, q64_b1;
	char litfilename[MAX_OSPATH];
	const char *lighting_source;
	unsigned int path_id = 0;
	int	bspxsize, bspx_lighting_size, bspx_rgb_size, bspx_dlit_size;
	int remastered_lump_samples;
	qboolean bsp_lightmap_bgr = false;
	qboolean litfile_bgr = false;
	const char *lightmap_source_name = "NONE";

	loadmodel->lightdata = NULL;
	loadmodel->lightdatasamples = 0;
	loadmodel->lightdirdata = NULL;
	loadmodel->lightdirsamples = 0;
	loadmodel->lightdata_rgb = NULL;
	loadmodel->lightdata_rgb_size = 0;
	loadmodel->has_lightdata_rgb = false;
	loadmodel->lightdatasize = l->filelen;
	loadmodel->flags &= ~MOD_HDRLIGHTING;
	loadmodel->litfile = false;
	bspx_lighting = Q1BSPX_FindLump("LIGHTING", &bspx_lighting_size);
	bspx_rgblighting = Q1BSPX_FindLump("RGBLIGHTING", &bspx_rgb_size);
	bspx_dlit = Q1BSPX_FindLump("DLIT", &bspx_dlit_size);

	if (gl_bsp_lightmap_bgr.value > 0.f)
		bsp_lightmap_bgr = true;
	else if (gl_bsp_lightmap_bgr.value < 0.f)
		bsp_lightmap_bgr = Mod_ShouldAutoSwapBspLighting(loadmodel);

	/* External .lit files are authored RGB/RGBE in most packs.
	 * Keep auto BGR detection scoped to embedded BSP/BSPX lighting only. */
	if (gl_bsp_lightmap_bgr.value > 0.f)
		litfile_bgr = true;

	/*
	 * Lighting source precedence for loadmodel->lightdata:
	 * 1) external .lit
	 * 2) BSPX LIGHTING_E5BGR9
	 * 3) BSPX RGBLIGHTING
	 * 4) BSPX DLIT
	 * 5) BSPX LIGHTING (classic bytes)
	 * 6) embedded BSP LUMP_LIGHTING (classic bytes)
	 */
	// LordHavoc: check for a .lit file
	q_strlcpy(litfilename, loadmodel->name, sizeof(litfilename));
	COM_StripExtension(litfilename, litfilename, sizeof(litfilename));
	q_strlcat(litfilename, ".lit", sizeof(litfilename));
	lighting_source = litfilename;
	mark = Hunk_LowMark();
	data = NULL;

	if (gl_loadlitfiles.value >= 1) // woods #loadlits #litdir
	{
		char altlitfilename[MAX_OSPATH];
		qboolean try_external = false;

		// Check if we should try external lits first
		if (gl_loadlitfiles.value >= 2 && external_lits_dir.string[0])
		{
			q_snprintf(altlitfilename, sizeof(altlitfilename), "maps/%s/%s",
				external_lits_dir.string, COM_SkipPath(litfilename));

			if (gl_loadlitfiles.value == 2 ||
				(gl_loadlitfiles.value == 3 && (rand() & 1)))
			{
				try_external = true;
			}

			if (try_external && COM_FileExists(altlitfilename, NULL))
			{
				Con_DPrintf2("trying to load %s\n", altlitfilename);
				data = (byte*)COM_LoadHunkFile(altlitfilename, &path_id);
				if (data)
					lighting_source = altlitfilename;
			}
		}

		// Load standard .lit file if no external data loaded
		if (!data)
		{
			data = (byte*)COM_LoadHunkFile(litfilename, &path_id);
			lighting_source = litfilename;
		}
	}
	if (data)
	{
		// use lit file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < loadmodel->path_id)
		{
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("ignored %s from a gamedir with lower priority\n", lighting_source);
		}
		else
		if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
		{
			i = LittleLong(((int *)data)[1]);
			if (i == 1)
			{
				if (8+l->filelen*3 == com_filesize)
				{
					Con_Printf("loaded %s lighting (ldr)\n", lighting_source);

					if (litfile_bgr)
					{
						byte *tmp = (byte *)q_malloc(l->filelen * 3);
						byte *hunkbuf;
						const byte *src;
						byte *dst;

						if (!tmp)
						{
							Hunk_FreeToLowMark(mark);
							Sys_Error("Mod_LoadLighting: failed to allocate %d bytes for BGR swap",
								l->filelen * 3);
						}

						src = data + 8;
						dst = tmp;
						for (int s = 0; s < l->filelen; s++, src += 3, dst += 3)
						{
							dst[0] = src[2];
							dst[1] = src[1];
							dst[2] = src[0];
						}

						Hunk_FreeToLowMark(mark);
						hunkbuf = (byte *)Hunk_AllocName(l->filelen * 3, litfilename);
						memcpy(hunkbuf, tmp, l->filelen * 3);
						q_free(tmp);
						loadmodel->lightdata = hunkbuf;
						lightmap_source_name = "LIT_V1_BGR->RGB";
					}
					else
					{
						loadmodel->lightdata = data + 8;
						lightmap_source_name = "LIT_V1";
					}

					loadmodel->lightdatasamples = l->filelen;
					loadmodel->litfile = true;
					Con_Printf("LIT: loaded %s (%u texels) from %s\n",
						lightmap_source_name, loadmodel->lightdatasamples, lighting_source);
					goto loadlightdir;
                                }
				Hunk_FreeToLowMark(mark);
				Con_Printf("Outdated .lit file (%s should be %u bytes, not %u)\n", lighting_source, 8+l->filelen*3, (unsigned)com_filesize);
			}
			else if (i == 0x10001)
			{
                                if (8+l->filelen*4 == com_filesize)
                                {
                                        byte *rgbe = (byte *)q_malloc(l->filelen * 4);
                                        byte *decoded = NULL;

                                        if (!rgbe)
                                        {
                                                Hunk_FreeToLowMark(mark);
                                                Sys_Error("Mod_LoadLighting: failed to allocate %d bytes for hdr temp buffer",
                                                        l->filelen * 4);
                                        }

                                        memcpy(rgbe, data + 8, l->filelen * 4);
                                        Hunk_FreeToLowMark(mark);

                                        decoded = (byte *)Hunk_AllocName(l->filelen * 3, litfilename);

                                        Con_Printf("loaded %s lighting (hdr)\n", lighting_source);
                                        Mod_DecodeRgbeLighting(decoded, rgbe, l->filelen, litfile_bgr);

                                        q_free(rgbe);

                                        loadmodel->lightdata = decoded;
                                        loadmodel->lightdatasamples = l->filelen;
                                        loadmodel->litfile = true;
					lightmap_source_name = "LIT_V2_RGBE";

                                        goto loadlightdir;
                                }
				Hunk_FreeToLowMark(mark);
				Con_Printf("Outdated .lit file (%s should be %u bytes, not %u)\n", lighting_source, 8+l->filelen*4, (unsigned)com_filesize);
			}
			else
			{
				Hunk_FreeToLowMark(mark);
				Con_Printf("Unknown .lit file version (%d)\n", i);
			}
		}
		else
		{
			Hunk_FreeToLowMark(mark);
			Con_Printf("Corrupt .lit file (old version?), ignoring\n");
		}
	}
	/*
	 * Quake Remastered BSP29 stores colored lightmaps in 4-byte BGRA-like records
	 * in the LIGHTING lump. Detect and convert to RGB here so uploads stay GL_RGB.
	 */
	remastered_lump_samples = (l->filelen > 0 && (l->filelen % 4) == 0) ? (l->filelen / 4) : 0;
	if (!loadmodel->lightdata && bsp_lightmap_bgr && remastered_lump_samples > 0)
	{
		const byte *src = mod_base + l->fileofs;

		if (!Mod_RgbExponentLooksLikeAlpha(src, remastered_lump_samples))
		{
			loadmodel->lightdata = (byte *)Hunk_AllocName(remastered_lump_samples * 3, litfilename);
			loadmodel->lightdatasamples = remastered_lump_samples;
			loadmodel->litfile = true;
			out = loadmodel->lightdata;

			for (i = 0; i < remastered_lump_samples; i++)
			{
				const byte *pix = src + i * 4;
				/* BGRA/BGRX -> RGB, keep alpha/exponent byte ignored. */
				*out++ = pix[2];
				*out++ = pix[1];
				*out++ = pix[0];
			}

			lightmap_source_name = "REMASTERED_BGRA";
			goto loadlightdir;
		}
		else
		{
			byte *decoded = (byte *)Hunk_AllocName(remastered_lump_samples * 3, litfilename);

			Mod_DecodeRgbeLighting(decoded, src, remastered_lump_samples, bsp_lightmap_bgr);
			loadmodel->lightdata = decoded;
			loadmodel->lightdatasamples = remastered_lump_samples;
			loadmodel->litfile = true;
			loadmodel->flags |= MOD_HDRLIGHTING;
			lightmap_source_name = "REMASTERED_RGBE";
			goto loadlightdir;
		}
	}


	// LordHavoc: no .lit found, expand the white lighting data to color
	if (!l->filelen && !(bspx_lighting && bspx_lighting_size > 0))
		goto loadlightdir;

        if (gl_loadlitfiles.value > 0) // woods #loadlits
        {
                in = Q1BSPX_FindLump("LIGHTING_E5BGR9", &bspxsize);
                if (in && bspxsize && (bspxsize % 4 == 0))
                {
                        int samples = bspxsize / 4;
                        loadmodel->lightdata = (byte*)Hunk_AllocName(samples * 3, litfilename);
                        loadmodel->lightdatasamples = samples;
                        loadmodel->litfile = true;
                        Q1BSPX_DecodeE5BGR9Lighting(loadmodel->lightdata, (const unsigned int *)in, samples);
                        Q1BSPX_MarkUsed("LIGHTING_E5BGR9");
					Con_Printf("loaded BSPX HDR lighting (E5BGR9)\n");
					lightmap_source_name = "BSPX_LIGHTING_E5BGR9";
                        goto loadlightdir;
                }
                else if (in)
                {
                        Q1BSPX_MarkUnsupported("LIGHTING_E5BGR9");
                        Con_DWarning("LIGHTING_E5BGR9 lump size %d is not a multiple of 4 bytes\n", bspxsize);
                }

                if (bspx_rgblighting && (bspx_rgb_size % 3 == 0))
                {
                        int samples = bspx_rgb_size / 3;

			loadmodel->lightdata = (byte*)Hunk_AllocName(bspx_rgb_size, litfilename);
                        loadmodel->lightdatasamples = samples;
                        loadmodel->litfile = true;

			memcpy(loadmodel->lightdata, bspx_rgblighting, bspx_rgb_size);
                        Q1BSPX_MarkUsed("RGBLIGHTING");

					Con_Printf("loaded BSPX lighting (%d samples)\n", samples);
					lightmap_source_name = "BSPX_RGBLIGHTING";
                        goto loadlightdir;
                }
                else if (bspx_rgblighting)
                {
                        Q1BSPX_MarkUnsupported("RGBLIGHTING");
			Con_DWarning("RGBLIGHTING lump size %d is not a multiple of 3 bytes\n", bspx_rgb_size);
                }

        }
        else {
                Con_DPrintf2("gl_loadlitfiles 0: ignoring BSPX colored lighting lumps\n");
        }

        if (bspx_dlit && bspx_dlit_size > 0 && !loadmodel->lightdata)
        {
                int samples = bspx_dlit_size / 6;

                if (bspx_dlit_size % 6)
                {
                        Q1BSPX_MarkUnsupported("DLIT");
                        Con_DWarning("DLIT lump size %d is not a multiple of 6 bytes\n", bspx_dlit_size);
                }
                else
                {
                        byte *luxout;

                        loadmodel->lightdata = (byte *)Hunk_AllocName(samples * 3, litfilename);
                        loadmodel->lightdatasamples = samples;
                        loadmodel->lightdirdata = (byte *)Hunk_AllocName(samples * 3, litfilename);
                        loadmodel->lightdirsamples = samples;
                        in = bspx_dlit;
                        out = loadmodel->lightdata;
                        luxout = loadmodel->lightdirdata;

                        for (int sample = 0; sample < samples; sample++)
                        {
						*out++ = *in++;
						*out++ = *in++;
						*out++ = *in++;
						/* DLIT color bytes are RGB, followed by LIGHTINGDIR XYZ bytes. */

						*luxout++ = *in++;
						*luxout++ = *in++;
						*luxout++ = *in++;
                        }

                        Q1BSPX_MarkUsed("DLIT");
					lightmap_source_name = "BSPX_DLIT";
                        goto loadlightdir;
                }
        }


	if (bspx_lighting && bspx_lighting_size > 0)
	{
		loadmodel->lightdata = (byte *) Hunk_AllocName (bspx_lighting_size * 3, litfilename);
		loadmodel->lightdatasamples = bspx_lighting_size;
		in = loadmodel->lightdata + bspx_lighting_size * 2;
		out = loadmodel->lightdata;
		memcpy (in, bspx_lighting, bspx_lighting_size);
		for (unsigned int sample = 0; sample < (unsigned int)bspx_lighting_size; sample++)
		{
			d = *in++;
			*out++ = d;
			*out++ = d;
			*out++ = d;
		}
		Q1BSPX_MarkUsed("LIGHTING");
		lightmap_source_name = "BSPX_LIGHTING";
		goto loadlightdir;
	}

        // Quake64 bsp lighmap data
        if (loadmodel->bspversion == BSPVERSION_QUAKE64)
        {
                // RGB lightmap samples are packed in 16bits.
                // RRRRR GGGGG BBBBBB

                loadmodel->lightdata = (byte *) Hunk_AllocName ( (l->filelen / 2)*3, litfilename);
                loadmodel->lightdatasamples = (l->filelen / 2);
                loadmodel->litfile = true;
                in = mod_base + l->fileofs;
                out = loadmodel->lightdata;

                for (unsigned int sample = 0; sample < (unsigned int)(l->filelen / 2); sample++)
                {
                        q64_b0 = *in++;
                        q64_b1 = *in++;

                        *out++ = q64_b0 & 0xf8;/* 0b11111000 */
                        *out++ = ((q64_b0 & 0x07) << 5) + ((q64_b1 & 0xc0) >> 5);/* 0b00000111, 0b11000000 */
                        *out++ = (q64_b1 & 0x3f) << 2;/* 0b00111111 */
                }
                lightmap_source_name = "BSP_Q64";
                goto loadlightdir;
        }

	if (l->filelen)
	{
		loadmodel->lightdata = (byte *) Hunk_AllocName ( l->filelen*3, litfilename);
		loadmodel->lightdatasamples = l->filelen;
		in = loadmodel->lightdata + l->filelen*2; // place the file at the end, so it will not be overwritten until the very last write
		out = loadmodel->lightdata;
		memcpy (in, mod_base + l->fileofs, l->filelen);
                for (unsigned int sample = 0; sample < (unsigned int)l->filelen; sample++)
		{
			d = *in++;
			*out++ = d;
			*out++ = d;
			*out++ = d;
		}
		lightmap_source_name = "BSP_LIGHTING";
		goto loadlightdir;
	}


loadlightdir:
	if (bspx_rgblighting && loadmodel->lightdatasamples && !loadmodel->litfile)
		Mod_LoadRGBLightingBSPX(loadmodel, bspx_rgblighting, bspx_rgb_size);

	if (!loadmodel->lightdirdata)
	{
		in = Q1BSPX_FindLump("LIGHTINGDIR", &bspxsize);
		if (in && bspxsize > 0)
		{
			int samples = bspxsize / 3;
			int expected_samples = loadmodel->lightdatasamples ? loadmodel->lightdatasamples : l->filelen;
			qboolean sample_count_ok = true;

			if (bspxsize % 3)
			{
				Q1BSPX_MarkUnsupported("LIGHTINGDIR");
				Con_DWarning("LIGHTINGDIR lump size %d is not a multiple of 3 bytes\n", bspxsize);
			}
			else
			{
				if (expected_samples && samples != expected_samples)
				{
					Con_DWarning("LIGHTINGDIR lump has %d samples, expected %d\n", samples, expected_samples);
					sample_count_ok = false;
				}

				if (sample_count_ok)
				{
					loadmodel->lightdirdata = (byte *) Hunk_AllocNameNoFill (bspxsize, litfilename);
					loadmodel->lightdirsamples = samples;
					memcpy (loadmodel->lightdirdata, in, bspxsize);
					Q1BSPX_MarkUsed("LIGHTINGDIR");
				}
				else
					Q1BSPX_MarkUnsupported("LIGHTINGDIR");
			}
		}
	}

	Con_DPrintf("Lightmap: source=%s fmt=%s texels=%d\n",
		lightmap_source_name,
		bsp_lightmap_bgr ? "BGR->RGB" : "RGB",
		loadmodel->lightdatasamples);
	return;
}
/*
=================
Mod_LoadVisibility
=================
*/
static void Mod_LoadVisibility (lump_t *l)
{
	loadmodel->viswarn = false;
	if (!l->filelen)
	{
		loadmodel->visdata = NULL;
		return;
	}
	loadmodel->visdata = (byte *) Hunk_AllocNameNoFill ( l->filelen, loadname);
	memcpy (loadmodel->visdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadEntities
=================
*/
static void Mod_LoadEntities (lump_t *l)
{
	char	basemapname[MAX_QPATH];
	char	entfilename[MAX_QPATH];
	char		*ents;
	int		mark;
	unsigned int	path_id;
	unsigned int	crc = 0;

	if (! external_ents.value)
		goto _load_embedded;

	mark = Hunk_LowMark();
	if (l->filelen > 0) {
		crc = CRC_Block(mod_base + l->fileofs, l->filelen - 1);
	}

	q_strlcpy(basemapname, loadmodel->name, sizeof(basemapname));
	COM_StripExtension(basemapname, basemapname, sizeof(basemapname));

	q_snprintf(entfilename, sizeof(entfilename), "%s@%04x.ent", basemapname, crc);
	Con_DPrintf2("trying to load %s\n", entfilename);
	ents = (char *) COM_LoadHunkFile (entfilename, &path_id);

	if (!ents)
	{
		q_snprintf(entfilename, sizeof(entfilename), "%s.ent", basemapname);
		Con_DPrintf2("trying to load %s\n", entfilename);
		ents = (char *) COM_LoadHunkFile (entfilename, &path_id);
	}

	if (ents)
	{
		// use ent file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < loadmodel->path_id)
		{
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("ignored %s from a gamedir with lower priority\n", entfilename);
		}
		else
		{
			loadmodel->entities = ents;
			Con_DPrintf("Loaded external entity file %s\n", entfilename);
			return;
		}
	}

_load_embedded:
	if (!l->filelen)
	{
		loadmodel->entities = NULL;
		return;
	}
	loadmodel->entities = (char *) Hunk_AllocNameNoFill ( l->filelen, loadname);
	memcpy (loadmodel->entities, mod_base + l->fileofs, l->filelen);
}

static qboolean Mod_ParseWorldspawnKey (qmodel_t *mod, const char *key, char *value, size_t valuesize)
{
	const char *data;

	if (!mod || !mod->entities || !key || !value || valuesize == 0)
		return false;

	data = COM_Parse (mod->entities);
	if (!data || com_token[0] != '{')
		return false;

	while (1)
	{
		data = COM_Parse (data);
		if (!data || !com_token[0])
			return false;
		if (com_token[0] == '}')
			return false;

		if (!strcmp (com_token, "classname"))
		{
			data = COM_Parse (data);
			if (!data || strcmp (com_token, "worldspawn"))
				return false;
			continue;
		}

		if (!strcmp (com_token, key))
		{
			data = COM_Parse (data);
			if (!data)
				return false;
			q_strlcpy (value, com_token, valuesize);
			return true;
		}

		data = COM_Parse (data);
		if (!data)
			return false;
	}
}


/*
=================
Mod_LoadVertexes
=================
*/
static void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t		*in;
	mvertex_t		*out;
	int			i, count;
	int			bspxsize;
	const float	*normal_in = NULL;

	loadmodel->vertexes = NULL;

	in = (dvertex_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (mvertex_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	{
		void *normal_lump = Q1BSPX_FindLump("VERTEXNORMALS", &bspxsize);
		if (normal_lump)
		{
			if (bspxsize == count * sizeof(vec3_t))
			{
				Q1BSPX_MarkUsed("VERTEXNORMALS");
				normal_in = (const float *)normal_lump;
			}
			else
				Q1BSPX_MarkUnsupported("VERTEXNORMALS");
		}
	}

	for (i=0 ; i<count ; i++, in++, out++)
	{
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);

		if (normal_in)
		{
			out->normal[0] = LittleFloat (*normal_in++);
			out->normal[1] = LittleFloat (*normal_in++);
			out->normal[2] = LittleFloat (*normal_in++);
		}
		else
			VectorClear(out->normal);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
static int32_t Mod_ReadInt16(byte *base, int offset)
{
        return LittleShort(*(int16_t *)(base + offset));
}

static int32_t Mod_ReadUInt16(byte *base, int offset)
{
        return (unsigned short)LittleShort(*(int16_t *)(base + offset));
}

static int32_t Mod_ReadInt32(byte *base, int offset)
{
        return LittleLong(*(int32_t *)(base + offset));
}

static void Mod_LoadEdges (lump_t *l)
{
        byte *in = mod_base + l->fileofs;
        medge_t *out;
        int     i, count;

        if (l->filelen % bsp_loader.edge_stride)
                Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);

        count = l->filelen / bsp_loader.edge_stride;
        out = (medge_t *) Hunk_AllocNameNoFill ( (count + 1) * sizeof(*out), loadname);

        loadmodel->edges = out;
        loadmodel->numedges = count;

        for (i=0 ; i<count ; i++, out++)
        {
                size_t offset = i * bsp_loader.edge_stride;
                out->v[0] = bsp_loader.read_edge_index(in, offset);
                out->v[1] = bsp_loader.read_edge_index(in, offset + bsp_loader.edge_vertex_step);
        }
}

static void Mod_SetupBspLoader(qboolean bsp2_version, qboolean bsp29)
{
        bsp_loader.is_bsp29 = bsp29;

        if (bsp2_version)
        {
                bsp_loader.is_bsp2 = true;
                bsp_loader.read_edge_index = Mod_ReadInt32;
                bsp_loader.read_face_numedges = Mod_ReadInt32;
                bsp_loader.read_face_planenum = Mod_ReadInt32;
                bsp_loader.read_face_side = Mod_ReadInt32;
                bsp_loader.read_face_texinfo = Mod_ReadInt32;
                bsp_loader.edge_stride = sizeof(dledge_t);
                bsp_loader.edge_vertex_step = sizeof(((dledge_t *)0)->v[0]);
                bsp_loader.face_stride = sizeof(dlface_t);
                bsp_loader.face_side_offset = offsetof(dlface_t, side);
                bsp_loader.face_firstedge_offset = offsetof(dlface_t, firstedge);
                bsp_loader.face_numedges_offset = offsetof(dlface_t, numedges);
                bsp_loader.face_texinfo_offset = offsetof(dlface_t, texinfo);
                bsp_loader.face_styles_offset = offsetof(dlface_t, styles);
                bsp_loader.face_lightofs_offset = offsetof(dlface_t, lightofs);
        }
        else
        {
                bsp_loader.is_bsp2 = false;
                bsp_loader.read_edge_index = Mod_ReadUInt16;
                bsp_loader.read_face_numedges = Mod_ReadUInt16;
                bsp_loader.read_face_planenum = Mod_ReadInt16;
                bsp_loader.read_face_side = Mod_ReadInt16;
                bsp_loader.read_face_texinfo = Mod_ReadInt16;
                bsp_loader.edge_stride = sizeof(dsedge_t);
                bsp_loader.edge_vertex_step = sizeof(((dsedge_t *)0)->v[0]);
                bsp_loader.face_stride = sizeof(dsface_t);
                bsp_loader.face_side_offset = offsetof(dsface_t, side);
                bsp_loader.face_firstedge_offset = offsetof(dsface_t, firstedge);
                bsp_loader.face_numedges_offset = offsetof(dsface_t, numedges);
                bsp_loader.face_texinfo_offset = offsetof(dsface_t, texinfo);
                bsp_loader.face_styles_offset = offsetof(dsface_t, styles);
                bsp_loader.face_lightofs_offset = offsetof(dsface_t, lightofs);
        }
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void Mod_LoadTexinfo (lump_t *l)
{
	texinfo_t *in;
	mtexinfo_t *out;
	int	i, j, count, miptex;
	int missing = 0; //johnfitz

	in = (texinfo_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (mtexinfo_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<4 ; j++)
		{
			out->vecs[0][j] = LittleFloat (in->vecs[0][j]);
			out->vecs[1][j] = LittleFloat (in->vecs[1][j]);
		}

		miptex = LittleLong (in->miptex);
		out->flags = LittleLong (in->flags);
		out->miptex = miptex;

		//johnfitz -- rewrote this section
		if (miptex >= loadmodel->numtextures-1 || !loadmodel->textures[miptex])
		{
			if (out->flags & TEX_SPECIAL)
				out->texnum = loadmodel->numtextures-1;
			else
				out->texnum = loadmodel->numtextures-2;
			out->flags |= TEX_MISSING;
			missing++;
		}
		else
		{
			out->texnum = miptex;
		}
		//johnfitz
	}

	//johnfitz: report missing textures
	if (missing && loadmodel->numtextures > 1)
		Con_Printf ("Mod_LoadTexinfo: %d texture(s) missing from BSP file\n", missing);
	//johnfitz
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void CalcSurfaceExtents (msurface_t *s)
{
	float	mins[2], maxs[2], val;
	int		i,j, e;
	mvertex_t	*v;
	mtexinfo_t	*tex;
	double	texvecs[2][4];

	mins[0] = mins[1] = FLT_MAX;
	maxs[0] = maxs[1] = -FLT_MAX;

	tex = s->texinfo;

#ifdef USE_SSE2
	{
		__m128 tv0 = _mm_loadu_ps (tex->vecs[0]);
		__m128 tv1 = _mm_loadu_ps (tex->vecs[1]);
		_mm_storeu_pd (&texvecs[0][0], _mm_cvtps_pd (tv0));
		_mm_storeu_pd (&texvecs[0][2], _mm_cvtps_pd (_mm_shuffle_ps (tv0, tv0, _MM_SHUFFLE (1, 0, 3, 2))));
		_mm_storeu_pd (&texvecs[1][0], _mm_cvtps_pd (tv1));
		_mm_storeu_pd (&texvecs[1][2], _mm_cvtps_pd (_mm_shuffle_ps (tv1, tv1, _MM_SHUFFLE (1, 0, 3, 2))));
	}
#else
	for (i=0 ; i<4 ; i++)
	{
		texvecs[0][i] = (double) tex->vecs[0][i];
		texvecs[1][i] = (double) tex->vecs[1][i];
	}
#endif

	for (i=0 ; i<s->numedges ; i++)
	{
		double vposition[3];

		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

		vposition[0] = (double) v->position[0];
		vposition[1] = (double) v->position[1];
		vposition[2] = (double) v->position[2];

		for (j=0 ; j<2 ; j++)
		{
			/* The following calculation is sensitive to floating-point
			 * precision.  It needs to produce the same result that the
			 * light compiler does, because R_BuildLightMap uses surf->
			 * extents to know the width/height of a surface's lightmap,
			 * and incorrect rounding here manifests itself as patches
			 * of "corrupted" looking lightmaps.
			 * Most light compilers are win32 executables, so they use
			 * x87 floating point.  This means the multiplies and adds
			 * are done at 80-bit precision, and the result is rounded
			 * down to 32-bits and stored in val.
			 * Adding the casts to double seems to be good enough to fix
			 * lighting glitches when Quakespasm is compiled as x86_64
			 * and using SSE2 floating-point.  A potential trouble spot
			 * is the hallway at the beginning of mfxsp17.  -- ericw
			 */
			val =
				(vposition[0] * texvecs[j][0]) +
				(vposition[1] * texvecs[j][1]) +
				(vposition[2] * texvecs[j][2]) +
				texvecs[j][3];

			mins[j] = q_min (mins[j], val);
			maxs[j] = q_max (maxs[j], val);
		}
	}


	for (i=0 ; i<2 ; i++)
	{
		int bmin = 16 * (int) floor (mins[i]/16);
		int bmax = 16 * (int) ceil (maxs[i]/16);

		s->texturemins[i] = bmin;
		s->extents[i] = bmax - bmin;

		if ( !(tex->flags & TEX_SPECIAL) && s->extents[i] > 2000) //johnfitz -- was 512 in glquake, 256 in winquake
			Sys_Error ("Bad surface extents");
	}
}

/*
=================
Mod_CalcSurfaceBounds -- johnfitz -- calculate bounding box for per-surface frustum culling
=================
*/
void Mod_CalcSurfaceBounds (msurface_t *s)
{
	int			i, e;
	mvertex_t	*v;

	s->mins[0] = s->mins[1] = s->mins[2] = FLT_MAX;
	s->maxs[0] = s->maxs[1] = s->maxs[2] = -FLT_MAX;

	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

		s->mins[0] = q_min (s->mins[0], v->position[0]);
		s->mins[1] = q_min (s->mins[1], v->position[1]);
		s->mins[2] = q_min (s->mins[2], v->position[2]);

		s->maxs[0] = q_max (s->maxs[0], v->position[0]);
		s->maxs[1] = q_max (s->maxs[1], v->position[1]);
		s->maxs[2] = q_max (s->maxs[2], v->position[2]);
	}
}


static qboolean BSPX_LightGridLoad (qmodel_t *mod, void *lump, int lumpsize)
{
        if (!mod || !lump || lumpsize <= 0)
                return false;

        Lightgrid_Free (cl.lightgrid);
        cl.lightgrid = NULL;

        return LightgridOctree_LoadBSPX (mod, lump, lumpsize);
}

static void Lightgrid_Info_f (void)
{
	const lightgrid_t *lg = Lightgrid_Get ();

	if (!lg || !lg->octree)
	{
		Con_Printf ("No lightgrid loaded\n");
		return;
	}

	const lightgrid_octree_t *oct = lg->octree;
	Con_Printf ("Lightgrid (octree): %zu nodes, %zu leaves\n", oct->node_count, oct->leaf_count);
	Con_Printf ("Grid dist: %.1f %.1f %.1f size: %d %d %d styles: %u\n",
		oct->header.grid_dist[0], oct->header.grid_dist[1], oct->header.grid_dist[2],
		oct->header.grid_size[0], oct->header.grid_size[1], oct->header.grid_size[2],
		oct->header.num_styles);
	Con_Printf ("Bounds: mins (%.1f %.1f %.1f) maxs (%.1f %.1f %.1f)\n",
		lg->mins[0], lg->mins[1], lg->mins[2],
		lg->maxs[0], lg->maxs[1], lg->maxs[2]);
	Con_Printf ("Source: %s\n", Lightgrid_GetSource ());
}

static void Mod_PrintLightgridDebug (const char *op, const qmodel_t *mod, const char *source_path, qboolean applied, const char *reason)
{
	const lightgrid_t *active = Lightgrid_Get();

	Con_Printf ("%s debug information:\n", op);
	Con_Printf ("  worldmodel: %s%s\n", mod ? mod->name : "<none>",
		mod && mod->type == mod_brush ? " (brush)" : "");

	if (source_path && source_path[0])
		Con_Printf ("  source path: %s\n", source_path);

	Con_Printf ("  active lightgrid: %s\n", active ? "yes" : "no");
	if (active && active->octree)
	{
		Con_Printf ("    backend=OCTREE, nodes=%zu, leaves=%zu\n",
			active->octree->node_count, active->octree->leaf_count);
	}

	if (applied)
	{
		Con_Printf ("  lightgrid applied successfully.\n");
	}
	else if (reason && reason[0])
	{
		Con_Printf ("  lightgrid not applied: %s\n", reason);
	}
}

static qboolean Mod_LoadLightgridOctreeFromMap (qmodel_t *mod, const char *mapname)
{
	byte *buffer;
	char path[MAX_OSPATH];
	char base[MAX_QPATH];
	char old_loadname[sizeof(loadname)];
	qmodel_t *old_loadmodel;
	byte *old_mod_base;
	size_t lumpsize;
	void *lglump;
	qboolean ok = false;

	if (!mod || !mapname || !mapname[0] || mod->type != mod_brush)
		return false;

	if (!mod->bspx_header)
		return false;

	COM_FileBase (mapname, base, sizeof(base));

	if (strchr(base, '.'))
		q_strlcpy (path, base, sizeof(path));
	else
		q_snprintf (path, sizeof(path), "maps/%s.bsp", base);

	buffer = COM_LoadMallocFile (path, NULL);
	if (!buffer)
	{
		Con_Printf ("load_lightgrid_octree: couldn't load %s\n", path);
		return false;
	}

	memcpy (old_loadname, loadname, sizeof(loadname));
	old_loadmodel = loadmodel;
	old_mod_base = mod_base;

	COM_FileBase (path, loadname, sizeof(loadname));
	loadmodel = mod;
	mod_base = buffer;

	lglump = BSPX_FindLump (mod->bspx_header, buffer, "LIGHTGRID_OCTREE", &lumpsize);
	if (!lglump)
	{
		Con_Printf ("load_lightgrid_octree: LIGHTGRID_OCTREE not found in %s\n", path);
		goto done;
	}

	if (!LightgridOctree_LoadBSPX (mod, lglump, (int)lumpsize))
	{
		Con_Warning ("load_lightgrid_octree: failed to consume %s\n", path);
		goto done;
	}

	ok = true;

done:
	memcpy (loadname, old_loadname, sizeof(loadname));
	loadmodel = old_loadmodel;
	mod_base = old_mod_base;
	q_free(buffer);
	return ok;
}

static void Mod_LoadLightgridOctree_f (cvar_t *var)
{
	static qboolean resetting = false;
	const lightgrid_t *previous;
	char mapname[MAX_QPATH];
	qboolean success = false;
	const char *failure_reason = NULL;

	if (resetting)
		return;

	if (!var || !var->string)
		return;

	mapname[0] = '\0';

	if (!cl.worldmodel)
	{
		failure_reason = "no map loaded";
	}
	else if (cl.worldmodel->type != mod_brush)
	{
		failure_reason = "worldmodel is not a brush model";
	}
	else if (!cl.worldmodel->bspx_header)
	{
		failure_reason = "current map has no BSPX directory";
	}
	else
	{
		previous = Lightgrid_Get ();
		q_strlcpy (mapname, cl.worldmodel->name, sizeof(mapname));

		Con_Printf ("load_lightgrid_octree: loading LIGHTGRID_OCTREE lump for current map (%s)\n", mapname);

		success = Mod_LoadLightgridOctreeFromMap (cl.worldmodel, mapname);
		if (!success)
			failure_reason = "failed to load LIGHTGRID_OCTREE lump";
		else if (!Lightgrid_Get () || Lightgrid_Get () == previous)
			failure_reason = "lightgrid failed to apply";
	}

	Mod_PrintLightgridDebug ("load_lightgrid_octree", cl.worldmodel, cl.worldmodel ? mapname : NULL,
		success && Lightgrid_Get () != NULL, failure_reason);

	resetting = true;
	Cvar_Set (var->name, "");
	resetting = false;
}

static qboolean LightgridOctree_LoadBSPX (qmodel_t *mod, void *data, int size)
{
	const byte *cursor, *end;
	lightgrid_octree_t *oct;
	lightgrid_t *lg;

	if (!mod || !data || size <= 0)
		return false;

	cursor = (const byte *)data;
	end = cursor + size;

	if ((size_t)(end - cursor) < sizeof(float) * 6 + sizeof(uint8_t) + sizeof(uint32_t))
		return false;

	oct = (lightgrid_octree_t *)Hunk_AllocName (sizeof(*oct), "lgoctree");
	if (!oct)
		return false;

	memset (oct, 0, sizeof(*oct));

	for (int i = 0; i < 3; i++)
	{
		float temp;
		memcpy(&temp, cursor, sizeof(temp));
		oct->header.grid_dist[i] = LittleFloat (temp);
		cursor += sizeof(float);
	}

	for (int i = 0; i < 3; i++)
	{
		int32_t temp;
		memcpy(&temp, cursor, sizeof(temp));
		oct->header.grid_size[i] = LittleLong (temp);
		cursor += sizeof(uint32_t);
	}

	for (int i = 0; i < 3; i++)
	{
		float temp;
		memcpy(&temp, cursor, sizeof(temp));
		oct->header.grid_mins[i] = LittleFloat (temp);
		cursor += sizeof(float);
	}

	oct->header.num_styles = *cursor;
	cursor += sizeof(uint8_t);

	{
		uint32_t temp;
		memcpy(&temp, cursor, sizeof(temp));
		oct->header.root_node = LittleLong (temp);
	}
	cursor += sizeof(uint32_t);

	if (cursor > end)
		return false;

	if ((size_t)(end - cursor) < sizeof(uint32_t))
		return false;

	{
		uint32_t temp;
		memcpy(&temp, cursor, sizeof(temp));
		oct->node_count = (size_t)LittleLong (temp);
	}
	cursor += sizeof(uint32_t);

	if (!oct->node_count)
		return false;

	if (oct->node_count > SIZE_MAX / sizeof(lightgrid_octree_node_t))
		return false;

	{
		size_t node_bytes = sizeof(int32_t) * 3 + sizeof(uint32_t) * 8;
		if (oct->node_count > (size_t)(end - cursor) / node_bytes)
			return false;
	}

	oct->nodes = (lightgrid_octree_node_t *)Hunk_AllocName (oct->node_count * sizeof(lightgrid_octree_node_t), "lgoctnodes");
	if (!oct->nodes)
		return false;

	for (size_t i = 0; i < oct->node_count; i++)
	{
		for (int axis = 0; axis < 3; axis++)
		{
			int32_t temp;
			memcpy(&temp, cursor, sizeof(temp));
			oct->nodes[i].division_point[axis] = LittleLong (temp);
			cursor += sizeof(uint32_t);
		}

		for (int c = 0; c < 8; c++)
		{
			uint32_t temp;
			memcpy(&temp, cursor, sizeof(temp));
			oct->nodes[i].child[c] = LittleLong (temp);
			cursor += sizeof(uint32_t);
		}
	}

	if ((size_t)(end - cursor) < sizeof(uint32_t))
		return false;

	{
		uint32_t temp;
		memcpy(&temp, cursor, sizeof(temp));
		oct->leaf_count = (size_t)LittleLong (temp);
	}
	cursor += sizeof(uint32_t);

	if (!oct->leaf_count)
		return false;

	if (oct->leaf_count > SIZE_MAX / sizeof(lightgrid_octree_leaf_t))
		return false;

	oct->leaves = (lightgrid_octree_leaf_t *)Hunk_AllocName (oct->leaf_count * sizeof(lightgrid_octree_leaf_t), "lgoctleaf");
	if (!oct->leaves)
		return false;

	memset (oct->leaves, 0, oct->leaf_count * sizeof(lightgrid_octree_leaf_t));

	for (size_t leaf_index = 0; leaf_index < oct->leaf_count; leaf_index++)
	{
		lightgrid_octree_leaf_t *leaf = &oct->leaves[leaf_index];
		if ((size_t)(end - cursor) < sizeof(uint32_t) * 6)
			return false;

		for (int axis = 0; axis < 3; axis++)
		{
			int32_t temp;
			memcpy(&temp, cursor, sizeof(temp));
			leaf->mins[axis] = LittleLong (temp);
			cursor += sizeof(uint32_t);
		}
		for (int axis = 0; axis < 3; axis++)
		{
			int32_t temp;
			memcpy(&temp, cursor, sizeof(temp));
			leaf->size[axis] = LittleLong (temp);
			cursor += sizeof(uint32_t);
		}

		if (leaf->size[0] <= 0 || leaf->size[1] <= 0 || leaf->size[2] <= 0)
			return false;
		size_t sample_count = (size_t)leaf->size[0] * (size_t)leaf->size[1] * (size_t)leaf->size[2];

		if (!sample_count || sample_count > SIZE_MAX / sizeof(lightgrid_octree_sampleset_t))
			return false;

		if ((size_t)(end - cursor) < sample_count)
			return false;

		leaf->samples = (lightgrid_octree_sampleset_t *)Hunk_AllocName (sample_count * sizeof(lightgrid_octree_sampleset_t), "lgoctsamp");
		if (!leaf->samples)
			return false;

		memset (leaf->samples, 0, sample_count * sizeof(lightgrid_octree_sampleset_t));

		for (size_t i = 0; i < sample_count; i++)
		{
			lightgrid_octree_sampleset_t *set = &leaf->samples[i];
			uint8_t used = *cursor++;

			if (used == 0xFF)
			{
				set->occluded = true;
				continue;
			}

			if ((size_t)(end - cursor) < (size_t)used * (sizeof(uint8_t) + 3))
				return false;

			if (used == 0)
				continue;

			lightgrid_octree_sample_t chosen = { {0, 0, 0}, 0 };
			qboolean chosen_set = false;
			for (uint32_t j = 0; j < used; j++)
			{
				uint8_t style = *cursor++;
				uint8_t color[3];
					/*
					 * LIGHTGRID_OCTREE stores sample colors in BGR byte order.
					 * Keep the runtime representation in RGB to match the rest of
					 * the renderer (alias lighting, debug output).
					 */
				color[2] = *cursor++; /* B */
				color[1] = *cursor++; /* G */
				color[0] = *cursor++; /* R */

				if (!chosen_set)
				{
					chosen.style = style;
					chosen.color[0] = color[0];
					chosen.color[1] = color[1];
					chosen.color[2] = color[2];
					chosen_set = true;
				}

				if (style == 0)
				{
					chosen.style = style;
					chosen.color[0] = color[0];
					chosen.color[1] = color[1];
					chosen.color[2] = color[2];
				}
			}

			if (chosen_set)
			{
				set->samples[0] = chosen;
				set->used_samples = 1;
			}
		}
	}

	if (!Lightgrid_ValidateOctree (oct, r_lightgrid_octree_debug.value > 0.f))
		return false;

	mod->lightgrid_octree = oct;

	lg = (lightgrid_t *)Hunk_AllocName (sizeof(*lg), "lightgrid");
	if (!lg)
		return false;

	memset (lg, 0, sizeof(*lg));
	lg->backend = LIGHTGRID_BACKEND_OCTREE;
	lg->octree = oct;
	lg->source = LIGHTGRID_SRC_OCTREE;

	VectorCopy (oct->header.grid_mins, lg->mins);
	for (int i = 0; i < 3; i++)
		lg->maxs[i] = oct->header.grid_mins[i] + oct->header.grid_dist[i] * (oct->header.grid_size[i] - 1);

	Lightgrid_Set (lg);

	Con_Printf ("Loaded LIGHTGRID_OCTREE: dist(%.1f %.1f %.1f) size(%d %d %d) mins(%.1f %.1f %.1f) styles %u (%zu nodes, %zu leaves)\n",
		oct->header.grid_dist[0], oct->header.grid_dist[1], oct->header.grid_dist[2],
		oct->header.grid_size[0], oct->header.grid_size[1], oct->header.grid_size[2],
		oct->header.grid_mins[0], oct->header.grid_mins[1], oct->header.grid_mins[2],
		oct->header.num_styles, oct->node_count, oct->leaf_count);

	return true;
}

static void Mod_LoadLightgrid (qmodel_t *mod)
{
	void *lglump;
	int lumpsize;

        if (Mod_ShouldSkipBrushExtras (mod))
                return;

	if (!mod || mod->type != mod_brush)
		return;

	lglump = Q1BSPX_FindLump ("LIGHTGRID_OCTREE", &lumpsize);

	if (lglump && lumpsize > 0)
	{
		if (LightgridOctree_LoadBSPX (mod, lglump, lumpsize))
			Q1BSPX_MarkUsed ("LIGHTGRID_OCTREE");
		else
			Q1BSPX_MarkUnsupported ("LIGHTGRID_OCTREE");
	}
}


/*
=================
Mod_LoadFaces
=================
*/
static void Mod_LoadFaces (lump_t *l)
{
        byte            *in;
        msurface_t      *out;
        int                     i, count, surfnum, lofs, shift;
        int                     planenum, side, texinfon;

        unsigned char *lmshift = NULL, defaultshift = 4; // Standardwert: 16 texels per luxel (2^4)
        unsigned int *lmoffset = NULL;
        unsigned char *lmstyle8 = NULL;
        unsigned short *lmstyle16 = NULL;
        int stylesperface = 4;
        int lumpsize;
        char scalebuf[16];
        int facestyles;
        unsigned char *decoupledlm = NULL;

        in = mod_base + l->fileofs;

        if (l->filelen % bsp_loader.face_stride)
                Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);

        count = l->filelen / bsp_loader.face_stride;
        out = (msurface_t *)Hunk_AllocName ( count*sizeof(*out), loadname);

        //johnfitz -- warn mappers about exceeding old limits
        if (count > 32767 && !bsp_loader.is_bsp2)
                Con_DWarning ("%i faces exceeds standard limit of 32767.\n", count);
        //johnfitz

        if (!mod_ignorelmscale.value)
        {
                decoupledlm = Q1BSPX_FindLump("DECOUPLED_LM", &lumpsize); //RGB packed data
                if (decoupledlm && lumpsize >= count)
                {       //basically stomps over the lmshift+lmoffset stuff above. lmstyle/lmstyle16+lit/hdr+lux info is still needed
                        lmshift = NULL;
                        lmoffset = NULL;
                        Q1BSPX_MarkUsed("DECOUPLED_LM");
                }
                else
                {
                        decoupledlm = NULL;

                        lmshift = Q1BSPX_FindLump("LMSHIFT", &lumpsize);
                        if (lumpsize != sizeof(*lmshift)*count)
                                lmshift = NULL;
                        else
                                Q1BSPX_MarkUsed("LMSHIFT");
                        lmoffset = Q1BSPX_FindLump("LMOFFSET", &lumpsize);
                        if (lumpsize != sizeof(*lmoffset)*count)
                                lmoffset = NULL;
                        else
                                Q1BSPX_MarkUsed("LMOFFSET");

                        if (Mod_ParseWorldspawnKey(loadmodel, "lightmap_scale", scalebuf, sizeof(scalebuf)))
                        {
                                char *e;
                                i = strtol(scalebuf, &e, 10);
                                if (i < 0 || *e)
                                        Con_Warning("Incorrect value for lightmap_scale field - %s - should be texels-per-luxel(and power-of-two), use 16 (or omit) to match vanilla quake.\n", scalebuf);
                                else if (i == 0)
                                        ;       //silently use default when its explicitly set to 0 or empty. a bogus value but oh well.
                                else
                                {
                                        for(defaultshift = 0; i > 1; defaultshift++)
                                                i >>= 1;
                                }
                        }
                }
                		lmstyle16 = Q1BSPX_FindLump("LMSTYLE16", &lumpsize);
		stylesperface = lumpsize/(sizeof(*lmstyle16)*count);
		if (lumpsize != sizeof(*lmstyle16)*stylesperface*count)
			lmstyle16 = NULL;
		else
			Q1BSPX_MarkUsed("LMSTYLE16");
		if (!lmstyle16)
		{
			lmstyle8 = Q1BSPX_FindLump("LMSTYLE", &lumpsize);
			stylesperface = lumpsize/(sizeof(*lmstyle8)*count);
			if (lumpsize != sizeof(*lmstyle8)*stylesperface*count)
				lmstyle8 = NULL;
			else
				Q1BSPX_MarkUsed("LMSTYLE");
		}

        loadmodel->surfaces = out;
        loadmodel->numsurfaces = count;

        for (surfnum=0 ; surfnum<count ; surfnum++, out++)
        {
                texture_t *texture;
                byte *face_in = in + (surfnum * bsp_loader.face_stride);
                unsigned char *styles_in = face_in + bsp_loader.face_styles_offset;

                // initialize to avoid garbage when a BSP provides fewer than MAXLIGHTMAPS styles
                for (i=0 ; i<MAXLIGHTMAPS ; i++)
                        out->styles[i] = INVALID_LIGHTSTYLE;

                out->firstedge = Mod_ReadInt32(face_in, bsp_loader.face_firstedge_offset);
                out->numedges = bsp_loader.read_face_numedges(face_in, bsp_loader.face_numedges_offset);
                planenum = bsp_loader.read_face_planenum(face_in, 0);
                side = bsp_loader.read_face_side(face_in, bsp_loader.face_side_offset);
                texinfon = bsp_loader.read_face_texinfo(face_in, bsp_loader.face_texinfo_offset);
                for (i=0 ; i<MAXLIGHTMAPS ; i++)
                        out->styles[i] = ((styles_in[i]==INVALID_LIGHTSTYLE_OLD)?INVALID_LIGHTSTYLE:styles_in[i]);
                lofs = Mod_ReadInt32(face_in, bsp_loader.face_lightofs_offset);

                shift = defaultshift;
                //bspx overrides (for lmscale)
                if (lmshift)
                        shift = lmshift[surfnum];
                if (lmoffset)
                        lofs = LittleLong(lmoffset[surfnum]);
                if (lmstyle16)
                {
                        int copystyles = q_min(stylesperface, MAXLIGHTMAPS);
                        for (i=0 ; i<copystyles ; i++)
                                out->styles[i] = lmstyle16[surfnum*stylesperface+i];
                }
                else if (lmstyle8)
                {
                        int copystyles = q_min(stylesperface, MAXLIGHTMAPS);
                        for (i=0 ; i<copystyles ; i++)
                        {
                                out->styles[i] = lmstyle8[surfnum*stylesperface+i];
                                if (out->styles[i] == INVALID_LIGHTSTYLE_OLD)
                                        out->styles[i] = INVALID_LIGHTSTYLE;
                        }
                }
                for ( ; i<MAXLIGHTMAPS ; i++)
                        out->styles[i] = INVALID_LIGHTSTYLE;

                out->flags = 0;
                if (out->numedges < 3)
                        Con_Warning("surfnum %d: bad numedges %d\n", surfnum, out->numedges);

                if (side)
                        out->flags |= SURF_PLANEBACK;

                out->plane = loadmodel->planes + planenum;

                out->texinfo = loadmodel->texinfo + texinfon;

                CalcSurfaceExtents (out);

                Mod_CalcSurfaceBounds (out); //johnfitz -- for per-surface frustum culling

        // lighting info
                if (loadmodel->bspversion == BSPVERSION_QUAKE64)
                        lofs /= 2; // Q64 samples are 16bits instead 8 in normal Quake

                for (facestyles = 0 ; facestyles<MAXLIGHTMAPS && out->styles[facestyles] != INVALID_LIGHTSTYLE ; facestyles++)
                        ;       //count the styles so we can bound-check properly.
                if (lofs == -1)
                {
                        out->samples = NULL;
                        out->luxsamples = NULL;
                }
			else
			{
				int smax = (out->extents[0] >> 4) + 1;
				int tmax = (out->extents[1] >> 4) + 1;
				int facesamples = facestyles * smax * tmax;

				// use the same dimensions as R_BuildLightMap to avoid rejecting valid faces
				if (lofs + facesamples > loadmodel->lightdatasamples)
				out->samples = NULL; //corrupt...
				else if (loadmodel->flags & MOD_HDRLIGHTING)
				out->samples = loadmodel->lightdata + (lofs * 4); //spike -- hdr lighting data is 4-aligned
				else
				out->samples = loadmodel->lightdata + (lofs * 3); //johnfitz -- lit support via lordhavoc (was "+ i")

				out->luxsamples = NULL;
				if (loadmodel->lightdirdata)
				{
					if (lofs + facesamples > loadmodel->lightdirsamples)
						Con_DWarning("LIGHTINGDIR data too small for face %d\n", surfnum);
					else
						out->luxsamples = loadmodel->lightdirdata + (lofs * 3);
				}
			}

                texture = loadmodel->textures[out->texinfo->texnum];

                if (texture->type == TEXTYPE_SKY)
                {
                        out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
                }
                else if (TEXTYPE_ISLIQUID (texture->type))
                {
                        out->flags |= SURF_DRAWTURB;
                        if (out->texinfo->flags & TEX_SPECIAL)
                                out->flags |= SURF_DRAWTILED;
                }
                else if (out->texinfo->flags & TEX_SPECIAL)
                {
                        out->flags |= SURF_DRAWTILED;
                }
        }
}
}

static void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
		if (!node)
			return;

		node->parent = parent;

		if (node->contents < 0)
			return;

		Mod_SetParent (node->children[0], node);
		Mod_SetParent (node->children[1], node);
}

static void Mod_LoadNodes_S (lump_t *l)
{
        dsnode_t        *in;
        mnode_t         *out;
        int                     i, j, count, p;

        if (l->filelen % sizeof(*in))
                Sys_Error ("Mod_LoadNodes: funny lump size in %s", loadmodel->name);
        count = l->filelen / sizeof(*in);
        in = (dsnode_t *)(mod_base + l->fileofs);
        out = (mnode_t *) Hunk_AllocName ( count*sizeof(*out), loadname);

        loadmodel->nodes = out;
        loadmodel->numnodes = count;

        for (i=0 ; i<count ; i++, in++, out++)
        {
                out->contents = 0;

                for (j=0 ; j<3 ; j++)
                {
                        out->minmaxs[j] = LittleShort (in->mins[j]);
                        out->minmaxs[3+j] = LittleShort (in->maxs[j]);
                }

                out->plane = loadmodel->planes + LittleLong(in->planenum);

                for (j=0 ; j<2 ; j++)
                {
                        p = LittleShort (in->children[j]);
                        if (p >= 0)
                                out->children[j] = loadmodel->nodes + p;
                        else
                                out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
                }

                out->firstsurface = (unsigned short)LittleShort(in->firstface);
                out->numsurfaces = (unsigned short)LittleShort(in->numfaces);
        }
}

static void Mod_LoadNodes_L1 (lump_t *l)
{
        dl1node_t       *in;
        mnode_t         *out;
        int                     i, j, count, p;

        if (l->filelen % sizeof(*in))
                Sys_Error ("Mod_LoadNodes: funny lump size in %s", loadmodel->name);
        count = l->filelen / sizeof(*in);
        in = (dl1node_t *)(mod_base + l->fileofs);
        out = (mnode_t *) Hunk_AllocName ( count*sizeof(*out), loadname);

        loadmodel->nodes = out;
        loadmodel->numnodes = count;

        for (i=0 ; i<count ; i++, in++, out++)
        {
                out->contents = 0;

                for (j=0 ; j<3 ; j++)
                {
                        out->minmaxs[j] = LittleShort (in->mins[j]);
                        out->minmaxs[3+j] = LittleShort (in->maxs[j]);
                }

                out->plane = loadmodel->planes + LittleLong(in->planenum);

                for (j=0 ; j<2 ; j++)
                {
                        p = LittleLong (in->children[j]);
                        if (p >= 0)
                                out->children[j] = loadmodel->nodes + p;
                        else
                                out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
                }

                out->firstsurface = LittleLong(in->firstface);
                out->numsurfaces = LittleLong(in->numfaces);
        }
}

static void Mod_LoadNodes_L2 (lump_t *l)
{
        dl2node_t       *in;
        mnode_t         *out;
        int                     i, j, count, p;

        if (l->filelen % sizeof(*in))
                Sys_Error ("Mod_LoadNodes: funny lump size in %s", loadmodel->name);
        count = l->filelen / sizeof(*in);
        in = (dl2node_t *)(mod_base + l->fileofs);
        out = (mnode_t *) Hunk_AllocName ( count*sizeof(*out), loadname);

        loadmodel->nodes = out;
        loadmodel->numnodes = count;

        for (i=0 ; i<count ; i++, in++, out++)
        {
                out->contents = 0;

                for (j=0 ; j<3 ; j++)
                {
                        out->minmaxs[j] = LittleFloat (in->mins[j]);
                        out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
                }

                out->plane = loadmodel->planes + LittleLong(in->planenum);

                for (j=0 ; j<2 ; j++)
                {
                        p = LittleLong (in->children[j]);
                        if (p >= 0)
                                out->children[j] = loadmodel->nodes + p;
                        else
                                out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
                }

                out->firstsurface = LittleLong(in->firstface);
                out->numsurfaces = LittleLong(in->numfaces);
        }
}

static void Mod_LoadNodes (lump_t *l, int bsp2)
{
        if (bsp2 == 2)
		Mod_LoadNodes_L2(l);
	else if (bsp2)
		Mod_LoadNodes_L1(l);
	else
		Mod_LoadNodes_S(l);

	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

static void Mod_ProcessLeafs_S (dsleaf_t *in, int filelen)
{
	mleaf_t		*out;
	int			i, j, count, p;

	if (filelen % sizeof(*in))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", loadmodel->name);
	count = filelen / sizeof(*in);
	out = (mleaf_t *) Hunk_AllocName ( count*sizeof(*out), loadname);

	//johnfitz
	if (count > 32767)
		Host_Error ("Mod_LoadLeafs: %i leafs exceeds limit of 32767.", count);
	//johnfitz

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->contents);
		out->contents = p;

		out->firstmarksurface = loadmodel->marksurfaces + (unsigned short)LittleShort(in->firstmarksurface); //johnfitz -- unsigned short
		out->nummarksurfaces = (unsigned short)LittleShort(in->nummarksurfaces); //johnfitz -- unsigned short

		p = LittleLong(in->visofs);
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;

		for (j=0 ; j<4 ; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];

		//johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_ProcessLeafs_L1 (dl1leaf_t *in, int filelen)
{
	mleaf_t		*out;
	int			i, j, count, p;

	if (filelen % sizeof(*in))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", loadmodel->name);

	count = filelen / sizeof(*in);

	out = (mleaf_t *) Hunk_AllocName (count * sizeof(*out), loadname);

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->contents);
		out->contents = p;

		out->firstmarksurface = loadmodel->marksurfaces + LittleLong(in->firstmarksurface); //johnfitz -- unsigned short
		out->nummarksurfaces = LittleLong(in->nummarksurfaces); //johnfitz -- unsigned short

		p = LittleLong(in->visofs);
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;

		for (j=0 ; j<4 ; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];

		//johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_ProcessLeafs_L2 (dl2leaf_t *in, int filelen)
{
	mleaf_t		*out;
	int			i, j, count, p;

	if (filelen % sizeof(*in))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", loadmodel->name);

	count = filelen / sizeof(*in);

	out = (mleaf_t *) Hunk_AllocName (count * sizeof(*out), loadname);

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleFloat (in->mins[j]);
			out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
		}

		p = LittleLong(in->contents);
		out->contents = p;

		out->firstmarksurface = loadmodel->marksurfaces + LittleLong(in->firstmarksurface); //johnfitz -- unsigned short
		out->nummarksurfaces = LittleLong(in->nummarksurfaces); //johnfitz -- unsigned short

		p = LittleLong(in->visofs);
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;

		for (j=0 ; j<4 ; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];

		//johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

/*
=================
Mod_LoadLeafs
=================
*/
static void Mod_LoadLeafs (lump_t *l, int bsp2)
{
	void *in = (void *)(mod_base + l->fileofs);

	if (bsp2 == 2)
		Mod_ProcessLeafs_L2 ((dl2leaf_t *)in, l->filelen);
	else if (bsp2)
		Mod_ProcessLeafs_L1 ((dl1leaf_t *)in, l->filelen);
	else
		Mod_ProcessLeafs_S  ((dsleaf_t *) in, l->filelen);
}

/*
=================
Mod_CheckWaterVis
=================
*/
static void Mod_CheckWaterVis(void)
{
	mleaf_t		*leaf, *other;
	msurface_t * surf;
	int i, j, k;
	int numclusters = loadmodel->submodels[0].visleafs;
	int contentfound = 0;
	int contenttransparent = 0;
	int contenttype;
	unsigned hascontents = 0;

	if (r_novis.value)
	{	//all can be
		loadmodel->contentstransparent = (SURF_DRAWWATER|SURF_DRAWTELE|SURF_DRAWSLIME|SURF_DRAWLAVA);
		return;
	}

	//pvs is 1-based. leaf 0 sees all (the solid leaf).
	//leaf 0 has no pvs, and does not appear in other leafs either, so watch out for the biases.
	for (i=0,leaf=loadmodel->leafs+1 ; i<numclusters ; i++, leaf++)
	{
		byte *vis;
		if (leaf->contents < 0)	//err... wtf?
			hascontents |= 1u<<-leaf->contents;
		if (leaf->contents == CONTENTS_WATER)
		{
			if ((contenttransparent & (SURF_DRAWWATER|SURF_DRAWTELE))==(SURF_DRAWWATER|SURF_DRAWTELE))
				continue;
			//this check is somewhat risky, but we should be able to get away with it.
			for (contenttype = 0, j = 0; j < leaf->nummarksurfaces; j++)
			{
				surf = &loadmodel->surfaces[leaf->firstmarksurface[j]];
				if (surf->flags & (SURF_DRAWWATER|SURF_DRAWTELE))
				{
					contenttype = surf->flags & (SURF_DRAWWATER|SURF_DRAWTELE);
					break;
				}
			}
			//its possible that this leaf has absolutely no surfaces in it, turb or otherwise.
			if (contenttype == 0)
				continue;
		}
		else if (leaf->contents == CONTENTS_SLIME)
			contenttype = SURF_DRAWSLIME;
		else if (leaf->contents == CONTENTS_LAVA)
			contenttype = SURF_DRAWLAVA;
		//fixme: tele
		else
			continue;
		if (contenttransparent & contenttype)
		{
			nextleaf:
			continue;	//found one of this type already
		}
		contentfound |= contenttype;
		vis = Mod_DecompressVis(leaf->compressed_vis, loadmodel);
		for (j = 0; j < (numclusters+7)/8; j++)
		{
			if (vis[j])
			{
				for (k = 0; k < 8; k++)
				{
					if (vis[j] & (1u<<k))
					{
						other = &loadmodel->leafs[(j<<3)+k+1];
						if (leaf->contents != other->contents)
						{
//							Con_Printf("%p:%i sees %p:%i\n", leaf, leaf->contents, other, other->contents);
							contenttransparent |= contenttype;
							goto nextleaf;
						}
					}
				}
			}
		}
	}

	if (!contenttransparent)
	{	//no water leaf saw a non-water leaf
		//but only warn when there's actually water somewhere there...
		if (hascontents & ((1<<-CONTENTS_WATER)
						|  (1<<-CONTENTS_SLIME)
						|  (1<<-CONTENTS_LAVA)))
			Con_DPrintf("%s is not watervised\n", loadmodel->name);
	}
	else
	{
		Con_DPrintf2("%s is vised for transparent", loadmodel->name);
		if (contenttransparent & SURF_DRAWWATER)
			Con_DPrintf2(" water");
		if (contenttransparent & SURF_DRAWTELE)
			Con_DPrintf2(" tele");
		if (contenttransparent & SURF_DRAWLAVA)
			Con_DPrintf2(" lava");
		if (contenttransparent & SURF_DRAWSLIME)
			Con_DPrintf2(" slime");
		Con_DPrintf2("\n");
	}
	//any types that we didn't find are assumed to be transparent.
	//this allows submodels to work okay (eg: ad uses func_illusionary teleporters for some reason).
	loadmodel->contentstransparent = contenttransparent | (~contentfound & (SURF_DRAWWATER|SURF_DRAWTELE|SURF_DRAWSLIME|SURF_DRAWLAVA));
}

/*
=================
Mod_FindUsedTextures
=================
*/
static void Mod_FindUsedTextures (qmodel_t *mod)
{
	msurface_t	*s;
	int			i, count;
	int			ofs[TEXTYPE_COUNT];
	uint32_t	*inuse;

	inuse = (uint32_t *) q_calloc(BITARRAY_DWORDS (mod->numtextures), sizeof (uint32_t));
	if (!inuse)
		Sys_Error ("Mod_FindUsedTextures: out of memory (%d bits)", mod->numtextures);

	memset (ofs, 0, sizeof(ofs));
	for (i = 0, s = mod->surfaces + mod->firstmodelsurface; i < mod->nummodelsurfaces; i++, s++)
	{
		texture_t *t = mod->textures[s->texinfo->texnum];
		if (!t)
			continue;
		if (!GetBit (inuse, s->texinfo->texnum))
		{
			SetBit (inuse, s->texinfo->texnum);
			ofs[t->type]++;
		}
	}

	count = 0;
	for (i = 0; i < TEXTYPE_COUNT; i++)
	{
		int tmp = ofs[i];
		mod->texofs[i] = ofs[i] = count;
		count += tmp;
	}

	mod->texofs [TEXTYPE_COUNT] = count;
	mod->usedtextures = (int *) Hunk_Alloc (sizeof(mod->usedtextures[0]) * count);
	for (i = 0; i < mod->numtextures; i++)
	{
		texture_t *t = mod->textures[i];
		if (GetBit (inuse, i))
			mod->usedtextures[ofs[t->type]++] = i;
	}

	q_free(inuse);

	//Con_Printf("%s: %d/%d textures\n", mod->name, count, mod->numtextures);
}

static void ValidateClipnodeChildIndex (int child, int childslot, int nodeindex, int totalcount)
{
	if (child >= 0 && child >= totalcount)
		Host_Error ("Mod_LoadClipnodes: clipnode %d child[%d] index %d out of bounds (count %d)",
			nodeindex, childslot, child, totalcount);
}

/*
=================
Mod_LoadClipnodes
=================
*/
static void Mod_LoadClipnodes (lump_t *l, qboolean bsp2)
{
	dsclipnode_t *ins;
	dlclipnode_t *inl;

	mclipnode_t *out; //johnfitz -- was dclipnode_t
	int			i, count;
	hull_t		*hull;

	if (bsp2)
	{
		ins = NULL;
		inl = (dlclipnode_t *)(mod_base + l->fileofs);
		if (l->filelen % sizeof(*inl))
			Sys_Error ("Mod_LoadClipnodes: funny lump size in %s",loadmodel->name);

		count = l->filelen / sizeof(*inl);
	}
	else
	{
		ins = (dsclipnode_t *)(mod_base + l->fileofs);
		inl = NULL;
		if (l->filelen % sizeof(*ins))
			Sys_Error ("Mod_LoadClipnodes: funny lump size in %s",loadmodel->name);

		count = l->filelen / sizeof(*ins);
	}
	out = (mclipnode_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	//johnfitz -- warn about exceeding old limits
	if (count > 32767 && !bsp2)
		Con_DWarning ("%i clipnodes exceeds standard limit of 32767.\n", count);
	//johnfitz

	loadmodel->clipnodes = out;
	loadmodel->numclipnodes = count;

	hull = &loadmodel->hulls[1];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -16;
	hull->clip_mins[1] = -16;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 16;
	hull->clip_maxs[1] = 16;
	hull->clip_maxs[2] = 32;

	hull = &loadmodel->hulls[2];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -32;
	hull->clip_mins[1] = -32;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 32;
	hull->clip_maxs[1] = 32;
	hull->clip_maxs[2] = 64;

	if (bsp2)
	{
		for (i=0 ; i<count ; i++, out++, inl++)
		{
			out->planenum = LittleLong(inl->planenum);

			//johnfitz -- bounds check
			if (out->planenum < 0 || out->planenum >= loadmodel->numplanes)
				Host_Error ("Mod_LoadClipnodes: planenum out of bounds");
			//johnfitz

			out->children[0] = LittleLong(inl->children[0]);
			out->children[1] = LittleLong(inl->children[1]);

			ValidateClipnodeChildIndex (out->children[0], 0, i, count);
			ValidateClipnodeChildIndex (out->children[1], 1, i, count);
		}
	}
	else
	{
		for (i=0 ; i<count ; i++, out++, ins++)
		{
			out->planenum = LittleLong(ins->planenum);

			//johnfitz -- bounds check
			if (out->planenum < 0 || out->planenum >= loadmodel->numplanes)
				Host_Error ("Mod_LoadClipnodes: planenum out of bounds");
			//johnfitz

			//johnfitz -- support clipnodes > 32k
			out->children[0] = (unsigned short)LittleShort(ins->children[0]);
			out->children[1] = (unsigned short)LittleShort(ins->children[1]);

			if (out->children[0] >= count)
				out->children[0] -= 65536;
			if (out->children[1] >= count)
				out->children[1] -= 65536;

			ValidateClipnodeChildIndex (out->children[0], 0, i, count);
			ValidateClipnodeChildIndex (out->children[1], 1, i, count);
			//johnfitz
		}
	}
}

/*
=================
Mod_MakeHull0

Duplicate the drawing hull structure as a clipping hull
=================
*/
static void Mod_MakeHull0 (void)
{
	mnode_t		*in, *child;
	mclipnode_t *out; //johnfitz -- was dclipnode_t
	int			i, j, count;
	hull_t		*hull;

	hull = &loadmodel->hulls[0];

	in = loadmodel->nodes;
	count = loadmodel->numnodes;
	out = (mclipnode_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		out->planenum = in->plane - loadmodel->planes;
		for (j=0 ; j<2 ; j++)
		{
			child = in->children[j];
			if (child->contents < 0)
				out->children[j] = child->contents;
			else
				out->children[j] = child - loadmodel->nodes;
		}
	}

	//if qbsp was run with -noclip, make sure the extra hulls use the rnodes instead of the missing clipnodes
	//this won't 'fix' it, but it will stop it from crashing if it was just quickly built for debugging or whatever.
	if (!loadmodel->hulls[1].clipnodes)
	{	//hulls will be point-sized.
		//bias that point so that its mid,mid,bottom instead of at the absmin or origin. this will retain view offsets.
		loadmodel->hulls[1].clip_maxs[2] -= loadmodel->hulls[1].clip_mins[2];
		loadmodel->hulls[1].clip_mins[2] = 0;
		loadmodel->hulls[1].clipnodes = hull->clipnodes;
		loadmodel->hulls[1].firstclipnode = hull->firstclipnode;
		loadmodel->hulls[1].lastclipnode = hull->lastclipnode;
	}
	if (!loadmodel->hulls[2].clipnodes)
	{
		loadmodel->hulls[2].clip_maxs[2] -= loadmodel->hulls[2].clip_mins[2];
		loadmodel->hulls[2].clip_mins[2] = 0;
		loadmodel->hulls[2].clipnodes = loadmodel->hulls[1].clipnodes;
		loadmodel->hulls[2].firstclipnode = loadmodel->hulls[1].firstclipnode;
		loadmodel->hulls[2].lastclipnode = loadmodel->hulls[1].lastclipnode;
	}
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
static void Mod_LoadMarksurfaces (lump_t *l, int bsp2)
{
	int		i, j, count;
	int		*out;
	if (bsp2)
	{
		unsigned int *in = (unsigned int *)(mod_base + l->fileofs);

		if (l->filelen % sizeof(*in))
			Host_Error ("Mod_LoadMarksurfaces: funny lump size in %s",loadmodel->name);

		count = l->filelen / sizeof(*in);
		out = (int*)Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

		loadmodel->marksurfaces = out;
		loadmodel->nummarksurfaces = count;

		for (i=0 ; i<count ; i++)
		{
			j = LittleLong(in[i]);
			if (j >= loadmodel->numsurfaces)
				Host_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = j;
		}
	}
	else
	{
		short *in = (short *)(mod_base + l->fileofs);

		if (l->filelen % sizeof(*in))
			Host_Error ("Mod_LoadMarksurfaces: funny lump size in %s",loadmodel->name);

		count = l->filelen / sizeof(*in);
		out = (int*)Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

		loadmodel->marksurfaces = out;
		loadmodel->nummarksurfaces = count;

		//johnfitz -- warn mappers about exceeding old limits
		if (count > 32767)
			Con_DWarning ("%i marksurfaces exceeds standard limit of 32767.\n", count);
		//johnfitz

		for (i=0 ; i<count ; i++)
		{
			j = (unsigned short)LittleShort(in[i]); //johnfitz -- explicit cast as unsigned short
			if (j >= loadmodel->numsurfaces)
				Sys_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = j;
		}
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
static void Mod_LoadSurfedges (lump_t *l)
{
	int		i, count;
	int		*in, *out;

	in = (int *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (int *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for (i=0 ; i<count ; i++)
		out[i] = LittleLong (in[i]);
}


/*
=================
Mod_LoadPlanes
=================
*/
static void Mod_LoadPlanes (lump_t *l)
{
	int			i, j;
	mplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;

	in = (dplane_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (mplane_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;
		out->pad[0] = out->pad[1] = 0;
	}
}

/*
=================
RadiusFromBounds
=================
*/
static float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	int		i;
	vec3_t	corner;

	for (i=0 ; i<3 ; i++)
	{
		corner[i] = fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
	}

	return VectorLength (corner);
}

/*
=================
Mod_LoadSubmodels
=================
*/
static void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t	*in;
	dmodel_t	*out;
	int			i, j, count;

	in = (dmodel_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (dmodel_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		for (j=0 ; j<MAX_MAP_HULLS ; j++)
			out->headnode[j] = LittleLong (in->headnode[j]);
		out->visleafs = LittleLong (in->visleafs);
		out->firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);
	}

	// johnfitz -- check world visleafs -- adapted from bjp
	out = loadmodel->submodels;

	if (out->visleafs > 8192)
		Con_DWarning ("%i visleafs exceeds standard limit of 8192.\n", out->visleafs);
	//johnfitz
}

/*
=================
Mod_BoundsFromClipNode -- johnfitz

update the model's clipmins and clipmaxs based on each node's plane.

This works because of the way brushes are expanded in hull generation.
Each brush will include all six axial planes, which bound that brush.
Therefore, the bounding box of the hull can be constructed entirely
from axial planes found in the clipnodes for that hull.
=================
*/
#if 0 /* disabled for now -- see in Mod_SetupSubmodels()  */
static void Mod_BoundsFromClipNode (qmodel_t *mod, int hull, int nodenum)
{
	mplane_t	*plane;
	mclipnode_t	*node;

	if (nodenum < 0)
		return; //hit a leafnode

	node = &mod->clipnodes[nodenum];
	plane = mod->hulls[hull].planes + node->planenum;
	switch (plane->type)
	{

	case PLANE_X:
		if (plane->signbits == 1)
			mod->clipmins[0] = q_min(mod->clipmins[0], -plane->dist - mod->hulls[hull].clip_mins[0]);
		else
			mod->clipmaxs[0] = q_max(mod->clipmaxs[0], plane->dist - mod->hulls[hull].clip_maxs[0]);
		break;
	case PLANE_Y:
		if (plane->signbits == 2)
			mod->clipmins[1] = q_min(mod->clipmins[1], -plane->dist - mod->hulls[hull].clip_mins[1]);
		else
			mod->clipmaxs[1] = q_max(mod->clipmaxs[1], plane->dist - mod->hulls[hull].clip_maxs[1]);
		break;
	case PLANE_Z:
		if (plane->signbits == 4)
			mod->clipmins[2] = q_min(mod->clipmins[2], -plane->dist - mod->hulls[hull].clip_mins[2]);
		else
			mod->clipmaxs[2] = q_max(mod->clipmaxs[2], plane->dist - mod->hulls[hull].clip_maxs[2]);
		break;
	default:
		//skip nonaxial planes; don't need them
		break;
	}

	Mod_BoundsFromClipNode (mod, hull, node->children[0]);
	Mod_BoundsFromClipNode (mod, hull, node->children[1]);
}
#endif /* #if 0 */

/* EXTERNAL VIS FILE SUPPORT:
 */
typedef struct vispatch_s
{
	char	mapname[32];
	int	filelen;	// length of data after header (VIS+Leafs)
} vispatch_t;
#define VISPATCH_HEADER_LEN 36

static FILE *Mod_FindVisibilityExternal(void)
{
	vispatch_t header;
	char visfilename[MAX_QPATH];
	const char* shortname;
	unsigned int path_id;
	FILE *f;
	long pos;
	size_t r;

	q_snprintf(visfilename, sizeof(visfilename), "maps/%s.vis", loadname);
	if (COM_FOpenFile(visfilename, &f, &path_id) < 0)
	{
		Con_DPrintf("%s not found, trying ", visfilename);
		q_snprintf(visfilename, sizeof(visfilename), "%s.vis", COM_SkipPath(com_gamedir));
		Con_DPrintf("%s\n", visfilename);
		if (COM_FOpenFile(visfilename, &f, &path_id) < 0)
		{
			Con_DPrintf("external vis not found\n");
			return NULL;
		}
	}
	if (path_id < loadmodel->path_id)
	{
		fclose(f);
		Con_DPrintf("ignored %s from a gamedir with lower priority\n", visfilename);
		return NULL;
	}

	Con_DPrintf("Found external VIS %s\n", visfilename);

	shortname = COM_SkipPath(loadmodel->name);
	pos = 0;
	while ((r = fread(&header, 1, VISPATCH_HEADER_LEN, f)) == VISPATCH_HEADER_LEN)
	{
		header.filelen = LittleLong(header.filelen);
		if (header.filelen <= 0) {	/* bad entry -- don't trust the rest. */
			fclose(f);
			return NULL;
		}
		if (!q_strcasecmp(header.mapname, shortname))
			break;
		pos += header.filelen + VISPATCH_HEADER_LEN;
		fseek(f, pos, SEEK_SET);
	}
	if (r != VISPATCH_HEADER_LEN) {
		fclose(f);
		Con_DPrintf("%s not found in %s\n", shortname, visfilename);
		return NULL;
	}

	return f;
}

static byte *Mod_LoadVisibilityExternal(FILE* f)
{
	int		mark, filelen;
	byte*	visdata;

	filelen = 0;
	if (fread(&filelen, 1, 4, f) != 4) // woods
		return NULL;
	filelen = LittleLong(filelen);
	if (filelen <= 0) return NULL;
	Con_DPrintf("...%d bytes visibility data\n", filelen);
	mark = Hunk_LowMark ();
	visdata = (byte *) Hunk_AllocNameNoFill (filelen, "EXT_VIS");
	if (!fread(visdata, filelen, 1, f))
	{
		Hunk_FreeToLowMark (mark);
		return NULL;
	}
	return visdata;
}

static void Mod_LoadLeafsExternal(FILE* f)
{
	int		mark, filelen;
	void*	in;

	filelen = 0;
	if (fread(&filelen, 4, 1, f) != 1)
	{
		Con_Warning ("Couldn't read external leaf data length\n");
		return;
	}
	filelen = LittleLong(filelen);
	if (filelen <= 0) return;
	Con_DPrintf("...%d bytes leaf data\n", filelen);
	mark = Hunk_LowMark ();
	in = Hunk_AllocNameNoFill (filelen, "EXT_LEAF");
	if (!fread(in, filelen, 1, f))
	{
		Hunk_FreeToLowMark (mark);
		return;
	}
	Mod_ProcessLeafs_S((dsleaf_t *)in, filelen);
}

/*
=================
Mod_LoadBrushModel
=================
*/
static void Mod_LoadBrushModel (qmodel_t *mod, void *buffer)
{
	int			i, j;
	int			bsp2;
	dmodel_t 	*bm;
	float		radius; //johnfitz
	bsp_header_info_t header;
	bspx_header_t	*bspx;
	const int submodel_index = (mod->name[0] == '*') ? atoi(mod->name + 1) : 0;
	const qboolean is_main_model = (submodel_index == 0);

	loadmodel->type = mod_brush;

	mod_base = (byte *)buffer;

	if (!Mod_ParseBSPHeader(buffer, (size_t)com_filesize, &header))
	{
		loadmodel->type = mod_ext_invalid;
		Con_Warning("Mod_LoadBrushModel: %s has unsupported header\n", mod->name);
		return;
	}

	mod->bspversion = header.bspversion;
	switch(mod->bspversion)
	{
	case BSPVERSION:
		bsp2 = false;
		break;
	case BSP2VERSION_2PSB:
		bsp2 = 1;	//first iteration
		break;
	case BSP2VERSION_BSP2:
		bsp2 = 2;	//sanitised revision
		break;
	case BSPVERSION_QUAKE64:
		bsp2 = false;
		break;
	default:
		loadmodel->type = mod_ext_invalid;
		Con_Warning ("Mod_LoadBrushModel: %s has unsupported version number (%i should be %i)\n", mod->name, mod->bspversion, BSPVERSION);
		return;
	}

        if (is_main_model)
        {
                bspx = BSPX_Setup(mod, &header, com_filesize);
                mod->bspx_header = bspx;

                if (bspx)
                {
                        BSPX_ApplyLumpOverride(mod, "BSPX_VERTS", &header.lumps[LUMP_VERTEXES], (size_t)com_filesize);
                        BSPX_ApplyLumpOverride(mod, "BSPX_FACES", &header.lumps[LUMP_FACES], (size_t)com_filesize);

                        /*
                         * Keep LUMP_LIGHTING tied to classic lighting data.
                         * Extended BSPX lighting variants are selected by Mod_LoadLighting
                         * with explicit precedence.
                         */
                        BSPX_ApplyLumpOverride(mod, "BSPX_ENTITYSTRING", &header.lumps[LUMP_ENTITIES], (size_t)com_filesize);

                        BSPX_ApplyLumpOverride(mod, "BSPX_MODELS", &header.lumps[LUMP_MODELS], (size_t)com_filesize);

                        if (!BSPX_ApplyLumpOverride(mod, "VISX", &header.lumps[LUMP_VISIBILITY], (size_t)com_filesize))
                        {
                                if (!BSPX_ApplyLumpOverride(mod, "PVS2", &header.lumps[LUMP_VISIBILITY], (size_t)com_filesize))
                                        BSPX_ApplyLumpOverride(mod, "PVS_COMPRESSED", &header.lumps[LUMP_VISIBILITY], (size_t)com_filesize);
                        }
                }
        }
        else
        {
                mod->bspx_entries = NULL;
                mod->bspx_entries_count = 0;
                mod->bspx_header = NULL;
        }

        // load into heap

        Mod_SetupBspLoader(header.is_bsp2, header.is_bsp29);

	Mod_LoadVertexes (&header.lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (&header.lumps[LUMP_EDGES]);
	Mod_LoadSurfedges (&header.lumps[LUMP_SURFEDGES]);
	Mod_LoadTextures (&header.lumps[LUMP_TEXTURES]);
	Mod_LoadLighting (&header.lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (&header.lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (&header.lumps[LUMP_TEXINFO]);
	Mod_LoadEntities (&header.lumps[LUMP_ENTITIES]);	//Spike: moved this earlier, so that we can parse worldspawn keys earlier.
	Mod_LoadFaces (&header.lumps[LUMP_FACES]);
	Mod_LoadMarksurfaces (&header.lumps[LUMP_MARKSURFACES], bsp2);
        if (is_main_model)
        {
                void *lump;
                int lumpsize;

                lump = Q1BSPX_FindLump("FACENORMALS", &lumpsize);
                if (lump && lumpsize > 0)
                        Mod_LoadFaceNormalsBSPX(mod, lump, lumpsize);
                else
                        Con_DPrintf("BSPX: no FACENORMALS lump found\n");
        }

	if (mod->bspversion == BSPVERSION && external_vis.value/* && sv.modelname[0] && !q_strcasecmp(loadname, sv.name)*/) // woods allow vis load online
	{
		FILE* fvis;
		Con_DPrintf("trying to open external vis file\n");
		fvis = Mod_FindVisibilityExternal();
		if (fvis) {
			int mark = Hunk_LowMark();
			loadmodel->leafs = NULL;
			loadmodel->numleafs = 0;
			Con_DPrintf("found valid external .vis file for map\n");
			loadmodel->visdata = Mod_LoadVisibilityExternal(fvis);
			if (loadmodel->visdata) {
				Mod_LoadLeafsExternal(fvis);
			}
			fclose(fvis);
			if (loadmodel->visdata && loadmodel->leafs && loadmodel->numleafs) {
				goto visdone;
			}
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("External VIS data failed, using standard vis.\n");
		}
	}

	Mod_LoadVisibility (&header.lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (&header.lumps[LUMP_LEAFS], bsp2);
visdone:
        Mod_LoadNodes (&header.lumps[LUMP_NODES], bsp2);
        Mod_LoadClipnodes (&header.lumps[LUMP_CLIPNODES], bsp2);
        Mod_LoadSubmodels (&header.lumps[LUMP_MODELS]);

        if (is_main_model)
                Mod_LoadLightgrid (mod);

        Mod_MakeHull0 ();

	mod->numframes = 2;		// regular and alternate animation

	Mod_CheckWaterVis();

	if (is_main_model)
	{
		Mod_DispatchBSPXLumps(mod);
		Q1BSPX_LogUsage(mod->name);
	}

//
// set up the submodels (FIXME: this is confusing)
//
	if (mod->numsubmodels > 1)
		mod->nummodelsurfaces = mod->submodels[1].firstface;
	else
	mod->nummodelsurfaces = mod->numsurfaces;
	mod->sortkey = (CRC_Block (mod->name, strlen(mod->name)) & MODSORT_MODELMASK) << MODSORT_FRAMEBITS;
	Mod_FindUsedTextures (mod);
	// johnfitz -- okay, so that i stop getting confused every time i look at this loop, here's how it works:
	// we're looping through the submodels starting at 0.  Submodel 0 is the main model, so we don't have to
	// worry about clobbering data the first time through, since it's the same data.  At the end of the loop,
	// we create a new copy of the data to use the next time through.
	for (i=0 ; i<mod->numsubmodels ; i++)
	{
		bm = &mod->submodels[i];

		mod->hulls[0].firstclipnode = bm->headnode[0];
		for (j=1 ; j<MAX_MAP_HULLS ; j++)
		{
			mod->hulls[j].firstclipnode = bm->headnode[j];
			mod->hulls[j].lastclipnode = mod->numclipnodes-1;
		}

		mod->firstmodelsurface = bm->firstface;
		mod->nummodelsurfaces = bm->numfaces;

		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);

		//johnfitz -- calculate rotate bounds and yaw bounds
		radius = RadiusFromBounds (mod->mins, mod->maxs);
		mod->rmaxs[0] = mod->rmaxs[1] = mod->rmaxs[2] = mod->ymaxs[0] = mod->ymaxs[1] = mod->ymaxs[2] = radius;
		mod->rmins[0] = mod->rmins[1] = mod->rmins[2] = mod->ymins[0] = mod->ymins[1] = mod->ymins[2] = -radius;
		//johnfitz

		//johnfitz -- correct physics cullboxes so that outlying clip brushes on doors and stuff are handled right
		if (i > 0 || strcmp(mod->name, sv.modelname) != 0) //skip submodel 0 of sv.worldmodel, which is the actual world
		{
			// start with the hull0 bounds
			VectorCopy (mod->maxs, mod->clipmaxs);
			VectorCopy (mod->mins, mod->clipmins);

			// process hull1 (we don't need to process hull2 becuase there's
			// no such thing as a brush that appears in hull2 but not hull1)
			//Mod_BoundsFromClipNode (mod, 1, mod->hulls[1].firstclipnode); // (disabled for now becuase it fucks up on rotating models)
		}
		//johnfitz

		mod->numleafs = bm->visleafs;
		// don't overwrite sort key for main model, otherwise we break instancing for bmodel items (e.g. ammo)
		if (i)
			mod->sortkey = (i & MODSORT_MODELMASK) << MODSORT_FRAMEBITS;
		Mod_FindUsedTextures (mod);

		if (i < mod->numsubmodels-1)
		{	// duplicate the basic information
			char	name[12];

			q_snprintf (name, sizeof (name), "*%i", i + 1);
			loadmodel = Mod_FindName (name);
			*loadmodel = *mod;
			q_strlcpy (loadmodel->name, name, sizeof (loadmodel->name));
			mod = loadmodel;
		}
	}
}

/*
=================
Mod_SanitizeMapDescription

Cleans up map descriptions:
- removes colors
- replaces newlines with spaces
- replaces consecutive spaces with single one
- removes leading/trailing spaces

Returns dst string length (excluding NUL terminator)
=================
*/
size_t Mod_SanitizeMapDescription (char *dst, size_t dstsize, const char *src)
{
	int srcpos, dstpos;

	if (!dstsize)
		return 0;

	for (srcpos = dstpos = 0; src[srcpos] && (size_t)dstpos + 1 < dstsize; srcpos++)
	{
		char c = src[srcpos] & 0x7f; // remove color
		if (c == '\n' || c == '\r') // replace newlines with spaces
			c = ' ';
		else if (c == '\\' && src[srcpos + 1] == 'n') // replace '\\' followed by 'n' with space
		{
			c = ' ';
			srcpos++;
		}
		// remove leading spaces, replace consecutive spaces with single one
		if (c != ' ' || (dstpos > 0 && dst[dstpos - 1] != c))
			dst[dstpos++] = c;
	}
	// remove trailing space, if any
	if (dstpos > 0 && dst[dstpos - 1] == ' ')
		--dstpos;

	dst[dstpos] = '\0';
	return dstpos;
}

/*
=================
Mod_LoadMapDescription helpers
=================
*/
static qboolean Mod_MapDescription_ParseEntities (char *desc, size_t maxchars, const char *buffer, qboolean playable_hint)
{
        const char      *data;
        int             entity_index;
        qboolean        ret = playable_hint;

        for (entity_index = 0, data = buffer; data; entity_index++)
        {
                data = COM_Parse (data);
                if (!data || com_token[0] != '{')
                        return ret;

                while (1)
                {
                        qboolean is_message;
                        qboolean is_classname;

                        data = COM_Parse (data);
                        if (!data)
                                return ret;
                        if (com_token[0] == '}')
                                break;

                        is_message = (entity_index == 0) && !strcmp (com_token, "message");
                        is_classname = (entity_index != 0) && !strcmp (com_token, "classname");

                        data = COM_ParseEx (data, CPE_ALLOWTRUNC);
                        if (!data)
                                return ret;

                        if (is_message)
                        {
                                Mod_SanitizeMapDescription (desc, maxchars, com_token);
                                if (ret)
                                        return true;
                        }
                        else if (is_classname)
                        {
#define CLASSNAME_STARTS_WITH(str)      (!strncmp (com_token, str, strlen (str)))
#define CLASSNAME_IS(str)               (!strcmp (com_token, str))

                                if (CLASSNAME_STARTS_WITH ("info_player_") ||
                                        CLASSNAME_STARTS_WITH ("ammo_") ||
                                        CLASSNAME_STARTS_WITH ("weapon_") ||
                                        CLASSNAME_STARTS_WITH ("monster_") ||
                                        CLASSNAME_IS ("trigger_changelevel"))
                                {
                                        return true;
                                }

#undef CLASSNAME_IS
#undef CLASSNAME_STARTS_WITH
                        }
                }
        }

        return ret;
}

static qboolean Mod_LoadMapDescription_ReadEntityLump (char *desc, size_t maxchars, FILE *f, int filesize, int fileofs, int filelen)
{
        char            buf[4 * 1024];
        qboolean        playable = false;
        size_t          readlen;

        if (filelen < 0 || fileofs < 0 || filelen > filesize || fileofs > filesize - filelen)
                return false;

        if (filelen >= (int) sizeof (buf))
        {
                playable = true;
                filelen = sizeof (buf) - 1;
        }

        if (fseek (f, fileofs, SEEK_SET))
                return false;

        readlen = fread (buf, 1, (size_t) filelen, f);
        if (!readlen)
                return false;
        buf[readlen] = '\0';

        return Mod_MapDescription_ParseEntities (desc, maxchars, buf, playable);
}

static qboolean Mod_LoadMapDescription_Q1 (char *desc, size_t maxchars, FILE *f, int filesize)
{
        dheader_t       header;
        int             i;

        if (filesize <= (int) sizeof (header))
                return false;

        if (fread (&header, sizeof (header), 1, f) != 1)
                return false;

        header.version = LittleLong (header.version);

        switch (header.version)
        {
        case BSPVERSION:
        case BSP2VERSION_2PSB:
        case BSP2VERSION_BSP2:
        case BSPVERSION_QUAKE64:
                break;
        default:
                return false;
        }

        for (i = 0; i < HEADER_LUMPS; i++)
        {
                header.lumps[i].fileofs = LittleLong (header.lumps[i].fileofs);
                header.lumps[i].filelen = LittleLong (header.lumps[i].filelen);
        }

        return Mod_LoadMapDescription_ReadEntityLump (desc, maxchars, f, filesize,
                        header.lumps[LUMP_ENTITIES].fileofs, header.lumps[LUMP_ENTITIES].filelen);
}

/*
=================
Mod_LoadMapDescription

Parses the entity lump in the given map to find its worldspawn message
Writes at most maxchars bytes to dest, including the NUL terminator
Returns true if map is playable, false otherwise
=================
*/
qboolean Mod_LoadMapDescription (char *desc, size_t maxchars, const char *map)
{
        char            path[MAX_QPATH];
        FILE            *f = NULL;
        int             filesize;
        int             ident;
        qboolean        ret = false;

        if (!maxchars)
                return false;
        *desc = '\0';

        if ((size_t) q_snprintf (path, sizeof (path), "maps/%s.bsp", map) >= sizeof (path))
                return false;

        filesize = COM_FOpenFile (path, &f, NULL);
        if (filesize <= 0 || !f)
        {
                if (f)
                        fclose (f);
                return false;
        }

        if (fread (&ident, sizeof (ident), 1, f) != 1)
        {
                fclose (f);
                return false;
        }

        ident = LittleLong (ident);

                if (fseek (f, 0, SEEK_SET))
                {
                        fclose (f);
                        return false;
                }
                ret = Mod_LoadMapDescription_Q1 (desc, maxchars, f, filesize);

        fclose (f);
        return ret;
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

aliashdr_t			*pheader;

const stvert_t		*stverts;
const dtriangle_t	*triangles;

// a pose is a single set of vertexes.  a frame may be
// an animating sequence of poses
trivertx_t	*poseverts[MAXALIASFRAMES];
static int		posenum;

/*
=================
Mod_LoadAliasFrame
=================
*/
static void *Mod_LoadAliasFrame (void * pin, maliasframedesc_t *frame)
{
	trivertx_t		*pinframe;
	int				i;
	daliasframe_t	*pdaliasframe;

	if (posenum >= MAXALIASFRAMES)
		Sys_Error ("posenum >= MAXALIASFRAMES");

	pdaliasframe = (daliasframe_t *)pin;

	q_strlcpy (frame->name, pdaliasframe->name, sizeof (frame->name));
	frame->firstpose = posenum;
	frame->numposes = 1;

	for (i=0 ; i<3 ; i++)
	{
		// these are byte values, so we don't have to worry about
		// endianness
		frame->bboxmin.v[i] = pdaliasframe->bboxmin.v[i];
		frame->bboxmax.v[i] = pdaliasframe->bboxmax.v[i];
	}

	pinframe = (trivertx_t *)(pdaliasframe + 1);

	poseverts[posenum] = pinframe;
	posenum++;

	pinframe += pheader->numverts;

	return (void *)pinframe;
}


/*
=================
Mod_LoadAliasGroup
=================
*/
static void *Mod_LoadAliasGroup (void * pin,  maliasframedesc_t *frame)
{
	daliasgroup_t		*pingroup;
	int					i, numframes;
	daliasinterval_t	*pin_intervals;
	void				*ptemp;

	pingroup = (daliasgroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	frame->firstpose = posenum;
	frame->numposes = numframes;

	for (i=0 ; i<3 ; i++)
	{
		// these are byte values, so we don't have to worry about endianness
		frame->bboxmin.v[i] = pingroup->bboxmin.v[i];
		frame->bboxmax.v[i] = pingroup->bboxmax.v[i];
	}

	pin_intervals = (daliasinterval_t *)(pingroup + 1);

	frame->interval = LittleFloat (pin_intervals->interval);

	pin_intervals += numframes;

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		if (posenum >= MAXALIASFRAMES) Sys_Error ("posenum >= MAXALIASFRAMES");

		poseverts[posenum] = (trivertx_t *)((daliasframe_t *)ptemp + 1);
		posenum++;

		ptemp = (trivertx_t *)((daliasframe_t *)ptemp + 1) + pheader->numverts;
	}

	return ptemp;
}

//=========================================================


/*
=================
Mod_FloodFillSkin

Fill background pixels so mipmapping doesn't have haloes - Ed
=================
*/

typedef struct
{
	short		x, y;
} floodfill_t;

// must be a power of 2
#define	FLOODFILL_FIFO_SIZE		0x1000
#define	FLOODFILL_FIFO_MASK		(FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP( off, dx, dy )				\
do {								\
	if (pos[off] == fillcolor)				\
	{							\
		pos[off] = 255;					\
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;	\
	}							\
	else if (pos[off] != 255) fdc = pos[off];		\
} while (0)

static void Mod_FloodFillSkin( byte *skin, int skinwidth, int skinheight )
{
	byte		fillcolor = *skin; // assume this is the pixel to fill
	floodfill_t	fifo[FLOODFILL_FIFO_SIZE];
	int			inpt = 0, outpt = 0;
	int			filledcolor = -1;
	int			i;

	if (filledcolor == -1)
	{
		filledcolor = 0;
		// attempt to find opaque black
		for (i = 0; i < 256; ++i)
		{
			if (d_8to24table[i] == (255 << 0)) // alpha 1.0
			{
				filledcolor = i;
				break;
			}
		}
	}

	// can't fill to filled color or to transparent color (used as visited marker)
	if ((fillcolor == filledcolor) || (fillcolor == 255))
	{
		//printf( "not filling skin from %d to %d\n", fillcolor, filledcolor );
		return;
	}

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int			x = fifo[outpt].x, y = fifo[outpt].y;
		int			fdc = filledcolor;
		byte		*pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)				FLOODFILL_STEP( -1, -1, 0 );
		if (x < skinwidth - 1)	FLOODFILL_STEP( 1, 1, 0 );
		if (y > 0)				FLOODFILL_STEP( -skinwidth, 0, -1 );
		if (y < skinheight - 1)	FLOODFILL_STEP( skinwidth, 0, 1 );
		skin[x + skinwidth * y] = fdc;
	}
}

/*
===============
Mod_LoadAllSkins
===============
*/
static void *Mod_LoadAllSkins (int numskins, daliasskintype_t *pskintype)
{
	int			i, j, k, size, groupskins;
	char			name[MAX_QPATH];
	byte			*skin, *texels;
	daliasskingroup_t	*pinskingroup;
	daliasskininterval_t	*pinskinintervals;
	char			fbr_mask_name[MAX_QPATH]; //johnfitz -- added for fullbright support
	src_offset_t		offset; //johnfitz
	unsigned int		texflags = TEXPREF_PAD | TEXPREF_SRGB;

	skin = (byte *)(pskintype + 1);

	if (numskins < 1 || numskins > MAX_SKINS)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of skins: %d", numskins);

	size = pheader->skinwidth * pheader->skinheight;

	if (loadmodel->flags & MF_HOLEY)
		texflags |= TEXPREF_ALPHA;

	for (i=0 ; i<numskins ; i++)
	{
		if (pskintype->type == ALIAS_SKIN_SINGLE)
		{
			Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );

			// save 8 bit texels for the player model to remap
			texels = (byte *) Hunk_AllocName(size, loadname);
			pheader->texels[i] = texels - (byte *)pheader;
			memcpy (texels, (byte *)(pskintype + 1), size);

			//johnfitz -- rewritten
			q_snprintf (name, sizeof(name), "%s:frame%i", loadmodel->name, i);
			offset = (src_offset_t)(pskintype+1) - (src_offset_t)mod_base;
			if (Mod_CheckFullbrights ((byte *)(pskintype+1), size))
			{
				if (!(texflags & TEXPREF_ALPHA))
				{
					pheader->gltextures[i][0] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags | TEXPREF_ALPHABRIGHT);
				}
				else
				{
					pheader->gltextures[i][0] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags | TEXPREF_NOBRIGHT);
					q_snprintf (fbr_mask_name, sizeof(fbr_mask_name), "%s:frame%i_glow", loadmodel->name, i);
					pheader->fbtextures[i][0] = TexMgr_LoadImage (loadmodel, fbr_mask_name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags | TEXPREF_FULLBRIGHT);
				}
			}
			else
			{
				pheader->gltextures[i][0] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
					SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags);
				pheader->fbtextures[i][0] = NULL;
			}

                        pheader->gltextures[i][3] = pheader->gltextures[i][2] = pheader->gltextures[i][1] = pheader->gltextures[i][0];
                        pheader->fbtextures[i][3] = pheader->fbtextures[i][2] = pheader->fbtextures[i][1] = pheader->fbtextures[i][0];
                        pheader->emissivetextures[i][3] = pheader->emissivetextures[i][2] = pheader->emissivetextures[i][1] = pheader->emissivetextures[i][0];
			//johnfitz

			pskintype = (daliasskintype_t *)((byte *)(pskintype+1) + size);
		}
		else
		{
			// animating skin group.  yuck.
			pskintype++;
			pinskingroup = (daliasskingroup_t *)pskintype;
			groupskins = LittleLong (pinskingroup->numskins);
			pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

			pskintype = (daliasskintype_t *)(pinskinintervals + groupskins);

			for (j=0 ; j<groupskins ; j++)
			{
				Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );
				if (j == 0) {
					texels = (byte *) Hunk_AllocName(size, loadname);
					pheader->texels[i] = texels - (byte *)pheader;
					memcpy (texels, (byte *)(pskintype), size);
				}

				//johnfitz -- rewritten
				q_snprintf (name, sizeof(name), "%s:frame%i_%i", loadmodel->name, i,j);
				offset = (src_offset_t)(pskintype) - (src_offset_t)mod_base; //johnfitz
				if (Mod_CheckFullbrights ((byte *)(pskintype), size))
				{
					if (!(texflags & TEXPREF_ALPHA))
					{
						pheader->gltextures[i][j&3] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
							SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags | TEXPREF_ALPHABRIGHT);
					}
					else
					{
						pheader->gltextures[i][j&3] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
							SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags | TEXPREF_NOBRIGHT);
						q_snprintf (fbr_mask_name, sizeof(fbr_mask_name), "%s:frame%i_%i_glow", loadmodel->name, i,j);
						pheader->fbtextures[i][j&3] = TexMgr_LoadImage (loadmodel, fbr_mask_name, pheader->skinwidth, pheader->skinheight,
							SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags | TEXPREF_FULLBRIGHT);
					}
				}
				else
				{
					pheader->gltextures[i][j&3] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags);
					pheader->fbtextures[i][j&3] = NULL;
				}
				//johnfitz

				pskintype = (daliasskintype_t *)((byte *)(pskintype) + size);
                        }
                        k = j;
                        for (/**/; j < 4; j++)
                                pheader->gltextures[i][j&3] = pheader->gltextures[i][j - k];
                        {
                                int m;
                                for (m = k; m < 4; m++)
                                        pheader->emissivetextures[i][m&3] = pheader->emissivetextures[i][m - k];
                        }
                }
        }

	return (void *)pskintype;
}

//=========================================================================

/*
=================
Mod_CalcAliasBounds -- johnfitz -- calculate bounds of alias model for nonrotated, yawrotated, and fullrotated cases
=================
*/
static void Mod_CalcAliasBounds (aliashdr_t *a)
{
	int			i,j,k;
	float		dist, yawradius, radius;
	vec3_t		v;

	//clear out all data
	for (i=0; i<3;i++)
	{
		loadmodel->mins[i] = loadmodel->ymins[i] = loadmodel->rmins[i] = FLT_MAX;
		loadmodel->maxs[i] = loadmodel->ymaxs[i] = loadmodel->rmaxs[i] = -FLT_MAX;
		radius = yawradius = 0;
	}

	for (;;)
	{
		if (a->numposes && a->numverts)
		{
			switch(a->poseverttype)
			{
			case PV_QUAKE1:
				//process verts
				for (i=0 ; i<a->numposes; i++)
					for (j=0; j<a->numverts; j++)
					{
						for (k=0; k<3;k++)
							v[k] = poseverts[i][j].v[k] * pheader->scale[k] + pheader->scale_origin[k];

						for (k=0; k<3;k++)
						{
							loadmodel->mins[k] = q_min(loadmodel->mins[k], v[k]);
							loadmodel->maxs[k] = q_max(loadmodel->maxs[k], v[k]);
						}
						dist = v[0] * v[0] + v[1] * v[1];
						if (yawradius < dist)
							yawradius = dist;
						dist += v[2] * v[2];
						if (radius < dist)
							radius = dist;
					}
				break;
			case PV_IQM:
				//process verts
				for (i=0 ; i<a->numposes; i++)
				{
					const iqmvert_t *pv = (const iqmvert_t *)((byte*)a+a->vertexes) + i*a->numverts;
					for (j=0; j<a->numverts; j++)
					{
						for (k=0; k<3;k++)
							v[k] = pv[j].xyz[k];

						for (k=0; k<3;k++)
						{
							loadmodel->mins[k] = q_min(loadmodel->mins[k], v[k]);
							loadmodel->maxs[k] = q_max(loadmodel->maxs[k], v[k]);
						}
						dist = v[0] * v[0] + v[1] * v[1];
						if (yawradius < dist)
							yawradius = dist;
						dist += v[2] * v[2];
						if (radius < dist)
							radius = dist;
					}
				}
				break;
			}
		}

		if (!a->nextsurface)
			break;
		a = (aliashdr_t*)((byte*)a + a->nextsurface);
	}

	//rbounds will be used when entity has nonzero pitch or roll
	radius = sqrt(radius);
	loadmodel->rmins[0] = loadmodel->rmins[1] = loadmodel->rmins[2] = -radius;
	loadmodel->rmaxs[0] = loadmodel->rmaxs[1] = loadmodel->rmaxs[2] = radius;

	//ybounds will be used when entity has nonzero yaw
	yawradius = sqrt(yawradius);
	loadmodel->ymins[0] = loadmodel->ymins[1] = -yawradius;
	loadmodel->ymaxs[0] = loadmodel->ymaxs[1] = yawradius;
	loadmodel->ymins[2] = loadmodel->mins[2];
	loadmodel->ymaxs[2] = loadmodel->maxs[2];
}

static qboolean
nameInList(const char *list, const char *name)
{
	const char *s;
	char tmp[MAX_QPATH];
	int i;

	s = list;

	while (*s)
	{
		// make a copy until the next comma or end of string
		i = 0;
		while (*s && *s != ',')
		{
			if (i < MAX_QPATH - 1)
				tmp[i++] = *s;
			s++;
		}
		tmp[i] = '\0';
		//compare it to the model name
		if (!strcmp(name, tmp))
		{
			return true;
		}
		//search forwards to the next comma or end of string
		while (*s && *s == ',')
			s++;
	}
	return false;
}

/*
=================
Mod_SetExtraFlags -- johnfitz -- set up extra flags that aren't in the mdl
=================
*/
void Mod_SetExtraFlags (qmodel_t *mod)
{
	extern cvar_t r_nolerp_list;
	extern cvar_t r_noshadow_list;

	if (!mod || mod->type != mod_alias)
		return;

	mod->flags &= (0xFF | MF_HOLEY); //only preserve first byte, plus MF_HOLEY

	// nolerp flag
	if (nameInList(r_nolerp_list.string, mod->name))
		mod->flags |= MOD_NOLERP;

	// shadow casting flag
	if (nameInList(r_noshadow_list.string, mod->name))
		mod->flags |= MOD_NOSHADOW;

}

/*
=================
Mod_LoadAliasModel
=================
*/
static void Mod_LoadAliasModel (qmodel_t *mod, void *buffer)
{
	char				path[MAX_QPATH];
	unsigned int		md5_path_id;
	int					i, j;
	mdl_t				*pinmodel;
	stvert_t			*pinstverts;
	dtriangle_t			*pintriangles;
	int					version, numframes;
	int					size;
	daliasframetype_t	*pframetype;
	daliasskintype_t	*pskintype;
	int					start, end, total;

	start = Hunk_LowMark ();

	pinmodel = (mdl_t *)buffer;
	mod_base = (byte *)buffer; //johnfitz

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		Sys_Error ("%s has wrong version number (%i should be %i)",
			mod->name, version, ALIAS_VERSION);
	mod->flags = LittleLong (pinmodel->flags);

	if (r_md5.value)
	{
		COM_StripExtension (mod->name, path, sizeof (path));
		COM_AddExtension (path, ".md5mesh", sizeof (path));

		if (COM_FileExists (path, &md5_path_id))
		{
			char *md5buffer = (char *) COM_LoadMallocFile (path, NULL);
			if (md5buffer)
			{
				Mod_LoadMD5MeshModel (mod, md5buffer);
				q_free(md5buffer);
				return;
			}
		}
	}

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size	= sizeof(aliashdr_t) +
		 (LittleLong (pinmodel->numframes) - 1) * sizeof (pheader->frames[0]);
	pheader = (aliashdr_t *) Hunk_AllocName (size, loadname);
	memset (pheader->emissivetextures, 0, sizeof(pheader->emissivetextures));

//
// endian-adjust and copy the data, starting with the alias model header
//
	pheader->boundingradius = LittleFloat (pinmodel->boundingradius);
	pheader->numskins = LittleLong (pinmodel->numskins);
	pheader->skinwidth = LittleLong (pinmodel->skinwidth);
	pheader->skinheight = LittleLong (pinmodel->skinheight);

	if (pheader->skinheight > MAX_LBM_HEIGHT)
		Con_DWarning ("model %s has a skin taller than %d", mod->name,
				   MAX_LBM_HEIGHT);

	pheader->numverts = LittleLong (pinmodel->numverts);

	if (pheader->numverts <= 0)
		Sys_Error ("model %s has no vertices", mod->name);
	else if (pheader->numverts > MAXALIASVERTS)
		Sys_Error ("model %s has too many vertices (%d; max = %d)", mod->name, pheader->numverts, MAXALIASVERTS);
	else if (pheader->numverts > MAXALIASVERTS_QS && (developer.value || map_checks.value))
		Con_Warning ("model %s vertex count of %d exceeds QS limit of %d\n", mod->name, pheader->numverts, MAXALIASVERTS_QS);

	pheader->numtris = LittleLong (pinmodel->numtris);

	if (pheader->numtris <= 0)
		Sys_Error ("model %s has no triangles", mod->name);
	else if (pheader->numtris > MAXALIASTRIS_QS && (developer.value || map_checks.value))
		Con_Warning ("model %s triangle count of %d exceeds QS limit of %d\n", mod->name, pheader->numtris, MAXALIASTRIS_QS);

	pheader->numframes = LittleLong (pinmodel->numframes);
	numframes = pheader->numframes;
	if (numframes < 1)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of frames: %d", numframes);

	pheader->size = LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
	mod->synctype = (synctype_t) LittleLong (pinmodel->synctype);
	mod->numframes = pheader->numframes;

	for (i=0 ; i<3 ; i++)
	{
		pheader->scale[i] = LittleFloat (pinmodel->scale[i]);
		pheader->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
		pheader->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
	}

//
// load the skins
//
	pskintype = (daliasskintype_t *)&pinmodel[1];
	pskintype = (daliasskintype_t *) Mod_LoadAllSkins (pheader->numskins, pskintype);

//
// endian-swap base s and t vertices in place
//
	pinstverts = (stvert_t *)pskintype;
	stverts = pinstverts;

	for (i=0 ; i<pheader->numverts ; i++)
	{
		pinstverts[i].onseam = LittleLong (pinstverts[i].onseam);
		pinstverts[i].s = LittleLong (pinstverts[i].s);
		pinstverts[i].t = LittleLong (pinstverts[i].t);
	}

//
// endian-swap triangle lists in place
//
	pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];
	triangles = pintriangles;

	for (i=0 ; i<pheader->numtris ; i++)
	{
		pintriangles[i].facesfront = LittleLong (pintriangles[i].facesfront);

		for (j=0 ; j<3 ; j++)
		{
			pintriangles[i].vertindex[j] =
					LittleLong (pintriangles[i].vertindex[j]);
		}
	}

//
// load the frames
//
	posenum = 0;
	pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];

	for (i=0 ; i<numframes ; i++)
	{
		aliasframetype_t	frametype;
		frametype = (aliasframetype_t) LittleLong (pframetype->type);
		if (frametype == ALIAS_SINGLE)
			pframetype = (daliasframetype_t *) Mod_LoadAliasFrame (pframetype + 1, &pheader->frames[i]);
		else
			pframetype = (daliasframetype_t *) Mod_LoadAliasGroup (pframetype + 1, &pheader->frames[i]);
	}

	pheader->numposes = posenum;
	pheader->poseverttype = PV_QUAKE1;

	mod->type = mod_alias;

	Mod_SetExtraFlags (mod); //johnfitz

	Mod_CalcAliasBounds (pheader); //johnfitz

	//
	// build the draw lists
	//
	GL_MakeAliasModelDisplayLists (mod, pheader);

//
// move the complete, relocatable alias model to the cache
//
	end = Hunk_LowMark ();
	total = end - start;

	Cache_Alloc (&mod->cache, total, loadname);
	if (!mod->cache.data)
		return;
	memcpy (mod->cache.data, pheader, total);

	mod->sortkey = ((CRC_Block (mod->name, strlen(mod->name)) >> 1) & MODSORT_FRAMEMASK) << MODSORT_FRAMEBITS;
	if (mod->flags & MF_HOLEY)
		mod->sortkey |= MODSORT_ALIAS_ALPHATEST;
	else
		mod->sortkey &= ~MODSORT_ALIAS_ALPHATEST;

	Hunk_FreeToLowMark (start);
}

//=============================================================================

/*
=================
Mod_LoadSpriteFrame
=================
*/
static void *Mod_LoadSpriteFrame (void * pin, mspriteframe_t **ppframe, int framenum)
{
	dspriteframe_t		*pinframe;
	mspriteframe_t		*pspriteframe;
	int					width, height, size, origin[2];
	char				name[64];
	src_offset_t			offset; //johnfitz

	pinframe = (dspriteframe_t *)pin;

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height;

	pspriteframe = (mspriteframe_t *) Hunk_AllocName (sizeof (mspriteframe_t),loadname);
	*ppframe = pspriteframe;

	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong (pinframe->origin[0]);
	origin[1] = LittleLong (pinframe->origin[1]);

	pspriteframe->up = origin[1];
	pspriteframe->down = origin[1] - height;
	pspriteframe->left = origin[0];
	pspriteframe->right = width + origin[0];

	//johnfitz -- image might be padded
	pspriteframe->smax = (float)width/(float)TexMgr_PadConditional(width);
	pspriteframe->tmax = (float)height/(float)TexMgr_PadConditional(height);
	//johnfitz

	q_snprintf (name, sizeof(name), "%s:frame%i", loadmodel->name, framenum);
	offset = (src_offset_t)(pinframe+1) - (src_offset_t)mod_base; //johnfitz
	pspriteframe->gltexture =
		TexMgr_LoadImage (loadmodel, name, width, height, SRC_INDEXED,
				  (byte *)(pinframe + 1), loadmodel->name, offset,
				  TEXPREF_PAD | TEXPREF_ALPHA | TEXPREF_NOPICMIP); //johnfitz -- TexMgr

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
static void *Mod_LoadSpriteGroup (void * pin, mspriteframe_t **ppframe, int framenum, spriteframetype_t type)
{
	dspritegroup_t		*pingroup;
	mspritegroup_t		*pspritegroup;
	int					i, numframes;
	dspriteinterval_t	*pin_intervals;
	float				*poutintervals;
	void				*ptemp;

	pingroup = (dspritegroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);
	if (type == SPR_ANGLED && numframes != 8)
		Sys_Error ("Mod_LoadSpriteGroup: Bad # of frames: %d", numframes);

	pspritegroup = (mspritegroup_t *) Hunk_AllocName (sizeof (mspritegroup_t) +
				(numframes - 1) * sizeof (pspritegroup->frames[0]), loadname);

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	pin_intervals = (dspriteinterval_t *)(pingroup + 1);

	poutintervals = (float *) Hunk_AllocName (numframes * sizeof (float), loadname);

	pspritegroup->intervals = poutintervals;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadSpriteGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i], framenum * 100 + i);
	}

	return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
static void Mod_LoadSpriteModel (qmodel_t *mod, void *buffer)
{
	int					i;
	int					version;
	dsprite_t			*pin;
	msprite_t			*psprite;
	int					numframes;
	int					size;
	dspriteframetype_t	*pframetype;

	pin = (dsprite_t *)buffer;
	mod_base = (byte *)buffer; //johnfitz

	version = LittleLong (pin->version);
	if (version != SPRITE_VERSION)
		Sys_Error ("%s has wrong version number "
				 "(%i should be %i)", mod->name, version, SPRITE_VERSION);

	numframes = LittleLong (pin->numframes);

	size = sizeof (msprite_t) + (numframes - 1) * sizeof (psprite->frames);

	psprite = (msprite_t *) Hunk_AllocName (size, loadname);

	mod->cache.data = psprite;

	psprite->type = LittleLong (pin->type);
	psprite->maxwidth = LittleLong (pin->width);
	psprite->maxheight = LittleLong (pin->height);
	mod->synctype = (synctype_t) LittleLong (pin->synctype);
	psprite->numframes = numframes;

	mod->mins[0] = mod->mins[1] = -psprite->maxwidth/2;
	mod->maxs[0] = mod->maxs[1] = psprite->maxwidth/2;
	mod->mins[2] = -psprite->maxheight/2;
	mod->maxs[2] = psprite->maxheight/2;

//
// load the frames
//
	if (numframes < 1)
		Sys_Error ("Mod_LoadSpriteModel: Invalid # of frames: %d", numframes);

	mod->numframes = numframes;

	pframetype = (dspriteframetype_t *)(pin + 1);

	for (i=0 ; i<numframes ; i++)
	{
		spriteframetype_t	frametype;

		frametype = (spriteframetype_t) LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		if (frametype == SPR_SINGLE)
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteFrame (pframetype + 1, &psprite->frames[i].frameptr, i);
		}
		else
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteGroup (pframetype + 1, &psprite->frames[i].frameptr, i, frametype);
		}
	}

	mod->type = mod_sprite;
	mod->sortkey = (CRC_Block (mod->name, strlen(mod->name)) & MODSORT_MODELMASK) << MODSORT_FRAMEBITS;
}

//=============================================================================

/*
================
Mod_Print
================
*/
static void Mod_Print (void)
{
	int		i;
	qmodel_t	*mod;

	Con_SafePrintf ("Cached models:\n"); //johnfitz -- safeprint instead of print
	for (i=0, mod=mod_known ; i < mod_numknown ; i++, mod++)
	{
		Con_SafePrintf ("%8p : %s\n", mod->cache.data, mod->name); //johnfitz -- safeprint instead of print
	}
	Con_Printf ("%i models\n",mod_numknown); //johnfitz -- print the total too
}

/*
=================================================================
MD5 Models, for compat with the rerelease and NOT doom3.
=================================================================
md5mesh:
MD5Version 10
commandline ""
numJoints N
numMeshes N
joints {
"name" ParentIdx ( Pos_X Y Z ) ( Quat_X Y Z )
}
mesh {
shader "name"	//file-relative path, with _%02d_%02d postfixed for skin/framegroup support. unlike doom3.
numverts N
vert # ( S T ) FirstWeight count
numtris N
tri # A B C
numweights N
weight # BoneIdx Scale ( X Y Z )
}

md5anim:
MD5Version 10
commandline ""
numFrames N
numJoints N
frameRate FPS
numAnimatedComponents N	//bones*6ish
hierachy {
"name" ParentIdx Flags DataStart
}
bounds {
( X Y Z ) ( X Y Z )
}
baseframe {
( pos_X Y Z ) ( quad_X Y Z )
}
frame # {
RAW ...
}

We'll unpack the animation to separate framegroups (one-pose per, for consistency with most q1 models).
*/

static qboolean MD5_ParseCheck(const char *s, const char **buffer)
{
	if (strcmp(com_token, s))
		return false;
	*buffer = COM_Parse(*buffer);
	return true;
}

static size_t MD5_ParseUInt(const char **buffer)
{
	size_t i = strtoull(com_token, NULL, 0);
	*buffer = COM_Parse(*buffer);
	return i;
}
static long MD5_ParseSInt(const char **buffer)
{
	long i = strtol(com_token, NULL, 0);
	*buffer = COM_Parse(*buffer);
	return i;
}
static double MD5_ParseFloat(const char **buffer)
{
	double i = strtod(com_token, NULL);
	*buffer = COM_Parse(*buffer);
	return i;
}
#define MD5EXPECT(s) do{if (strcmp(com_token, s)) Sys_Error ("Mod_LoadMD5MeshModel(%s): Expected \"%s\"", fname, s); buffer = COM_Parse(buffer); }while(0)
#define MD5UINT() MD5_ParseUInt(&buffer)
#define MD5SINT() MD5_ParseSInt(&buffer)
#define MD5FLOAT() MD5_ParseFloat(&buffer)
#define MD5CHECK(s) MD5_ParseCheck(s, &buffer)
#define MD5IGNORE() buffer = COM_Parse(buffer)

typedef struct
{
	size_t firstweight;
	unsigned int count;
} md5vertinfo_t;
typedef struct
{
	size_t bone;
	vec4_t pos;
} md5weightinfo_t;

static void GenMatrixPosQuat4Scale(const vec3_t pos, const vec4_t quat, const vec3_t scale, float result[12])
{
	float xx, xy, xz, xw, yy, yz, yw, zz, zw;
	float x2, y2, z2;
	float s;
	x2 = quat[0] + quat[0];
	y2 = quat[1] + quat[1];
	z2 = quat[2] + quat[2];

	xx = quat[0] * x2;   xy = quat[0] * y2;   xz = quat[0] * z2;
	yy = quat[1] * y2;   yz = quat[1] * z2;   zz = quat[2] * z2;
	xw = quat[3] * x2;   yw = quat[3] * y2;   zw = quat[3] * z2;

	s = scale[0];
	result[0*4+0] = s*(1.0f - (yy + zz));
	result[1*4+0] = s*(xy + zw);
	result[2*4+0] = s*(xz - yw);

	s = scale[1];
	result[0*4+1] = s*(xy - zw);
	result[1*4+1] = s*(1.0f - (xx + zz));
	result[2*4+1] = s*(yz + xw);

	s = scale[2];
	result[0*4+2] = s*(xz + yw);
	result[1*4+2] = s*(yz - xw);
	result[2*4+2] = s*(1.0f - (xx + yy));

	result[0*4+3]  =     pos[0];
	result[1*4+3]  =     pos[1];
	result[2*4+3]  =     pos[2];
}

static void Matrix3x4_Invert_Simple (const float *in1, float *out)
{
	// we only support uniform scaling, so assume the first row is enough
	// (note the lack of sqrt here, because we're trying to undo the scaling,
	// this means multiplying by the inverse scale twice - squaring it, which
	// makes the sqrt a waste of time)
#if 1
	double scale = 1.0 / (in1[0] * in1[0] + in1[1] * in1[1] + in1[2] * in1[2]);
#else
	double scale = 3.0 / sqrt
	(in1->m[0][0] * in1->m[0][0] + in1->m[0][1] * in1->m[0][1] + in1->m[0][2] * in1->m[0][2]
		+ in1->m[1][0] * in1->m[1][0] + in1->m[1][1] * in1->m[1][1] + in1->m[1][2] * in1->m[1][2]
		+ in1->m[2][0] * in1->m[2][0] + in1->m[2][1] * in1->m[2][1] + in1->m[2][2] * in1->m[2][2]);
	scale *= scale;
#endif

	// invert the rotation by transposing and multiplying by the squared
	// recipricol of the input matrix scale as described above
	out[0] = in1[0] * scale;
	out[1] = in1[4] * scale;
	out[2] = in1[8] * scale;
	out[4] = in1[1] * scale;
	out[5] = in1[5] * scale;
	out[6] = in1[9] * scale;
	out[8] = in1[2] * scale;
	out[9] = in1[6] * scale;
	out[10] = in1[10] * scale;

	// invert the translate
	out[3] = -(in1[3] * out[0] + in1[7] * out[1] + in1[11] * out[2]);
	out[7] = -(in1[3] * out[4] + in1[7] * out[5] + in1[11] * out[6]);
	out[11] = -(in1[3] * out[8] + in1[7] * out[9] + in1[11] * out[10]);
}

static void Matrix3x4_RM_Transform4(const float *matrix, const float *vector, float *product)
{
	product[0] = matrix[0]*vector[0] + matrix[1]*vector[1] + matrix[2]*vector[2] + matrix[3]*vector[3];
	product[1] = matrix[4]*vector[0] + matrix[5]*vector[1] + matrix[6]*vector[2] + matrix[7]*vector[3];
	product[2] = matrix[8]*vector[0] + matrix[9]*vector[1] + matrix[10]*vector[2] + matrix[11]*vector[3];
}
static void MD5_BakeInfluences(const char *fname, bonepose_t *outposes, iqmvert_t *vert, md5vertinfo_t *vinfo, md5weightinfo_t *weight, size_t numverts, size_t numweights)
{
	size_t v, i, lowidx, k;
	md5weightinfo_t *w;
	vec3_t pos;
	float lowval, scale;
	unsigned int maxinfluences = 0;
	float scaleimprecision = 1;
	for (v = 0; v < numverts; v++, vert++, vinfo++)
	{
		// unquantized weights
		float weights[countof (vert->weight)];
		memset (weights, 0, sizeof (weights));

		//st were already loaded
		//norm will need to be calculated after we have xyz info
		vert->xyz[0] = vert->xyz[1] = vert->xyz[2] = 0;
		vert->idx[0] = vert->idx[1] = vert->idx[2] = vert->idx[3] = 0;

		if (vinfo->firstweight + vinfo->count > numweights)
			Sys_Error ("%s: weight index out of bounds", fname);
		if (maxinfluences < vinfo->count)
			maxinfluences = vinfo->count;
		w = weight + vinfo->firstweight;
		for (i = 0; i < vinfo->count; i++, w++)
		{
			Matrix3x4_RM_Transform4(outposes[w->bone].mat, w->pos, pos);
			VectorAdd(vert->xyz, pos, vert->xyz);

			if (i < countof(weights))
			{
				weights[i] = w->pos[3];
				vert->idx[i] = w->bone;
			}
			else
			{
				//obnoxious code to find the lowest of the current possible bone indexes.
				lowval = weights[0];
				lowidx = 0;
				for (k = 1; k < countof(weights); k++)
					if (weights[k] < lowval)
					{
						lowval = weights[k];
						lowidx = k;
					}
				if (weights[lowidx] < w->pos[3])
				{	//found a lower/unset weight, replace it.
					weights[lowidx] = w->pos[3];
					vert->idx[lowidx] = w->bone;
				}
			}
		}

		//normalize in case we dropped some weights.
		scale = weights[0] + weights[1] + weights[2] + weights[3];
		if (scale>0)
		{
			if (scaleimprecision < scale)
				scaleimprecision = scale;
			scale = 255.f/scale;
			for (k = 0; k < countof(vert->weight); k++)
				vert->weight[k] = (int) (weights[k] * scale);
		}
		else	//something bad...
			vert->weight[0] = 255, vert->weight[1] = vert->weight[2] = vert->weight[3] = 0;
	}
	if (maxinfluences > countof(vert->weight))
		Con_DWarning("%s uses up to %u influences per vertex (weakest: %g)\n", fname, maxinfluences, scaleimprecision);
}

static uint32_t HashVert (const vec3_t v)
{
	return COM_HashBlock (&v[0], 3 * sizeof (float));
}

static void MD5_ComputeNormals(iqmvert_t *vert, size_t numverts, unsigned short *indexes, size_t numindexes)
{
	size_t			v, t, hashsize;
	int				*hashmap;
	vec3_t			*normals;
	unsigned short	*weld;

	hashsize = numverts * 2;
	hashmap = (int *) q_calloc(hashsize, sizeof (*hashmap));
	weld = (unsigned short *) q_malloc(numverts * sizeof (*weld));
	normals = (vec3_t *) q_calloc(numverts, sizeof (vec3_t));
	if (!hashmap || !weld || !normals)
		Sys_Error ("MD5_ComputeNormals: out of memory (%u verts/%u tris)", (unsigned int)numverts, (unsigned int)(numindexes/3));

	for (v = 0; v < numverts; v++)
	{
		uint32_t pos = HashVert (vert[v].xyz) % hashsize;
		uint32_t end = pos;

		do
		{
			if (!hashmap[pos])
			{
				hashmap[pos] = v + 1;
				weld[v] = v;
				break;
			}

			t = hashmap[pos] - 1;
			if (VectorCompare (vert[t].xyz, vert[v].xyz))
			{
				weld[v] = weld[t];
				break;
			}

			++pos;
			if (pos == hashsize)
				pos = 0;
		}
		while (pos != end);
	}

	for (t = 0; t < numindexes; t += 3)
	{
		vec3_t d1, d2, norm;
		int i0 = weld[indexes[t+0]];
		int i1 = weld[indexes[t+1]];
		int i2 = weld[indexes[t+2]];
		iqmvert_t *v0 = &vert[i0];
		iqmvert_t *v1 = &vert[i1];
		iqmvert_t *v2 = &vert[i2];

		VectorSubtract(v1->xyz, v0->xyz, d1);
		VectorSubtract(v2->xyz, v0->xyz, d2);
		CrossProduct(d2, d1, norm);

		VectorAdd(normals[i0], norm, normals[i0]);
		VectorAdd(normals[i1], norm, normals[i1]);
		VectorAdd(normals[i2], norm, normals[i2]);
	}

	for (v = 0; v < numverts; v++)
	{
		if (v == weld[v])
		{
			VectorNormalize(normals[v]);
			vert[v].norm[0] = (int) (normals[v][0] * 127.f);
			vert[v].norm[1] = (int) (normals[v][1] * 127.f);
			vert[v].norm[2] = (int) (normals[v][2] * 127.f);
			vert[v].norm[3] = 0;
		}
		else
		{
			vert[v].norm[0] = vert[weld[v]].norm[0];
			vert[v].norm[1] = vert[weld[v]].norm[1];
			vert[v].norm[2] = vert[weld[v]].norm[2];
			vert[v].norm[3] = vert[weld[v]].norm[3];
		}
	}

	q_free(normals);
	q_free(weld);
	q_free(hashmap);
}

typedef struct
{
	char *animfile;
	const char *buffer;
	char fname[MAX_QPATH];
	size_t numposes;
	size_t numjoints;
	bonepose_t *posedata;
} md5animctx_t;
//This is split into two because aliashdr_t has silly trailing framegroup info.
static void MD5Anim_Begin(md5animctx_t *ctx, const char *fname)
{
	//Load an md5anim into it, if we can.
	COM_StripExtension(fname, ctx->fname, sizeof(ctx->fname));
	COM_AddExtension(ctx->fname, ".md5anim", sizeof(ctx->fname));
	fname = ctx->fname;
	ctx->animfile = (char *) COM_LoadMallocFile(fname, NULL);
	ctx->numposes = 0;

	if (ctx->animfile)
	{
		const char *buffer = COM_Parse(ctx->animfile);
		MD5EXPECT("MD5Version");
		MD5EXPECT("10");
		if (MD5CHECK("commandline"))	buffer = COM_Parse(buffer);
		MD5EXPECT("numFrames");	ctx->numposes = MD5UINT();
		MD5EXPECT("numJoints");	ctx->numjoints = MD5UINT();
		MD5EXPECT("frameRate"); /*irrelevant here*/

		if (ctx->numposes <= 0)
			Sys_Error ("%s has no poses", fname);

		ctx->buffer = buffer;
	}
}
static void MD5Anim_Load(md5animctx_t *ctx, boneinfo_t *bones, size_t numbones)
{
	typedef struct { unsigned int flags, offset;} md5animbase_t;
	const char *fname = ctx->fname;
	md5animbase_t *ab;
	size_t rawcount;
	float *raw, *r;
	bonepose_t *outposes, *frameposes;
	const char *buffer = COM_Parse(ctx->buffer);
	size_t j;

	if (!buffer)
	{
		q_free(ctx->animfile);
		return;
	}

	MD5EXPECT("numAnimatedComponents");	rawcount = MD5UINT();

	if (ctx->numjoints != numbones)
		Sys_Error ("%s has incorrect bone count", fname);

	raw = (float *) Z_Malloc(sizeof(*raw)*(rawcount+6));
	ab = (md5animbase_t *) Z_Malloc(sizeof(*ab)*ctx->numjoints);

	ctx->posedata = outposes = (bonepose_t *) Hunk_Alloc(sizeof(*outposes)*ctx->numjoints*ctx->numposes);
	frameposes = (bonepose_t *) q_malloc(sizeof (*frameposes) * ctx->numjoints);
	if (!frameposes)
		Sys_Error ("MD5Anim_Load: out of memory (%u joints)", (unsigned int)ctx->numjoints);


	MD5EXPECT("hierarchy");
	MD5EXPECT("{");
	for (j = 0; j < ctx->numjoints; j++)
	{
		//validate stuff
		if (strcmp(bones[j].name, com_token))
			Sys_Error ("%s: bone was renamed", fname);
		buffer = COM_Parse(buffer);
		if (bones[j].parent != MD5SINT())
			Sys_Error ("%s: bone has wrong parent", fname);
		//new info
		ab[j].flags = MD5UINT();
		if (ab[j].flags & ~63)
			Sys_Error ("%s: bone has unsupported flags", fname);
		ab[j].offset = MD5UINT();
		if (ab[j].offset > rawcount+6)
			Sys_Error ("%s: bone has bad offset", fname);
	}
	MD5EXPECT("}");
	MD5EXPECT("bounds");
	MD5EXPECT("{");
	while(MD5CHECK("("))
	{
		MD5IGNORE();
		MD5IGNORE();
		MD5IGNORE();
		MD5EXPECT(")");

		MD5EXPECT("(");
		MD5IGNORE();
		MD5IGNORE();
		MD5IGNORE();
		MD5EXPECT(")");
	}
	MD5EXPECT("}");

	MD5EXPECT("baseframe");
	MD5EXPECT("{");
	while(MD5CHECK("("))
	{
		MD5IGNORE();
		MD5IGNORE();
		MD5IGNORE();
		MD5EXPECT(")");

		MD5EXPECT("(");
		MD5IGNORE();
		MD5IGNORE();
		MD5IGNORE();
		MD5EXPECT(")");
	}
	MD5EXPECT("}");

	while(MD5CHECK("frame"))
	{
		size_t idx = MD5UINT();
		if (idx >= ctx->numposes)
			Sys_Error ("%s: invalid pose index", fname);
		MD5EXPECT("{");
		for (j = 0; j < rawcount; j++)
			raw[j] = MD5FLOAT();
		MD5EXPECT("}");

		//okay, we have our raw info, unpack the actual bone info.
		for (j = 0; j < ctx->numjoints; j++)
		{
			bonepose_t local;
			vec3_t pos = {0,0,0};
			static vec3_t scale = {1,1,1};
			vec4_t quat = {0,0,0};
			r = raw + ab[j].offset;
			if (ab[j].flags & 1)	pos[0] = *r++;
			if (ab[j].flags & 2)	pos[1] = *r++;
			if (ab[j].flags & 4)	pos[2] = *r++;

			if (ab[j].flags & 8)	quat[0] = *r++;
			if (ab[j].flags & 16)	quat[1] = *r++;
			if (ab[j].flags & 32)	quat[2] = *r++;

			quat[3] = 1 - DotProduct(quat,quat);
			if (quat[3] < 0)
				quat[3] = 0;//we have no imagination.
			quat[3] = -sqrt(quat[3]);

			GenMatrixPosQuat4Scale(pos, quat, scale, local.mat);

			if (bones[j].parent < 0)
				frameposes[j] = local;
			else
				R_ConcatTransforms ((void *) frameposes[bones[j].parent].mat, (void *) local.mat, (void *) frameposes[j].mat);
		}

		for (j = 0; j < ctx->numjoints; j++)
			R_ConcatTransforms ((void *) frameposes[j].mat, (void *) bones[j].inverse.mat, (void *) outposes[idx*ctx->numjoints + j].mat);
	}

	Z_Free(raw);
	Z_Free(ab);
	q_free(frameposes);
	q_free(ctx->animfile);
}
static void Mod_LoadMD5MeshModel (qmodel_t *mod, const char *buffer)
{
	const char			*fname = mod->name;
	unsigned short		*poutindexes;
	iqmvert_t			*poutvert;
	int					start, end, total;
	aliashdr_t			*outhdr, *surf;
	size_t				hdrsize;

	bonepose_t			*outposes;
	boneinfo_t			*outbones;

	size_t				numjoints, j;
	size_t				nummeshes, m;
	char				texname[MAX_QPATH];
	md5vertinfo_t		*vinfo;
	md5weightinfo_t		*weight;
	size_t				numweights;

	md5animctx_t		anim = {NULL};

	start = Hunk_LowMark ();

	buffer = COM_Parse(buffer);

	MD5EXPECT("MD5Version");
	MD5EXPECT("10");
	if (MD5CHECK("commandline"))	buffer = COM_Parse(buffer);
	MD5EXPECT("numJoints");	numjoints = MD5UINT();
	MD5EXPECT("numMeshes");	nummeshes = MD5UINT();

	if (numjoints <= 0)
		Sys_Error ("%s has no bones", mod->name);
	if (nummeshes <= 0)
		Sys_Error ("%s has no meshes", mod->name);

	if (strcmp(com_token, "joints")) Sys_Error ("Mod_LoadMD5MeshModel(%s): Expected \"%s\"", fname, "joints");
	MD5Anim_Begin(&anim, fname);
	buffer = COM_Parse(buffer);

        hdrsize = sizeof(*outhdr) - sizeof(outhdr->frames);
        hdrsize += sizeof(outhdr->frames)*anim.numposes;
        outhdr = (aliashdr_t *) Hunk_Alloc(hdrsize*numjoints);
        memset (outhdr, 0, hdrsize * numjoints);
        outbones = (boneinfo_t *) Hunk_Alloc(sizeof(*outbones)*numjoints);
        outposes = (bonepose_t *) Z_Malloc(sizeof(*outposes)*numjoints);

	MD5EXPECT("{");
	for (j = 0; j < numjoints; j++)
	{
		vec3_t pos;
		static vec3_t scale = {1,1,1};
		vec4_t quat;
		q_strlcpy(outbones[j].name, com_token, sizeof(outbones[j].name));	buffer = COM_Parse(buffer);
		outbones[j].parent = MD5SINT();
		if (outbones[j].parent < -1 && outbones[j].parent >= (int)numjoints)
			Sys_Error ("bone index out of bounds");
		MD5EXPECT("(");
		pos[0] = MD5FLOAT();
		pos[1] = MD5FLOAT();
		pos[2] = MD5FLOAT();
		MD5EXPECT(")");
		MD5EXPECT("(");
		quat[0] = MD5FLOAT();
		quat[1] = MD5FLOAT();
		quat[2] = MD5FLOAT();
		quat[3] = 1 - DotProduct(quat,quat);
		if (quat[3] < 0)
			quat[3] = 0;//we have no imagination.
		quat[3] = -sqrt(quat[3]);
		MD5EXPECT(")");

		GenMatrixPosQuat4Scale(pos, quat, scale, outposes[j].mat);
		Matrix3x4_Invert_Simple(outposes[j].mat, outbones[j].inverse.mat);	//absolute, so we can just invert now.
	}

	if (strcmp(com_token, "}")) Sys_Error ("Mod_LoadMD5MeshModel(%s): Expected \"%s\"", fname, "}");
	MD5Anim_Load(&anim, outbones, numjoints);
	buffer = COM_Parse(buffer);

	for (m = 0; m < nummeshes; m++)
	{
		MD5EXPECT("mesh");
		MD5EXPECT("{");

		surf = (aliashdr_t*)((byte*)outhdr + m*hdrsize);
		if (m+1 < nummeshes)
			surf->nextsurface = hdrsize;
		else
			surf->nextsurface = 0;

		surf->poseverttype = PV_IQM;
		for (j = 0; j < 3; j++)
		{
			surf->scale_origin[j] = 0;
			surf->scale[j] = 1.0;
		}

		surf->numbones = numjoints;
		surf->boneinfo = (byte*)outbones-(byte*)surf;

		if (anim.numposes)
		{
			surf->boneposedata = (byte*)anim.posedata-(byte*)surf;
			surf->numboneposes = anim.numposes;

			for (j = 0; j < anim.numposes; j++)
			{
				surf->frames[j].firstpose = j;
				surf->frames[j].numposes = 1;
				surf->frames[j].interval = 0.1;
			}
			surf->numframes = j;
		}

		MD5EXPECT("shader");
		//MD5 violation: the skin is a single material. adding prefixes/postfixes here is the wrong thing to do.
		//but we do so anyway, because rerelease compat.
		for (surf->numskins = 0; surf->numskins < MAX_SKINS; surf->numskins++)
		{
			unsigned int fwidth, fheight, f;
			enum srcformat fmt = SRC_RGBA;
			void *data;
			int mark = Hunk_LowMark ();
			for (f = 0; f < countof(surf->gltextures[0]); f++)
			{
				q_snprintf(texname, sizeof(texname), "progs/%s_%02u_%02u", com_token, surf->numskins, f);

				data = Image_LoadImage (texname, (int*)&fwidth, (int*)&fheight, &fmt);
				//now load whatever we found
				if (data) //load external image
				{
                                       surf->gltextures[surf->numskins][f] = TexMgr_LoadImage (mod, texname, fwidth, fheight, fmt, data, texname, 0, TEXPREF_ALPHA|TEXPREF_NOBRIGHT|TEXPREF_MIPMAP );
                                       surf->fbtextures[surf->numskins][f] = NULL;
                                       surf->emissivetextures[surf->numskins][f] = NULL;
					if (fmt == SRC_INDEXED)
					{	//8bit base texture. use it for fullbrights.
						if (Mod_CheckFullbrights (data, fwidth*fheight))
							surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage (mod, va("%s_luma", texname), fwidth, fheight, fmt, data, texname, 0, TEXPREF_ALPHA|TEXPREF_FULLBRIGHT|TEXPREF_MIPMAP );
					}
					Hunk_FreeToLowMark (mark);
				}
				else
					break;
			}
			if (f == 0)
				break;	//no images loaded...

			//this stuff is hideous.
                        if (f < 2)
                        {
                                surf->gltextures[surf->numskins][1] = surf->gltextures[surf->numskins][0];
                                surf->fbtextures[surf->numskins][1] = surf->fbtextures[surf->numskins][0];
                                surf->emissivetextures[surf->numskins][1] = surf->emissivetextures[surf->numskins][0];
                        }
			if (f == 3)
				Con_Warning("progs/%s_%02u_##: 3 skinframes found...\n", com_token, surf->numskins);
                        if (f < 4)
                        {
                                surf->gltextures[surf->numskins][3] = surf->gltextures[surf->numskins][1];
                                surf->gltextures[surf->numskins][2] = surf->gltextures[surf->numskins][0];

                                surf->fbtextures[surf->numskins][3] = surf->fbtextures[surf->numskins][1];
                                surf->fbtextures[surf->numskins][2] = surf->fbtextures[surf->numskins][0];

                                surf->emissivetextures[surf->numskins][3] = surf->emissivetextures[surf->numskins][1];
                                surf->emissivetextures[surf->numskins][2] = surf->emissivetextures[surf->numskins][0];
                        }
                }
		surf->skinwidth = surf->gltextures[0][0]?surf->gltextures[0][0]->width:1;
		surf->skinheight = surf->gltextures[0][0]?surf->gltextures[0][0]->height:1;
		buffer = COM_Parse(buffer);
		MD5EXPECT("numverts");
		surf->numverts_vbo = surf->numverts = MD5UINT();

		vinfo = (md5vertinfo_t *) Z_Malloc(sizeof(*vinfo)*surf->numverts);
		poutvert = (iqmvert_t *) Hunk_Alloc(sizeof(*poutvert)*surf->numverts);
		surf->vertexes = (byte*)poutvert-(byte*)surf;
		surf->numposes = 1;
		while (MD5CHECK("vert"))
		{
			size_t idx = MD5UINT();
			if (idx >= (size_t)surf->numverts)
				Sys_Error ("vertex index out of bounds");
			MD5EXPECT("(");
			poutvert[idx].st[0] = MD5FLOAT();
			poutvert[idx].st[1] = MD5FLOAT();
			MD5EXPECT(")");
			vinfo[idx].firstweight = MD5UINT();
			vinfo[idx].count = MD5UINT();
		}
		MD5EXPECT("numtris");
		surf->numtris = MD5UINT();
		surf->numindexes = surf->numtris*3;
		poutindexes = (unsigned short *) Hunk_Alloc(sizeof(*poutindexes)*surf->numindexes);
		surf->indexes = (byte*)poutindexes-(byte*)surf;
		while (MD5CHECK("tri"))
		{
			size_t idx = MD5UINT();
			if (idx >= (size_t)surf->numtris)
				Sys_Error ("triangle index out of bounds");
			idx *= 3;
			for (j = 0; j < 3; j++)
			{
				size_t t = MD5UINT();
				if (t >= (size_t)surf->numverts)
					Sys_Error ("vertex index out of bounds");
				poutindexes[idx+j] = t;
			}
		}

		//md5 is a gpu-unfriendly interchange format. :(
		MD5EXPECT("numweights");
		numweights = MD5UINT();
		weight = (md5weightinfo_t *) Z_Malloc(sizeof(*weight)*numweights);
		while (MD5CHECK("weight"))
		{
			size_t idx = MD5UINT();
			if (idx >= numweights)
				Sys_Error ("weight index out of bounds");

			weight[idx].bone = MD5UINT();
			if (weight[idx].bone >= numjoints)
				Sys_Error ("bone index out of bounds");
			weight[idx].pos[3] = MD5FLOAT();
			MD5EXPECT("(");
			weight[idx].pos[0] = MD5FLOAT()*weight[idx].pos[3];
			weight[idx].pos[1] = MD5FLOAT()*weight[idx].pos[3];
			weight[idx].pos[2] = MD5FLOAT()*weight[idx].pos[3];
			MD5EXPECT(")");
		}
		//so make it gpu-friendly.
		MD5_BakeInfluences(fname, outposes, poutvert, vinfo, weight, surf->numverts, numweights);
		//and now make up the normals that the format lacks. we'll still probably have issues from seams, but then so did qme, so at least its faithful... :P
		MD5_ComputeNormals(poutvert, surf->numverts, poutindexes, surf->numindexes);

		Z_Free(weight);
		Z_Free(vinfo);

		MD5EXPECT("}");
	}
	Z_Free(outposes);

	GLMesh_LoadVertexBuffer (mod, outhdr);

	// Note: the md5 format does not have its own modelflags, yet we still need to know about trails and rotating etc, so we reuse the flags from the mdl version.

	mod->synctype = ST_FRAMETIME;	//keep IQM animations synced to when .frame is changed. framegroups are otherwise not very useful.
	mod->type = mod_alias;

	Mod_CalcAliasBounds (outhdr); //johnfitz

	//
	// move the complete, relocatable alias model to the cache
	//
	end = Hunk_LowMark ();
	total = end - start;

	Cache_Alloc (&mod->cache, total, loadname);
	if (!mod->cache.data)
		return;
	memcpy (mod->cache.data, outhdr, total);

	Hunk_FreeToLowMark (start);
}
