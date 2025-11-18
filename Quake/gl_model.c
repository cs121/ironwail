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

#define IDMD2HEADER    (('2'<<24)+('P'<<16)+('D'<<8)+'I')

static qmodel_t*	loadmodel;
static char	loadname[32];	// for hunk tags

static void Mod_LoadSpriteModel (qmodel_t *mod, void *buffer);
static void Mod_LoadBrushModel (qmodel_t *mod, void *buffer);
static void Mod_LoadAliasModel (qmodel_t *mod, void *buffer);
static void Mod_LoadMD2Model (qmodel_t *mod, void *buffer);
static void Mod_LoadMD5MeshModel (qmodel_t *mod, const char *buffer);
static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash);
static void Mod_LoadBspx (const byte *buffer);

static void Mod_Print (void);

static cvar_t	external_ents = {"external_ents", "1", CVAR_ARCHIVE};
static cvar_t	external_vis = {"external_vis", "1", CVAR_ARCHIVE};
cvar_t		debug_bspx = {"debug_bspx", "0", 0};
cvar_t			r_md5 = {"r_md5", "1", CVAR_ARCHIVE};
extern cvar_t			r_lightgrid_debug;

static byte	*mod_novis;
static int	mod_novis_capacity;

static byte	*mod_decompressed;
static int	mod_decompressed_capacity;
static size_t   mod_bspx_filesize;

static size_t Mod_BspLumpElementSize (int lump_index, int bsp2)
{
	switch (lump_index)
	{
	case LUMP_PLANES: return sizeof(dplane_t);
	case LUMP_VERTEXES: return sizeof(dvertex_t);
	case LUMP_EDGES: return bsp2 ? sizeof(dledge_t) : sizeof(dsedge_t);
	case LUMP_SURFEDGES: return sizeof(int);
	case LUMP_FACES: return bsp2 ? sizeof(dlface_t) : sizeof(dsface_t);
	case LUMP_MARKSURFACES: return bsp2 ? sizeof(unsigned int) : sizeof(unsigned short);
	case LUMP_NODES: return (bsp2 == 2) ? sizeof(dl2node_t) : (bsp2 ? sizeof(dl1node_t) : sizeof(dsnode_t));
	case LUMP_TEXINFO: return sizeof(texinfo_t);
	case LUMP_LEAFS: return (bsp2 == 2) ? sizeof(dl2leaf_t) : (bsp2 ? sizeof(dl1leaf_t) : sizeof(dsleaf_t));
	case LUMP_CLIPNODES: return bsp2 ? sizeof(dlclipnode_t) : sizeof(dsclipnode_t);
	case LUMP_MODELS: return sizeof(dmodel_t);
	default: return 0; // variable sized lumps
	}
}

static int Mod_ParseBspHeader (const byte *buffer, size_t filesize, dheader_t *out_header)
{
	int bsp2;
	const dheader_t *raw_header;

	if (filesize < sizeof(dheader_t))
		Sys_Error ("Mod_LoadBrushModel: %s is not a valid BSP (truncated header)", loadmodel->name);

	raw_header = (const dheader_t *)buffer;
	out_header->version = LittleLong (raw_header->version);

	switch(out_header->version)
	{
	case BSPVERSION:
	case BSPVERSION30:
	case BSPVERSION_QUAKE64:
		bsp2 = false;
		break;
	case BSP2VERSION_2PSB:
		bsp2 = 1;
		break;
	case BSP2VERSION_BSP2:
		bsp2 = 2;
		break;
	default:
		Sys_Error ("Mod_LoadBrushModel: %s has unsupported or missing BSP version (%i)", loadmodel->name, out_header->version);
		bsp2 = false; // not reached
	}

	for (int i = 0; i < HEADER_LUMPS; ++i)
	{
		const int raw_ofs = LittleLong (raw_header->lumps[i].fileofs);
		const int raw_len = LittleLong (raw_header->lumps[i].filelen);
		const size_t elemsize = Mod_BspLumpElementSize (i, bsp2);
		size_t lumpofs, lumplen;

		if (raw_ofs < 0 || raw_len < 0)
			Sys_Error ("Mod_LoadBrushModel: %s has invalid lump %d offset/length", loadmodel->name, i);

		lumpofs = (size_t)raw_ofs;
		lumplen = (size_t)raw_len;

		if (lumpofs > filesize || lumplen > filesize - lumpofs)
			Sys_Error ("Mod_LoadBrushModel: %s lump %d (ofs %d len %d) is outside file bounds", loadmodel->name, i, raw_ofs, raw_len);

		if (elemsize && (lumplen % elemsize))
			Sys_Error ("Mod_LoadBrushModel: %s has misaligned lump %d (len %zu, element %zu)", loadmodel->name, i, lumplen, elemsize);

		out_header->lumps[i].fileofs = raw_ofs;
		out_header->lumps[i].filelen = raw_len;
	}

	return bsp2;
}

static void Mod_BspxDebugf (const char *fmt, ...)
{
        if (debug_bspx.value <= 0.0f && r_lightgrid_debug.value <= 0.0f)
                return;

        char buffer[1024];
        va_list argptr;

        va_start (argptr, fmt);
        q_vsnprintf (buffer, sizeof(buffer), fmt, argptr);
        va_end (argptr);

        Con_Printf ("%s", buffer);
}

static void Mod_LoadExternalBspx (void)
{
        char bspxname[MAX_QPATH];
        byte *buffer;
        unsigned int path_id;
        qfileofs_t saved_filesize;
        size_t saved_bspx_size;

	COM_StripExtension (loadmodel->name, bspxname, sizeof(bspxname));

	q_strlcat (bspxname, ".bspx", sizeof(bspxname));

        if (!q_strcasecmp (bspxname, loadmodel->name))
                return;

        buffer = COM_LoadMallocFile (bspxname, &path_id);
        if (!buffer)
                return;

        saved_filesize = com_filesize;
        saved_bspx_size = mod_bspx_filesize;

        if (com_filesize > 0)
        {
                mod_bspx_filesize = (size_t)com_filesize;
                Mod_BspxDebugf ("BSPX: loading %s companion (%zu bytes)\n", bspxname, mod_bspx_filesize);
                Mod_LoadBspx ((const byte *)buffer);
        }

        mod_bspx_filesize = saved_bspx_size;
        com_filesize = saved_filesize;

        free (buffer);
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
	Cvar_RegisterVariable (&debug_bspx);
	Cvar_RegisterVariable (&r_lightgrid_debug);
	Cvar_RegisterVariable (&r_md5);
	Cvar_SetCallback (&r_md5, R_MD5_f);

	Cmd_AddCommand ("mcache", Mod_Print);

	//johnfitz -- create notexture miptex
	r_notexture_mip = (texture_t *) Hunk_AllocName (sizeof(texture_t), "r_notexture_mip");
	strcpy (r_notexture_mip->name, "notexture");
	r_notexture_mip->height = r_notexture_mip->width = 32;

	r_notexture_mip2 = (texture_t *) Hunk_AllocName (sizeof(texture_t), "r_notexture_mip2");
	strcpy (r_notexture_mip2->name, "notexture2");
	r_notexture_mip2->height = r_notexture_mip2->width = 32;
	//johnfitz
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
		byte	*newbuffer;

		mod_decompressed_capacity = (row + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK;
		newbuffer = (byte *) realloc (mod_decompressed, mod_decompressed_capacity);
		if (!newbuffer)
			Sys_Error ("Mod_DecompressVis: realloc() failed on %d bytes", mod_decompressed_capacity);
		mod_decompressed = newbuffer;
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
		byte	*newbuffer;

		mod_novis_capacity = pvsbytes;
		newbuffer = (byte *) realloc (mod_novis, mod_novis_capacity);
		if (!newbuffer)
			Sys_Error ("Mod_NoVisPVS: realloc() failed on %d bytes", mod_novis_capacity);

		mod_novis = newbuffer;
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

static qboolean Mod_TryResolvedName (const char *candidate, char *resolved, size_t resolved_size)
{
        if (COM_FileExists (candidate, NULL))
        {
                q_strlcpy (resolved, candidate, resolved_size);
                return true;
        }

        return false;
}

static void Mod_ResolveModelName (const char *name, char *resolved, size_t resolved_size)
{
        const char *ext;
        static const char * const model_exts[] = {"md5", "md2", "mdl"};
        char base[MAX_QPATH];
        size_t i;

        if (!resolved_size)
                return;

        if (!name[0])
        {
                resolved[0] = '\0';
                return;
        }

        if (name[0] == '*')
        {
                q_strlcpy (resolved, name, resolved_size);
                return;
        }

        ext = COM_FileGetExtension (name);

        if (ext[0] && q_strcasecmp (ext, "md5") && q_strcasecmp (ext, "md2") && q_strcasecmp (ext, "mdl"))
        {
                q_strlcpy (resolved, name, resolved_size);
                return;
        }

        if (!COM_StripModelExtension (name, base, sizeof(base)))
                q_strlcpy (base, name, sizeof(base));

        for (i = 0; i < countof(model_exts); i++)
        {
                char candidate[MAX_QPATH];

                q_snprintf (candidate, sizeof(candidate), "%s.%s", base, model_exts[i]);
                if (Mod_TryResolvedName (candidate, resolved, resolved_size))
                        return;
        }

        if (ext[0])
                q_snprintf (resolved, resolved_size, "%s.%s", base, ext);
        else
                q_snprintf (resolved, resolved_size, "%s.mdl", base);
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
	char		resolved[MAX_QPATH];

	Mod_ResolveModelName (name, resolved, sizeof(resolved));
	mod = Mod_FindName (resolved);

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
	buf = COM_LoadMallocFile (mod->name, &mod->path_id);
	if (!buf)
	{
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

        mod_type = (buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
        switch (mod_type)
        {
        case IDPOLYHEADER:
                Mod_LoadAliasModel (mod, buf);
                break;
	//Spike -- added checks for a few other model types.
	//this is useful because of the number of models with renamed extensions.
	//that and its hard to test the extension stuff when this was crashing.
	case (('R'<<0)+('A'<<8)+('P'<<16)+('O'<<24)):	//h2mp
		Con_Warning("%s is a hexen2-missionpack model, and is thus not supported\n", mod->name);
		mod->type = mod_ext_invalid;
		break;
	case (('I'<<0)+('D'<<8)+('P'<<16)+('3'<<24)):	//md3
		Con_Warning("%s is an md3, and is thus not supported\n", mod->name);
		mod->type = mod_ext_invalid;
		break;
	case (('I'<<0)+('N'<<8)+('T'<<16)+('E'<<24)):	//iqm
		Con_Warning("%s is an iqm, and is thus not supported\n", mod->name);
		mod->type = mod_ext_invalid;
		break;
	case (('D'<<0)+('A'<<8)+('R'<<16)+('K'<<24)):	//dpm
		Con_Warning("%s is an dpm, and is thus not supported\n", mod->name);
		mod->type = mod_ext_invalid;
		break;
	case (('A'<<0)+('C'<<8)+('T'<<16)+('R'<<24)):	//psk
		Con_Warning("%s is a psk, and is thus not supported\n", mod->name);
		mod->type = mod_ext_invalid;
		break;
	case (('I'<<0)+('B'<<8)+('S'<<16)+('P'<<24)):	//q2/q3bsp
		Con_Warning("%s is a q2/q3bsp, and is thus not supported\n", mod->name);
		mod->type = mod_ext_invalid;
		break;
        case IDMD2HEADER:
                Mod_LoadMD2Model (mod, buf);
                break;

        case IDSPRITEHEADER:
                Mod_LoadSpriteModel (mod, buf);
                break;

        default:
                Mod_LoadBrushModel (mod, buf);
                break;
        }

        if (crash && mod->type == mod_ext_invalid)
        {
                Host_Error ("Mod_LoadModel: couldn't load %s", mod->name);
                return NULL;
        }

        free (buf);

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
	char		resolved[MAX_QPATH];

	Mod_ResolveModelName (name, resolved, sizeof(resolved));
	mod = Mod_FindName (resolved);

	return Mod_LoadModel (mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static byte	*mod_base;

typedef struct {
    char lumpname[24]; // up to 23 chars, zero-padded
    int fileofs;  // from file start
    int filelen;
} bspx_lump_t;
typedef struct {
    char id[4];  // 'BSPX'
    int numlumps;
	bspx_lump_t lumps[1];
} bspx_header_t;
static char *bspxbase;
static bspx_header_t *bspxheader;
//supported lumps:
//RGBLIGHTING (.lit)
//unsupported lumps ('documented' elsewhere):
//BRUSHLIST (because hulls suck)
//LIGHTINGDIR (.lux)
//LMSHIFT (.lit2)
//LMOFFSET (.lit2)
//LMSTYLE (.lit2)
static void *Q1BSPX_FindLump(char *lumpname, int *lumpsize)
{
	int i;
	*lumpsize = 0;
	if (!bspxheader)
		return NULL;

	for (i = 0; i < bspxheader->numlumps; i++)
	{
		if (!strncmp(bspxheader->lumps[i].lumpname, lumpname, 24))
		{
			*lumpsize = bspxheader->lumps[i].filelen;
			return bspxbase + bspxheader->lumps[i].fileofs;
		}
	}
	return NULL;
}
static void Q1BSPX_Setup(qmodel_t *mod, char *filebase, unsigned int filelen, lump_t *lumps, int numlumps)
{
	int i;
	int offs = 0;
	bspx_header_t *h;

	bspxbase = filebase;
	bspxheader = NULL;

	for (i = 0; i < numlumps; i++, lumps++)
	{
		if (offs < lumps->fileofs + lumps->filelen)
			offs = lumps->fileofs + lumps->filelen;
	}
	offs = (offs + 3) & ~3;
	if (offs + sizeof(*bspxheader) > filelen)
		return; /*no space for it*/
	h = (bspx_header_t*)(filebase + offs);

	i = LittleLong(h->numlumps);
	/*verify the header*/
	if (!strncmp(h->id, "BSPX", 4) ||
		i < 0 ||
		offs + sizeof(*h) + sizeof(h->lumps[0])*(i-1) > filelen)
		return;
	h->numlumps = i;
	while(i-->0)
	{
		h->lumps[i].fileofs = LittleLong(h->lumps[i].fileofs);
		h->lumps[i].filelen = LittleLong(h->lumps[i].filelen);
		if ((unsigned int)h->lumps[i].fileofs + (unsigned int)h->lumps[i].filelen > filelen)
			return;
	}

	bspxheader = h;
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
	src_offset_t		offset;
	int			mark, fwidth, fheight;
	char		filename[MAX_OSPATH], mapname[MAX_OSPATH];
	byte		*data;
	enum srcformat fmt;
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

		if (!isDedicated) //no texture uploading for dedicated server
		{
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
				COM_StripExtension (loadmodel->name + 5, mapname, sizeof(mapname));
				q_snprintf (filename, sizeof(filename), "textures/%s/#%s", mapname, tx->name+1); //this also replaces the '*' with a '#'
				data = Image_LoadImage (filename, &fwidth, &fheight, &fmt);
				if (!data)
				{
					q_snprintf (filename, sizeof(filename), "textures/#%s", tx->name+1);
					data = Image_LoadImage (filename, &fwidth, &fheight, &fmt);
				}

				//now load whatever we found
				if (data) //load external image
				{
					q_strlcpy (texturename, filename, sizeof(texturename));
					tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, fwidth, fheight,
						fmt, data, filename, 0, TEXPREF_MIPMAP | TEXPREF_BINDLESS);
				}
				else //use the texture from the bsp file
				{
					q_snprintf (texturename, sizeof(texturename), "%s:%s", loadmodel->name, tx->name);
					offset = (src_offset_t)(mt+1) - (src_offset_t)mod_base;
					tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
						SRC_INDEXED, (byte *)(tx+1), loadmodel->name, offset, TEXPREF_MIPMAP | TEXPREF_BINDLESS);
				}
			}
			else //regular texture
			{
				int	extraflags = TEXPREF_BINDLESS;
				if (tx->type == TEXTYPE_CUTOUT)
					extraflags |= TEXPREF_ALPHA;

				//external textures -- first look in "textures/mapname/" then look in "textures/"
				mark = Hunk_LowMark ();
				COM_StripExtension (loadmodel->name + 5, mapname, sizeof(mapname));
				q_snprintf (filename, sizeof(filename), "textures/%s/%s", mapname, tx->name);
				data = Image_LoadImage (filename, &fwidth, &fheight, &fmt);
				if (!data)
				{
					q_snprintf (filename, sizeof(filename), "textures/%s", tx->name);
					data = Image_LoadImage (filename, &fwidth, &fheight, &fmt);
				}

				//now load whatever we found
                               if (data) //load external image
                               {
                                       char filename2[MAX_OSPATH];
                                       tx->gltexture = TexMgr_LoadImage (loadmodel, filename, fwidth, fheight,
                                               fmt, data, filename, 0, TEXPREF_MIPMAP | extraflags );

                                       Hunk_FreeToLowMark (mark);
                                       mark = Hunk_LowMark ();
                                       q_snprintf (filename2, sizeof(filename2), "%s_emissive", filename);
                                       data = Image_LoadImage (filename2, &fwidth, &fheight, &fmt);
                                       if (data)
                                               tx->emissive = TexMgr_LoadImage (loadmodel, filename2, fwidth, fheight,
                                                       fmt, data, filename2, 0, TEXPREF_MIPMAP | extraflags );

                                       Hunk_FreeToLowMark (mark);
                                       mark = Hunk_LowMark ();
                                       q_snprintf (filename2, sizeof(filename2), "%s_glow", filename);
                                       data = Image_LoadImage (filename2, &fwidth, &fheight, &fmt);
                                       if (!data)
                                       {
                                               q_snprintf (filename2, sizeof(filename2), "%s_luma", filename);
                                               data = Image_LoadImage (filename2, &fwidth, &fheight, &fmt);
                                       }

                                       if (data)
                                               tx->fullbright = TexMgr_LoadImage (loadmodel, filename2, fwidth, fheight,
                                                       fmt, data, filename2, 0, TEXPREF_MIPMAP | extraflags );
                               }
				else //use the texture from the bsp file
				{
					q_snprintf (texturename, sizeof(texturename), "%s:%s", loadmodel->name, tx->name);
					offset = (src_offset_t)(mt+1) - (src_offset_t)mod_base;
					if (Mod_CheckFullbrights ((byte *)(tx+1), pixels))
					{
						if (tx->type != TEXTYPE_CUTOUT)
						{
							tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
								SRC_INDEXED, (byte *)(tx+1), loadmodel->name, offset, TEXPREF_MIPMAP | TEXPREF_ALPHABRIGHT | extraflags);
						}
						else
						{
							tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
								SRC_INDEXED, (byte *)(tx+1), loadmodel->name, offset, TEXPREF_MIPMAP | TEXPREF_NOBRIGHT | extraflags);
							q_snprintf (texturename, sizeof(texturename), "%s:%s_glow", loadmodel->name, tx->name);
							tx->fullbright = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
								SRC_INDEXED, (byte *)(tx+1), loadmodel->name, offset, TEXPREF_MIPMAP | TEXPREF_FULLBRIGHT | extraflags);
						}
					}
					else
					{
						tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
							SRC_INDEXED, (byte *)(tx+1), loadmodel->name, offset, TEXPREF_MIPMAP | extraflags);
					}
				}
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
static qboolean Mod_LoadDeluxemapFile (const char *filename, int samplecount, int expected_version, const char *filekind)
{
        int mark;
        byte *data;
        unsigned int path_id;
        size_t expected;

        if (!filename || !filename[0])
                return false;

        expected = (size_t)samplecount * 3;
        mark = Hunk_LowMark ();
        data = (byte *) COM_LoadHunkFile (filename, &path_id);
        if (!data)
        {
                Hunk_FreeToLowMark (mark);
                return false;
        }

        if (path_id < loadmodel->path_id)
        {
                Hunk_FreeToLowMark (mark);
                Con_DPrintf ("ignored %s from a gamedir with lower priority\n", filename);
                return false;
        }

        if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
        {
                int version = LittleLong (((int *)data)[1]);
                if (version == expected_version)
                {
                        uint64_t expected_total = 8 + (uint64_t)expected;
                        if (expected_total == (uint64_t)com_filesize)
                        {
                                Con_DPrintf2 ("%s loaded\n", filename);
                                loadmodel->deluxdata = data + 8;
                                loadmodel->deluxfile = true;
                                return true;
                        }

                        Hunk_FreeToLowMark (mark);
                        Con_Printf ("Mismatched %s file (%s should be %" SDL_PRIu64 " bytes, not %" SDL_PRIs64 ")\n",
                                filekind, filename, expected_total, com_filesize);
                        return false;
                }

                Hunk_FreeToLowMark (mark);
                Con_Printf ("Unknown %s file version (%d)\n", filekind, version);
                return false;
        }

        Hunk_FreeToLowMark (mark);
        Con_Printf ("Corrupt %s file (old version?), ignoring\n", filekind);
        return false;
}

static void Mod_LoadDeluxemap (const char *dlitfilename, const char *luxfilename, int samplecount)
{
        loadmodel->deluxdata = NULL;
        loadmodel->deluxfile = false;
        loadmodel->numdeluxsamples = (samplecount > 0) ? samplecount : 0;

        if (samplecount <= 0)
                return;

        if (Mod_LoadDeluxemapFile (dlitfilename, samplecount, 2, ".dlit"))
                return;

        if (Mod_LoadDeluxemapFile (luxfilename, samplecount, 1, ".lux"))
                return;

        Con_DPrintf2 ("%s and %s not found, synthesizing deluxemap\n", dlitfilename, luxfilename);

        loadmodel->deluxdata = (byte *) Hunk_AllocNameNoFill (samplecount * 3, dlitfilename);
        memset (loadmodel->deluxdata, 0, samplecount * 3);
}

static byte Mod_BspxEncodeDeluxComponent (float value)
{
        float c = CLAMP (-1.0f, value, 1.0f);
        int v = (int) Q_rint ((c * 0.5f + 0.5f) * 255.0f);
        return (byte) CLAMP (0, v, 255);
}

static byte Mod_BspxConvertE5Bgr9Component (uint32_t mantissa, uint32_t exponent)
{
        float scale = ldexp (1.0f, (int)exponent - 24);
        int value = (int) Q_rint ((float)mantissa * scale * 128.0f);
        return (byte) CLAMP (0, value, 255);
}

static void Mod_BspxDecodeE5Bgr9Sample (uint32_t packed, byte *dst)
{
        uint32_t value = (uint32_t) LittleLong ((int)packed);
        uint32_t exponent = value >> 27;

        dst[2] = Mod_BspxConvertE5Bgr9Component (value & 0x1ffu, exponent);
        dst[1] = Mod_BspxConvertE5Bgr9Component ((value >> 9) & 0x1ffu, exponent);
        dst[0] = Mod_BspxConvertE5Bgr9Component ((value >> 18) & 0x1ffu, exponent);
}

static size_t Mod_BspxGuessSampleCount (size_t length)
{
        size_t count = 0;

        if (length % (3 * sizeof(float)) == 0)
                count = length / (3 * sizeof(float));
        else if (length % 3 == 0)
                count = length / 3;

        return count;
}

static qboolean Mod_BspxEnsureDeluxStorage (size_t samplecount, const char *lumpname)
{
        size_t expected_bytes;

        if (samplecount == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s reported zero samples\n", lumpname);
                return false;
        }

        if (loadmodel->numdeluxsamples > 0 && (size_t)loadmodel->numdeluxsamples != samplecount)
        {
                Con_DPrintf2 ("BSPX lump %s has unexpected sample count %zu (expected %d)\n",
                        lumpname, samplecount, loadmodel->numdeluxsamples);
                Mod_BspxDebugf ("BSPX:     %s sample count mismatch (%zu vs %d)\n",
                        lumpname, samplecount, loadmodel->numdeluxsamples);
                return false;
        }

        if (loadmodel->numdeluxsamples == 0)
                loadmodel->numdeluxsamples = (int)samplecount;

        expected_bytes = samplecount * 3;

        if (!loadmodel->deluxdata)
        {
                loadmodel->deluxdata = (byte *) Hunk_AllocNameNoFill (expected_bytes, loadname);
                if (!loadmodel->deluxdata)
                {
                        Mod_BspxDebugf ("BSPX:     %s failed to allocate %zu bytes for deluxemap\n", lumpname, expected_bytes);
                        return false;
                }
                memset (loadmodel->deluxdata, 0, expected_bytes);
        }

        return true;
}

static qboolean Mod_BspxImportDeluxemap (const char *lumpname, const byte *data, size_t length)
{
        const byte *payload = data;
        size_t payload_length = length;
        size_t samplecount = (loadmodel->numdeluxsamples > 0) ? (size_t)loadmodel->numdeluxsamples : 0;
        size_t expected_bytes;
        size_t expected_floats;
        qboolean from_qlit = false;

        if (!data || length == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s has no payload\n", lumpname);
                return false;
        }

        if (length >= 8 && data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
        {
                int version = LittleLong (((const int *)data)[1]);

                from_qlit = true;
                payload = data + 8;
                payload_length = length - 8;

                if (samplecount == 0)
                        samplecount = Mod_BspxGuessSampleCount (payload_length);

                if (!Mod_BspxEnsureDeluxStorage (samplecount, lumpname))
                        return false;

                expected_bytes = samplecount * 3;
                expected_floats = samplecount * 3 * sizeof(float);

                if (version == 2)
                {
                        if (payload_length == expected_bytes)
                        {
                                memcpy (loadmodel->deluxdata, payload, expected_bytes);
                                loadmodel->deluxfile = true;
                                Mod_BspxDebugf ("BSPX:     %s imported %zu delux samples (QLIT bytes)\n", lumpname, samplecount);
                                return true;
                        }

                        if (payload_length == expected_floats)
                        {
                                const float *src = (const float *)payload;
                                byte *dst = loadmodel->deluxdata;
                                size_t count = samplecount;

                                for (size_t i = 0; i < count; ++i)
                                {
                                        float nx = LittleFloat (src[i * 3 + 0]);
                                        float ny = LittleFloat (src[i * 3 + 1]);
                                        float nz = LittleFloat (src[i * 3 + 2]);

                                        dst[i * 3 + 0] = Mod_BspxEncodeDeluxComponent (nx);
                                        dst[i * 3 + 1] = Mod_BspxEncodeDeluxComponent (ny);
                                        dst[i * 3 + 2] = Mod_BspxEncodeDeluxComponent (nz);
                                }

                                loadmodel->deluxfile = true;
                                Mod_BspxDebugf ("BSPX:     %s imported %zu delux samples (QLIT floats)\n", lumpname, samplecount);
                                return true;
                        }

                        Con_DPrintf2 ("BSPX lump %s has unexpected version 2 size %zu (expected %zu or %zu)\n",
                                lumpname, payload_length, expected_bytes, expected_floats);
                        Mod_BspxDebugf ("BSPX:     %s rejected QLIT v2 size %zu (expected %zu or %zu)\n",
                                lumpname, payload_length, expected_bytes, expected_floats);
                        return false;
                }

                if (version == 1)
                {
                        if (payload_length == expected_floats)
                        {
                                const float *src = (const float *)payload;
                                byte *dst = loadmodel->deluxdata;
                                size_t count = samplecount;

                                for (size_t i = 0; i < count; ++i)
                                {
                                        float nx = LittleFloat (src[i * 3 + 0]);
                                        float ny = LittleFloat (src[i * 3 + 1]);
                                        float nz = LittleFloat (src[i * 3 + 2]);

                                        dst[i * 3 + 0] = Mod_BspxEncodeDeluxComponent (nx);
                                        dst[i * 3 + 1] = Mod_BspxEncodeDeluxComponent (ny);
                                        dst[i * 3 + 2] = Mod_BspxEncodeDeluxComponent (nz);
                                }

                                loadmodel->deluxfile = true;
                                Mod_BspxDebugf ("BSPX:     %s imported %zu delux samples (QLIT v1 floats)\n", lumpname, samplecount);
                                return true;
                        }

                        Con_DPrintf2 ("BSPX lump %s has unexpected version 1 size %zu (expected %zu)\n",
                                lumpname, payload_length, expected_floats);
                        Mod_BspxDebugf ("BSPX:     %s rejected QLIT v1 size %zu (expected %zu)\n",
                                lumpname, payload_length, expected_floats);
                        return false;
                }

                Con_DPrintf2 ("BSPX lump %s has unknown QLIT version %d\n", lumpname, version);
                Mod_BspxDebugf ("BSPX:     %s has unknown QLIT version %d\n", lumpname, version);
                return false;
        }
        else
        {
                if (samplecount == 0)
                        samplecount = Mod_BspxGuessSampleCount (length);

                if (!Mod_BspxEnsureDeluxStorage (samplecount, lumpname))
                        return false;

                expected_bytes = samplecount * 3;
                expected_floats = samplecount * 3 * sizeof(float);

                if (length == expected_bytes)
                {
                        memcpy (loadmodel->deluxdata, data, expected_bytes);
                        loadmodel->deluxfile = true;
                        Mod_BspxDebugf ("BSPX:     %s imported %zu delux samples (raw bytes)\n", lumpname, samplecount);
                        return true;
                }

                if (length == expected_floats)
                {
                        const float *src = (const float *)data;
                        byte *dst = loadmodel->deluxdata;
                        size_t count = samplecount;

                        for (size_t i = 0; i < count; ++i)
                        {
                                float nx = LittleFloat (src[i * 3 + 0]);
                                float ny = LittleFloat (src[i * 3 + 1]);
                                float nz = LittleFloat (src[i * 3 + 2]);

                                dst[i * 3 + 0] = Mod_BspxEncodeDeluxComponent (nx);
                                dst[i * 3 + 1] = Mod_BspxEncodeDeluxComponent (ny);
                                dst[i * 3 + 2] = Mod_BspxEncodeDeluxComponent (nz);
                        }

                        loadmodel->deluxfile = true;
                        Mod_BspxDebugf ("BSPX:     %s imported %zu delux samples (raw floats)\n", lumpname, samplecount);
                        return true;
                }
        }

        expected_bytes = (size_t)loadmodel->numdeluxsamples * 3;
        expected_floats = expected_bytes * sizeof(float);

        Con_DPrintf2 ("BSPX lump %s has unexpected size %zu (expected %zu or %zu)\n",
                lumpname, length, expected_bytes, expected_floats);
        Mod_BspxDebugf ("BSPX:     %s rejected size %zu (expected %zu or %zu)\n",
                lumpname, length, expected_bytes, expected_floats);
        return false;
}

static qboolean Mod_BspxImportRgbLighting (const char *lumpname, const byte *data, size_t length)
{
        const byte *payload = data;
        size_t payload_length = length;
        size_t samplecount;
        size_t expected_bytes;
        qboolean had_header = false;

        if (!data || length == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s has no payload\n", lumpname);
                return false;
        }

        if (length >= 8 && data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
        {
                int version = LittleLong (((const int *)data)[1]);

                had_header = true;
                payload = data + 8;
                payload_length = length - 8;

                if (version != 1)
                {
                        Con_DPrintf2 ("BSPX lump %s has unsupported QLIT version %d\n", lumpname, version);
                        Mod_BspxDebugf ("BSPX:     %s has unsupported QLIT version %d\n", lumpname, version);
                        return false;
                }
        }

        samplecount = Mod_BspxGuessSampleCount (payload_length);
        if (samplecount == 0 || payload_length != samplecount * 3)
        {
                Con_DPrintf2 ("BSPX lump %s has unexpected size %zu (expected multiples of 3 bytes)\n",
                        lumpname, payload_length);
                Mod_BspxDebugf ("BSPX:     %s size %zu is not a multiple of 3 bytes\n", lumpname, payload_length);
                return false;
        }

        if (!Mod_BspxEnsureDeluxStorage (samplecount, lumpname))
        {
                Mod_BspxDebugf ("BSPX:     %s rejected due to deluxemap storage mismatch\n", lumpname);
                return false;
        }

        expected_bytes = samplecount * 3;

        if (!loadmodel->lightdata)
        {
                loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill (expected_bytes, loadname);
                if (!loadmodel->lightdata)
                {
                        Mod_BspxDebugf ("BSPX:     %s failed to allocate %zu bytes for lightdata\n", lumpname, expected_bytes);
                        return false;
                }
        }
        else
        {
                size_t existing_bytes = (size_t)loadmodel->numdeluxsamples * 3;

                if (existing_bytes != expected_bytes)
                {
                        Con_DPrintf2 ("BSPX lump %s has unexpected size %zu for existing lightdata\n",
                                lumpname, payload_length);
                        Mod_BspxDebugf ("BSPX:     %s expected %zu bytes but have %zu\n",
                                lumpname, expected_bytes, existing_bytes);
                        return false;
                }
        }

        memcpy (loadmodel->lightdata, payload, expected_bytes);
        loadmodel->litfile = true;
        Mod_BspxDebugf ("BSPX:     %s imported %zu RGB samples%s\n",
                lumpname, samplecount, had_header ? " (QLIT)" : "");
        return true;
}

static qboolean Mod_BspxImportE5Bgr9Lighting (const char *lumpname, const byte *data, size_t length)
{
        const byte *payload = data;
        size_t payload_length = length;
        size_t samplecount;
        size_t expected_bytes;
        qboolean had_header = false;

        if (!data || length == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s has no payload\n", lumpname);
                return false;
        }

        if (length >= 8 && data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
        {
            int version = LittleLong (((const int *)data)[1]);

            had_header = true;
            payload = data + 8;
            payload_length = length - 8;

            if (version != 1)
            {
                    Con_DPrintf2 ("BSPX lump %s has unsupported QLIT version %d\n", lumpname, version);
                    Mod_BspxDebugf ("BSPX:     %s has unsupported QLIT version %d\n", lumpname, version);
                    return false;
            }
        }

        if (payload_length % sizeof(uint32_t))
        {
                Con_DPrintf2 ("BSPX lump %s has unexpected size %zu (expected multiples of 4 bytes)\n",
                        lumpname, payload_length);
                Mod_BspxDebugf ("BSPX:     %s size %zu is not a multiple of 4 bytes\n", lumpname, payload_length);
                return false;
        }

        samplecount = payload_length / sizeof(uint32_t);
        if (samplecount == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s contains zero E5BGR9 samples\n", lumpname);
                return false;
        }

        if (!Mod_BspxEnsureDeluxStorage (samplecount, lumpname))
        {
                Mod_BspxDebugf ("BSPX:     %s rejected due to deluxemap storage mismatch\n", lumpname);
                return false;
        }

        expected_bytes = samplecount * 3;

        if (!loadmodel->lightdata)
        {
                loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill (expected_bytes, loadname);
                if (!loadmodel->lightdata)
                {
                        Mod_BspxDebugf ("BSPX:     %s failed to allocate %zu bytes for lightdata\n", lumpname, expected_bytes);
                        return false;
                }
        }
        else
        {
                size_t existing_bytes = (size_t)loadmodel->numdeluxsamples * 3;

                if (existing_bytes != expected_bytes)
                {
                        Con_DPrintf2 ("BSPX lump %s has unexpected size %zu for existing lightdata\n",
                                lumpname, payload_length);
                        Mod_BspxDebugf ("BSPX:     %s expected %zu bytes but have %zu\n",
                                lumpname, expected_bytes, existing_bytes);
                        return false;
                }
        }

        if (loadmodel->litfile)
        {
                Mod_BspxDebugf ("BSPX:     %s ignored (existing light data)\n", lumpname);
                return false;
        }

        {
                const uint32_t *src = (const uint32_t *)payload;
                byte *dst = loadmodel->lightdata;

                for (size_t i = 0; i < samplecount; ++i, dst += 3)
                        Mod_BspxDecodeE5Bgr9Sample (src[i], dst);
        }

        loadmodel->litfile = true;
        Mod_BspxDebugf ("BSPX:     %s imported %zu RGB samples%s (E5BGR9)\n",
                lumpname, samplecount, had_header ? " (QLIT)" : "");
        return true;
}

static qboolean Mod_BspxImportStaticLights (const char *lumpname, const byte *data, size_t length,
        bspx_static_light_t **out_lights, int *out_count)
{
        const size_t stride_with_intensity = sizeof(float) * 8;
        const size_t stride_without_intensity = sizeof(float) * 7;
        size_t stride = 0;
        qboolean has_intensity = true;
        int count;
        bspx_static_light_t *lights;

        if (!out_lights || !out_count)
        {
                Mod_BspxDebugf ("BSPX:     %s skipped (invalid output pointers)\n", lumpname);
                return false;
        }

        *out_lights = NULL;
        *out_count = 0;

        if (!data || length == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s contains no static lights\n", lumpname);
                return true;
        }

        if (length % stride_with_intensity == 0)
        {
                stride = stride_with_intensity;
                has_intensity = true;
        }
        else if (length % stride_without_intensity == 0)
        {
                stride = stride_without_intensity;
                has_intensity = false;
        }
        else
        {
                Con_DPrintf2 ("BSPX lump %s has unexpected size %zu (expected multiples of %zu or %zu)\n",
                        lumpname, length, stride_with_intensity, stride_without_intensity);
                Mod_BspxDebugf ("BSPX:     %s rejected size %zu for static lights\n", lumpname, length);
                return false;
        }

        count = (int)(length / stride);
        if (count <= 0)
        {
                Mod_BspxDebugf ("BSPX:     %s contains zero static lights\n", lumpname);
                return true;
        }

        lights = (bspx_static_light_t *) Hunk_AllocName (count * sizeof(*lights), lumpname);
        if (!lights)
        {
                Mod_BspxDebugf ("BSPX:     %s failed to allocate %d static lights\n", lumpname, count);
                return false;
        }

        for (int i = 0; i < count; ++i)
        {
                const float *src = (const float *)(data + i * stride);
                float intensity = 1.0f;

                lights[i].origin[0] = LittleFloat (src[0]);
                lights[i].origin[1] = LittleFloat (src[1]);
                lights[i].origin[2] = LittleFloat (src[2]);

                lights[i].radius = LittleFloat (src[3]);
                if (lights[i].radius < 0.0f)
                        lights[i].radius = 0.0f;

                lights[i].color[0] = LittleFloat (src[4]);
                lights[i].color[1] = LittleFloat (src[5]);
                lights[i].color[2] = LittleFloat (src[6]);

                if (has_intensity)
                        intensity = LittleFloat (src[7]);

                lights[i].intensity = (intensity > 0.0f) ? intensity : 0.0f;
        }

        *out_lights = lights;
        *out_count = count;
        Mod_BspxDebugf ("BSPX:     %s imported %d static lights%s\n",
                lumpname, count, has_intensity ? " (with intensity)" : "");
        return true;
}

static qboolean Mod_BspxImportLmOffsets (const char *lumpname, const byte *data, size_t length)
{
	int count;
	int *offsets;

	loadmodel->bspx_light_offset_count = 0;
	loadmodel->bspx_light_offsets = NULL;

	if (!data || length == 0)
	{
		Mod_BspxDebugf ("BSPX:     %s contains no light offsets\n", lumpname);
		return true;
	}

	if (length % sizeof(int))
	{
		Con_DPrintf2 ("BSPX lump %s has unexpected size %zu (expected multiple of %zu)\n",
				lumpname, length, sizeof(int));
		Mod_BspxDebugf ("BSPX:     %s rejected size %zu for light offsets\n", lumpname, length);
		return false;
	}

	count = (int)(length / sizeof(int));
	if (count <= 0)
	{
		Mod_BspxDebugf ("BSPX:     %s contains zero light offsets\n", lumpname);
		return true;
	}

	offsets = (int *) Hunk_AllocName (count * sizeof(*offsets), lumpname);
	if (!offsets)
	{
		Mod_BspxDebugf ("BSPX:     %s failed to allocate %d light offsets\n", lumpname, count);
		return false;
	}

	for (int i = 0; i < count; ++i)
		offsets[i] = LittleLong (((const int *)data)[i]);

	loadmodel->bspx_light_offsets = offsets;
	loadmodel->bspx_light_offset_count = count;
	Mod_BspxDebugf ("BSPX:     %s imported %d light offsets\n", lumpname, count);
	return true;
}

static qboolean Mod_BspxImportStaticShadowIndices (const char *lumpname, const byte *data, size_t length,
        int **out_indices, int *out_count)
{
        int count;
        int *indices;

        if (!out_indices || !out_count)
        {
                Mod_BspxDebugf ("BSPX:     %s skipped (invalid shadow index outputs)\n", lumpname);
                return false;
        }

        *out_indices = NULL;
        *out_count = 0;

        if (!data || length == 0)
        {
                Mod_BspxDebugf ("BSPX:     %s contains no shadow indices\n", lumpname);
                return true;
        }

        if (length % sizeof(int))
        {
                Con_DPrintf2 ("BSPX lump %s has unexpected size %zu (expected multiple of %zu)\n",
                        lumpname, length, sizeof(int));
                Mod_BspxDebugf ("BSPX:     %s rejected size %zu for shadow indices\n", lumpname, length);
                return false;
        }

        count = (int)(length / sizeof(int));
        if (count <= 0)
        {
                Mod_BspxDebugf ("BSPX:     %s contains zero shadow indices\n", lumpname);
                return true;
        }

        indices = (int *) Hunk_AllocName (count * sizeof(*indices), lumpname);
        if (!indices)
        {
                Mod_BspxDebugf ("BSPX:     %s failed to allocate %d shadow indices\n", lumpname, count);
                return false;
        }

        for (int i = 0; i < count; ++i)
                indices[i] = LittleLong (((const int *)data)[i]);

        *out_indices = indices;
        *out_count = count;
        Mod_BspxDebugf ("BSPX:     %s imported %d shadow indices\n", lumpname, count);
        return true;
}

static qboolean Mod_BspxStoreLightgridBlob (const char *lumpname, const byte *data, size_t length,
        const char *label, const byte **out_data, size_t *out_length)
{
        if (!out_data || !out_length)
        {
                Mod_BspxDebugf ("BSPX:     %s skipped (invalid lightgrid storage)\n", lumpname);
                return false;
        }

        if (!data || length == 0)
        {
                *out_data = NULL;
                *out_length = 0;
                Mod_BspxDebugf ("BSPX:     %s contains no %s data\n", lumpname, label);
                return true;
        }

        if (*out_data)
        {
                Mod_BspxDebugf ("BSPX:     %s ignored (existing %s data)\n", lumpname, label);
                return false;
        }

        *out_data = data;
        *out_length = length;
        Mod_BspxDebugf ("BSPX:     %s stored %zu bytes of %s data\n", lumpname, length, label);
        return true;
}

static void Mod_LoadBspx (const byte *buffer)
{
        typedef struct bspx_header_s
        {
                char id[4];
                int numlumps;
        } bspx_header_t;

        typedef struct bspx_lump_s
        {
                char name[24];
                int fileofs;
                int filelen;
        } bspx_lump_t;

        const size_t header_size = sizeof(bspx_header_t);
        const size_t entry_size = sizeof(bspx_lump_t);
        const size_t filesize = mod_bspx_filesize;
        const byte *header_ptr = NULL;
        const bspx_header_t *raw_header = NULL;
        const bspx_lump_t *directory = NULL;
        size_t header_offset = 0;
        size_t scan_window;
        int numlumps = 0;
        int stored = 0;

        loadmodel->bspx_num_entries = 0;
        loadmodel->bspx_entries = NULL;

        if (!buffer || filesize < header_size)
                return;

        /*
        ** Some maps place the BSPX header far from the end of the file (e.g. companion
        ** lighting data appended after large BSP lumps). Searching only the last few
        ** kilobytes misses these headers and prevents the BSPX lumps from loading. Scan
        ** the entire file instead while still walking backwards from the end so we find
        ** the last occurrence of the magic string.
        */
        scan_window = filesize;
        for (size_t offset = 0; offset + header_size <= scan_window; ++offset)
        {
                const byte *candidate = buffer + filesize - header_size - offset;

                if (candidate < buffer)
                        break;
                if (memcmp (candidate, "BSPX", 4))
                        continue;

                raw_header = (const bspx_header_t *)candidate;
                numlumps = LittleLong (raw_header->numlumps);

                if (numlumps <= 0)
                        continue;

                header_offset = (size_t)(candidate - buffer);

                if (header_offset > filesize - header_size)
                        continue;

                if ((size_t)numlumps > (filesize - header_offset - header_size) / entry_size)
                        continue;

                directory = (const bspx_lump_t *)(candidate + header_size);
                header_ptr = candidate;
                break;
        }

        if (!header_ptr)
                return;

        Mod_BspxDebugf ("BSPX: directory has %d lumps (%zu bytes)\n", numlumps, (size_t)numlumps * entry_size);

        loadmodel->bspx_entries = (bspx_entry_t *) Hunk_AllocName (numlumps * sizeof(*loadmodel->bspx_entries), "BSPXLUMPS");
        if (!loadmodel->bspx_entries)
                return;

        for (int i = 0; i < numlumps; ++i)
        {
                const bspx_lump_t *entry = directory + i;
                char lumpname[25];
                size_t lumpofs, lumplen;
                int raw_ofs = LittleLong (entry->fileofs);
                int raw_len = LittleLong (entry->filelen);
                qboolean recognized = false;

                memcpy (lumpname, entry->name, sizeof(entry->name));
                lumpname[sizeof(entry->name)] = '\0';
                for (int j = (int)sizeof(entry->name) - 1; j >= 0 && (lumpname[j] == '\0' || lumpname[j] == ' '); --j)
                        lumpname[j] = '\0';

                Mod_BspxDebugf ("BSPX:   lump %-16s ofs %d len %d\n", lumpname, raw_ofs, raw_len);

                if (raw_ofs < 0 || raw_len < 0)
                {
                        Mod_BspxDebugf ("BSPX:     %s skipped (negative offset/length)\n", lumpname);
                        continue;
                }

                lumpofs = (size_t)raw_ofs;
                lumplen = (size_t)raw_len;

                if (lumplen > 64 * 1024 * 1024)
                {
                        Mod_BspxDebugf ("BSPX:     %s skipped (length %zu exceeds limit)\n", lumpname, lumplen);
                        continue;
                }

                if (lumpofs > filesize || lumplen > filesize - lumpofs)
                {
                        size_t rel_ofs = header_offset + lumpofs;

                        if (rel_ofs > filesize || lumplen > filesize - rel_ofs)
                        {
                                Mod_BspxDebugf ("BSPX:     %s skipped (offset/length outside file)\n", lumpname);
                                continue;
                        }

                        Mod_BspxDebugf ("BSPX:     %s interpreting offset relative to BSPX header (%zu -> %zu)\n",
                                lumpname, lumpofs, rel_ofs);
                        lumpofs = rel_ofs;
                }

                q_strlcpy (loadmodel->bspx_entries[stored].name, lumpname, sizeof(loadmodel->bspx_entries[stored].name));
                loadmodel->bspx_entries[stored].data = buffer + lumpofs;
                loadmodel->bspx_entries[stored].length = lumplen;

                if (!q_strcasecmp (lumpname, "RGBLIGHTING") ||
                        !q_strcasecmp (lumpname, "RGBLIGHTDATA") ||
                        !q_strcasecmp (lumpname, "LIGHTINGRGB"))
                {
                        recognized = true;
                        if (loadmodel->litfile)
                                Mod_BspxDebugf ("BSPX:     %s ignored (existing light data)\n", lumpname);
                        else
                                Mod_BspxImportRgbLighting (lumpname, loadmodel->bspx_entries[stored].data, lumplen);
                }
                else if (!q_strcasecmp (lumpname, "LIGHTING_E5BGR9"))
                {
                        recognized = true;
                        Mod_BspxImportE5Bgr9Lighting (lumpname, loadmodel->bspx_entries[stored].data, lumplen);
                }
                else if (!q_strcasecmp (lumpname, "LIGHTINGDIR") ||
                        !q_strcasecmp (lumpname, "DELUXEMAP") ||
                        !q_strcasecmp (lumpname, "DELUXDATA") ||
                        !q_strcasecmp (lumpname, "DELUXMAP") ||
                        !q_strcasecmp (lumpname, "LUX") ||
                        !q_strcasecmp (lumpname, "LUXDATA") ||
                        !q_strcasecmp (lumpname, "LUXMAP") ||
                        !q_strcasecmp (lumpname, "DLIT"))
                {
                        recognized = true;
                        if (loadmodel->deluxfile)
                                Mod_BspxDebugf ("BSPX:     %s ignored (existing deluxemap)\n", lumpname);
                        else
                                Mod_BspxImportDeluxemap (lumpname, loadmodel->bspx_entries[stored].data, lumplen);
                }
                else if (!q_strcasecmp (lumpname, "STATICLIGHTS") || !q_strcasecmp (lumpname, "STATIC_LIGHTS"))
                {
                        recognized = true;
                        Mod_BspxImportStaticLights (lumpname, loadmodel->bspx_entries[stored].data, lumplen,
                                &loadmodel->bspx_static_lights, &loadmodel->bspx_num_static_lights);
                }
                else if (!q_strcasecmp (lumpname, "STATICSHADOWS") || !q_strcasecmp (lumpname, "STATIC_SHADOWS"))
                {
                        recognized = true;
                        if (!Mod_BspxImportStaticLights (lumpname, loadmodel->bspx_entries[stored].data, lumplen,
                                        &loadmodel->bspx_static_shadow_lights, &loadmodel->bspx_num_static_shadow_lights))
                        {
                                Mod_BspxImportStaticShadowIndices (lumpname, loadmodel->bspx_entries[stored].data, lumplen,
                                        &loadmodel->bspx_static_shadow_indices, &loadmodel->bspx_num_static_shadow_indices);
                        }
                }
                else if (!q_strcasecmp (lumpname, "LMOFFSET"))
                {
                        recognized = true;
                        Mod_BspxImportLmOffsets (lumpname, loadmodel->bspx_entries[stored].data, lumplen);
                }
                else if (!q_strcasecmp (lumpname, "LIGHTGRID_OCTREE"))
                {
                        recognized = true;
                        Mod_BspxStoreLightgridBlob (lumpname, loadmodel->bspx_entries[stored].data, lumplen,
                                "light grid octree", &loadmodel->bspx_lightgrid_octree, &loadmodel->bspx_lightgrid_octree_length);
                }
                else if (!q_strcasecmp (lumpname, "LIGHTGRIDS"))
                {
                        recognized = true;
                        Mod_BspxStoreLightgridBlob (lumpname, loadmodel->bspx_entries[stored].data, lumplen,
                                "light grids", &loadmodel->bspx_lightgrids, &loadmodel->bspx_lightgrids_length);
                }

                if (!recognized)
                        Mod_BspxDebugf ("BSPX:     %s ignored (unhandled lump)\n", lumpname);

                ++stored;
        }

        loadmodel->bspx_num_entries = stored;
        Mod_BspxDebugf ("BSPX: summary -- lightdata %s, deluxemap %s, offsets %d, static lights %d, static shadow lights %d, indices %d, lightgrid %s, lightgrids %s, map entries %d\n",
                loadmodel->litfile ? "loaded" : "missing",
                loadmodel->deluxfile ? "loaded" : "missing",
                loadmodel->bspx_light_offset_count,
                loadmodel->bspx_num_static_lights,
                loadmodel->bspx_num_static_shadow_lights,
                loadmodel->bspx_num_static_shadow_indices,
                loadmodel->bspx_lightgrid_octree ? "loaded" : "missing",
                loadmodel->bspx_lightgrids ? "loaded" : "missing",
                loadmodel->bspx_num_entries);


}
static void Mod_LoadLighting (lump_t *l)
{
        int i, mark;
        byte *in, *out, *data;
        byte d, q64_b0, q64_b1;
        char litfilename[MAX_OSPATH];
        char dlitfilename[MAX_OSPATH];
        char luxfilename[MAX_OSPATH];
        unsigned int path_id;
        int samplecount = 0;

        loadmodel->lightdata = NULL;
        loadmodel->deluxdata = NULL;
        loadmodel->litfile = false;
        loadmodel->deluxfile = false;

        q_strlcpy (litfilename, loadmodel->name, sizeof (litfilename));
        COM_StripExtension (litfilename, litfilename, sizeof (litfilename));
        q_strlcpy (dlitfilename, litfilename, sizeof (dlitfilename));
        q_strlcpy (luxfilename, litfilename, sizeof (luxfilename));
        q_strlcat (litfilename, ".lit", sizeof (litfilename));
        q_strlcat (dlitfilename, ".dlit", sizeof (dlitfilename));
        q_strlcat (luxfilename, ".lux", sizeof (luxfilename));

        mark = Hunk_LowMark ();
        data = (byte *) COM_LoadHunkFile (litfilename, &path_id);
        if (data)
        {
                if (path_id < loadmodel->path_id)
                {
                        Hunk_FreeToLowMark (mark);
                        Con_DPrintf ("ignored %s from a gamedir with lower priority\n", litfilename);
                }
                else if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
                {
                        i = LittleLong (((int *)data)[1]);
                        if (i == 1)
                        {
                                if (8 + l->filelen * 3 == com_filesize)
                                {
                                        Con_DPrintf2 ("%s loaded\n", litfilename);
                                        loadmodel->lightdata = data + 8;
                                        loadmodel->litfile = true;
                                        samplecount = l->filelen;
                                        Mod_LoadDeluxemap (dlitfilename, luxfilename, samplecount);
                                        return;
                                }
                                Hunk_FreeToLowMark (mark);
                                Con_Printf ("Outdated .lit file (%s should be %u bytes, not %" SDL_PRIs64 ")\n",
                                        litfilename, 8 + l->filelen * 3, com_filesize);
                        }
                        else
                        {
                                Hunk_FreeToLowMark (mark);
                                Con_Printf ("Unknown .lit file version (%d)\n", i);
                        }
                }
                else
                {
                        Hunk_FreeToLowMark (mark);
                        Con_Printf ("Corrupt .lit file (old version?), ignoring\n");
                }
        }

        if (!l->filelen)
        {
                Mod_LoadDeluxemap (dlitfilename, luxfilename, 0);
                return;
        }

        if (loadmodel->bspversion == BSPVERSION_QUAKE64)
        {
                loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill ((l->filelen / 2) * 3, litfilename);
                in = mod_base + l->fileofs;
                out = loadmodel->lightdata;

                for (i = 0; i < (l->filelen / 2); i++)
                {
                        q64_b0 = *in++;
                        q64_b1 = *in++;

                        *out++ = q64_b0 & 0xf8;
                        *out++ = ((q64_b0 & 0x07) << 5) + ((q64_b1 & 0xc0) >> 5);
                        *out++ = (q64_b1 & 0x3f) << 2;
                }

                samplecount = l->filelen / 2;
                Mod_LoadDeluxemap (dlitfilename, luxfilename, samplecount);
                return;
        }

#ifdef BSP29_VALVE
        if (loadmodel->bspversion == BSPVERSION_VALVE)
        {
                loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill (l->filelen, litfilename);
                memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
                samplecount = l->filelen / 3;
                Mod_LoadDeluxemap (dlitfilename, luxfilename, samplecount);
                return;
        }
#endif

        loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill (l->filelen * 3, litfilename);
        in = loadmodel->lightdata + l->filelen * 2;
        out = loadmodel->lightdata;
        memcpy (in, mod_base + l->fileofs, l->filelen);
        for (i = 0; i < l->filelen; i++)
        {
                d = *in++;
                *out++ = d;
                *out++ = d;
                *out++ = d;
        }

        samplecount = l->filelen;
        Mod_LoadDeluxemap (dlitfilename, luxfilename, samplecount);
}


static void Mod_InitFallbackDeluxemap (msurface_t *surf)
{
        int style_count, i, count;
        int smax, tmax, facesize;
        vec3_t normal;
        byte encoded[3];
        byte *dst;

        if (!surf->deluxsamples)
                return;

        style_count = 0;
        while (style_count < MAXLIGHTMAPS && surf->styles[style_count] != 255)
                style_count++;
        if (style_count == 0)
                style_count = 1;

        smax = (surf->extents[0] >> 4) + 1;
        tmax = (surf->extents[1] >> 4) + 1;
        facesize = smax * tmax;

        VectorCopy (surf->plane->normal, normal);
        if (surf->flags & SURF_PLANEBACK)
                VectorNegate (normal, normal);

        for (i = 0; i < 3; i++)
        {
                float c = CLAMP (-1.0f, normal[i], 1.0f);
                int v = (int) Q_rint ((c * 0.5f + 0.5f) * 255.0f);
                encoded[i] = (byte) CLAMP (0, v, 255);
        }

        for (i = 0; i < style_count; i++)
        {
                dst = surf->deluxsamples + i * facesize * 3;
                for (count = 0; count < facesize; count++, dst += 3)
                {
                        dst[0] = encoded[0];
                        dst[1] = encoded[1];
                        dst[2] = encoded[2];
                }
        }
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


/*
=================
Mod_LoadVertexes
=================
*/
static void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t	*in;
	mvertex_t	*out;
	int			i, count;

	in = (dvertex_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (mvertex_t *) Hunk_AllocNameNoFill ( count*sizeof(*out), loadname);

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
static void Mod_LoadEdges (lump_t *l, int bsp2)
{
	medge_t *out;
	int 	i, count;

	if (bsp2)
	{
		dledge_t *in = (dledge_t *)(mod_base + l->fileofs);

		if (l->filelen % sizeof(*in))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);

		count = l->filelen / sizeof(*in);
		out = (medge_t *) Hunk_AllocNameNoFill ( (count + 1) * sizeof(*out), loadname);

		loadmodel->edges = out;
		loadmodel->numedges = count;

		for (i=0 ; i<count ; i++, in++, out++)
		{
			out->v[0] = LittleLong(in->v[0]);
			out->v[1] = LittleLong(in->v[1]);
		}
	}
	else
	{
		dsedge_t *in = (dsedge_t *)(mod_base + l->fileofs);

		if (l->filelen % sizeof(*in))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);

		count = l->filelen / sizeof(*in);
		out = (medge_t *) Hunk_AllocNameNoFill ( (count + 1) * sizeof(*out), loadname);

		loadmodel->edges = out;
		loadmodel->numedges = count;

		for (i=0 ; i<count ; i++, in++, out++)
		{
			out->v[0] = (unsigned short)LittleShort(in->v[0]);
			out->v[1] = (unsigned short)LittleShort(in->v[1]);
		}
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

/*
=================
Mod_LoadFaces
=================
*/
static void Mod_LoadFaces (lump_t *l, qboolean bsp2)
{
	dsface_t	*ins;
	dlface_t	*inl;
	msurface_t 	*out;
	int			i, count, surfnum, lofs;
	int			planenum, side, texinfon;

	if (bsp2)
	{
		ins = NULL;
		inl = (dlface_t *)(mod_base + l->fileofs);
		if (l->filelen % sizeof(*inl))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
		count = l->filelen / sizeof(*inl);
	}
	else
	{
		ins = (dsface_t *)(mod_base + l->fileofs);
		inl = NULL;
		if (l->filelen % sizeof(*ins))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
		count = l->filelen / sizeof(*ins);
	}
	out = (msurface_t *)Hunk_AllocName ( count*sizeof(*out), loadname);

	//johnfitz -- warn mappers about exceeding old limits
	if (count > 32767 && !bsp2)
		Con_DWarning ("%i faces exceeds standard limit of 32767.\n", count);
	//johnfitz

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	if (loadmodel->bspx_light_offsets && loadmodel->bspx_light_offset_count != count)
	{
		Con_DPrintf2 ("BSPX LMOFFSET count mismatch (%d vs %d)\n",
			loadmodel->bspx_light_offset_count, count);
		if (loadmodel->bspx_light_offset_count > count)
			loadmodel->bspx_light_offset_count = count;
	}

	for (surfnum=0 ; surfnum<count ; surfnum++, out++)
	{
		texture_t *texture;
		if (bsp2)
		{
			out->firstedge = LittleLong(inl->firstedge);
			out->numedges = LittleLong(inl->numedges);
			planenum = LittleLong(inl->planenum);
			side = LittleLong(inl->side);
			texinfon = LittleLong (inl->texinfo);
			for (i=0 ; i<MAXLIGHTMAPS ; i++)
				out->styles[i] = inl->styles[i];
			lofs = LittleLong(inl->lightofs);
			inl++;
		}
		else
		{
			out->firstedge = LittleLong(ins->firstedge);
			out->numedges = LittleShort(ins->numedges);
			planenum = LittleShort(ins->planenum);
			side = LittleShort(ins->side);
			texinfon = LittleShort (ins->texinfo);
			for (i=0 ; i<MAXLIGHTMAPS ; i++)
				out->styles[i] = ins->styles[i];
			lofs = LittleLong(ins->lightofs);
			ins++;
		}

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

                if (loadmodel->bspx_light_offsets && surfnum < loadmodel->bspx_light_offset_count)
                        lofs = loadmodel->bspx_light_offsets[surfnum];

                if (lofs == -1)
                {
                        out->samples = NULL;
                        out->deluxsamples = NULL;
                }
                else
                {
                        size_t sampleofs;

#ifdef BSP29_VALVE
                        sampleofs = (loadmodel->bspversion == BSPVERSION_VALVE) ? (size_t)lofs : (size_t)lofs * 3;
#else
                        sampleofs = (size_t)lofs * 3;
#endif

                        out->samples = loadmodel->lightdata + sampleofs; //johnfitz -- lit support via lordhavoc (was "+ i")
                        if (loadmodel->deluxdata)
                        {
                                out->deluxsamples = loadmodel->deluxdata + sampleofs;
                                if (!loadmodel->deluxfile)
                                        Mod_InitFallbackDeluxemap (out);
                        }
                        else
                        {
                                out->deluxsamples = NULL;
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
			else if (out->samples && !loadmodel->haslitwater)
			{
				Con_DPrintf ("Map has lit water\n");
				loadmodel->haslitwater = true;
			}

			if (texture->type == TEXTYPE_LAVA)
				out->flags |= SURF_DRAWLAVA;
			else if (texture->type == TEXTYPE_SLIME)
				out->flags |= SURF_DRAWSLIME;
			else if (texture->type == TEXTYPE_TELE)
				out->flags |= SURF_DRAWTELE;
			else
				out->flags |= SURF_DRAWWATER;
		}
		else if (texture->type == TEXTYPE_CUTOUT)
		{
			out->flags |= SURF_DRAWFENCE;
		}
		else if (out->texinfo->flags & TEX_MISSING)
		{
			if (out->samples) //lightmapped
			{
				out->flags |= SURF_NOTEXTURE;
			}
			else // not lightmapped
			{
				out->flags |= (SURF_NOTEXTURE | SURF_DRAWTILED);
			}
		}
		//johnfitz
	}
}


/*
=================
Mod_SetParent
=================
*/
static void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents < 0)
		return;
	Mod_SetParent (node->children[0], node);
	Mod_SetParent (node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
static void Mod_LoadNodes_S (lump_t *l)
{
	int			i, j, count, p;
	dsnode_t	*in;
	mnode_t		*out;

	in = (dsnode_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = (mnode_t *) Hunk_AllocName ( count*sizeof(*out), loadname);

	//johnfitz -- warn mappers about exceeding old limits
	if (count > 32767)
		Con_DWarning ("%i nodes exceeds standard limit of 32767.\n", count);
	//johnfitz

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = (unsigned short)LittleShort (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = (unsigned short)LittleShort (in->numfaces); //johnfitz -- explicit cast as unsigned short

		for (j=0 ; j<2 ; j++)
		{
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = (unsigned short)LittleShort(in->children[j]);
			if (p < count)
				out->children[j] = loadmodel->nodes + p;
			else
			{
				p = 65535 - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else
				{
					Con_Printf("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			}
			//johnfitz
		}
	}
}

static void Mod_LoadNodes_L1 (lump_t *l)
{
	int			i, j, count, p;
	dl1node_t	*in;
	mnode_t		*out;

	in = (dl1node_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("Mod_LoadNodes: funny lump size in %s",loadmodel->name);

	count = l->filelen / sizeof(*in);
	out = (mnode_t *)Hunk_AllocName ( count*sizeof(*out), loadname);

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = LittleLong (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = LittleLong (in->numfaces); //johnfitz -- explicit cast as unsigned short

		for (j=0 ; j<2 ; j++)
		{
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = LittleLong(in->children[j]);
			if (p >= 0 && p < count)
				out->children[j] = loadmodel->nodes + p;
			else
			{
				p = 0xffffffff - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p >= 0 && p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else
				{
					Con_Printf("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			}
			//johnfitz
		}
	}
}

static void Mod_LoadNodes_L2 (lump_t *l)
{
	int			i, j, count, p;
	dl2node_t	*in;
	mnode_t		*out;

	in = (dl2node_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("Mod_LoadNodes: funny lump size in %s",loadmodel->name);

	count = l->filelen / sizeof(*in);
	out = (mnode_t *)Hunk_AllocName ( count*sizeof(*out), loadname);

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleFloat (in->mins[j]);
			out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
		}

		p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = LittleLong (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = LittleLong (in->numfaces); //johnfitz -- explicit cast as unsigned short

		for (j=0 ; j<2 ; j++)
		{
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = LittleLong(in->children[j]);
			if (p > 0 && p < count)
				out->children[j] = loadmodel->nodes + p;
			else
			{
				p = 0xffffffff - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p >= 0 && p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else
				{
					Con_Printf("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			}
			//johnfitz
		}
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

	inuse = (uint32_t *) calloc (BITARRAY_DWORDS (mod->numtextures), sizeof (uint32_t));
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

	free (inuse);

	//Con_Printf("%s: %d/%d textures\n", mod->name, count, mod->numtextures);
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
			//Spike: FIXME: bounds check
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
	if (fread(&filelen, 4, 1, f) != 1)
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
	dheader_t	 header;
	dmodel_t 	*bm;
	float		radius; //johnfitz

	loadmodel->type = mod_brush;
	loadmodel->numdeluxsamples = 0;
	loadmodel->bspx_light_offset_count = 0;
	loadmodel->bspx_light_offsets = NULL;
	loadmodel->bspx_num_static_lights = 0;
	loadmodel->bspx_static_lights = NULL;
        loadmodel->bspx_num_static_shadow_lights = 0;
        loadmodel->bspx_static_shadow_lights = NULL;
        loadmodel->bspx_num_static_shadow_indices = 0;
        loadmodel->bspx_static_shadow_indices = NULL;
        loadmodel->bspx_num_entries = 0;
        loadmodel->bspx_entries = NULL;
        loadmodel->bspx_lightgrid_octree = NULL;
        loadmodel->bspx_lightgrid_octree_length = 0;
        loadmodel->bspx_lightgrids = NULL;
        loadmodel->bspx_lightgrids_length = 0;
        mod_bspx_filesize = (com_filesize > 0) ? (size_t)com_filesize : 0;

	if (!mod_bspx_filesize)
		Sys_Error ("Mod_LoadBrushModel: %s has unknown file size", mod->name);

	bsp2 = Mod_ParseBspHeader ((const byte *)buffer, mod_bspx_filesize, &header);
	mod->bspversion = header.version;

// swap all the lumps
	mod_base = (byte *)buffer;

	if (debug_bspx.value > 0.0f)
	{
		Mod_BspxDebugf ("BSPX: loading %s (version %d, %zu bytes)\n",
			loadmodel->name, mod->bspversion, mod_bspx_filesize);
		Mod_BspxDebugf ("BSPX:   lighting lump ofs %d len %d\n",
			header.lumps[LUMP_LIGHTING].fileofs, header.lumps[LUMP_LIGHTING].filelen);
	}

// load into heap

        Mod_LoadVertexes (&header.lumps[LUMP_VERTEXES]);
        Mod_LoadEdges (&header.lumps[LUMP_EDGES], bsp2);
        Mod_LoadSurfedges (&header.lumps[LUMP_SURFEDGES]);
        Mod_LoadTextures (&header.lumps[LUMP_TEXTURES]);
        Mod_LoadLighting (&header.lumps[LUMP_LIGHTING]);
        Mod_LoadBspx ((const byte *)buffer);
        Mod_LoadExternalBspx ();
        Mod_LoadPlanes (&header.lumps[LUMP_PLANES]);
        Mod_LoadTexinfo (&header.lumps[LUMP_TEXINFO]);
        Mod_LoadFaces (&header.lumps[LUMP_FACES], bsp2);
        Mod_LoadMarksurfaces (&header.lumps[LUMP_MARKSURFACES], bsp2);


	if (mod->bspversion == BSPVERSION && external_vis.value && sv.modelname[0] && !q_strcasecmp(loadname, sv.name))
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
	Mod_LoadEntities (&header.lumps[LUMP_ENTITIES]);
	Mod_LoadSubmodels (&header.lumps[LUMP_MODELS]);

	Mod_MakeHull0 ();

	mod->numframes = 2;		// regular and alternate animation

	Mod_CheckWaterVis ();

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

			sprintf (name, "*%i", i+1);
			loadmodel = Mod_FindName (name);
			*loadmodel = *mod;
			strcpy (loadmodel->name, name);
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


static void Mod_CalcAliasBounds (aliashdr_t *a);

static void Mod_LoadMD2Model (qmodel_t *mod, void *buffer)
{
	typedef struct
	{
		int				ident;
		int				version;
		int				skinwidth;
		int				skinheight;
		int				framesize;
		int				num_skins;
		int				num_vertices;
		int				num_texcoords;
		int				num_tris;
		int				num_glcmds;
		int				num_frames;
		int				ofs_skins;
		int				ofs_texcoords;
		int				ofs_tris;
		int				ofs_frames;
		int				ofs_glcmds;
		int				ofs_end;
	} md2_header_t;
	typedef struct
	{
		short		s;
		short		t;
	} md2_texcoord_t;
	typedef struct
	{
		unsigned short	vertindex[3];
		unsigned short	stindex[3];
	} md2_triangle_t;
	typedef struct
	{
		byte		v[3];
		byte		lightnormalindex;
	} md2_vertex_t;
	typedef struct
	{
		float		scale[3];
		float		translate[3];
		char		name[16];
		md2_vertex_t	verts[1];
	} md2_frame_t;

	md2_header_t	*md2;
	int			version;
	int			skinwidth, skinheight;
	int			framesize;
	int			numskins_raw;
	int			numskins, numverts, numst, numtris, numframes;
	int			ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_end;
	size_t		filesize;
	int			start;
	size_t		size;
	int			i, j, k;
	const md2_texcoord_t	*md2_tc;
	const md2_triangle_t	*md2_tris;
	stvert_t		*st_storage;
	dtriangle_t		*tri_storage;
	unsigned short	*vertex_remap;
	unsigned int	*pair_keys;
	size_t		maxverts, newnumverts;
	trivertx_t	*converted;
	float			mins[3], maxs[3];
	double			radius2;
	const char	*skinbase;
	char			modeldir[MAX_QPATH];

	md2 = (md2_header_t *) buffer;
	version = LittleLong (md2->version);
	if (version != 8)
		Sys_Error ("%s has wrong version number (%i should be %i)", mod->name, version, 8);

	skinwidth = LittleLong (md2->skinwidth);
	skinheight = LittleLong (md2->skinheight);
	framesize = LittleLong (md2->framesize);
	numskins_raw = LittleLong (md2->num_skins);
	numverts = LittleLong (md2->num_vertices);
	numst = LittleLong (md2->num_texcoords);
	numtris = LittleLong (md2->num_tris);
	numframes = LittleLong (md2->num_frames);
	ofs_skins = LittleLong (md2->ofs_skins);
	ofs_st = LittleLong (md2->ofs_texcoords);
	ofs_tris = LittleLong (md2->ofs_tris);
	ofs_frames = LittleLong (md2->ofs_frames);
	ofs_end = LittleLong (md2->ofs_end);

	if (numframes < 1)
		Sys_Error ("Mod_LoadMD2Model: %s has no frames", mod->name);
	if (numframes > MAXALIASFRAMES)
		Sys_Error ("Mod_LoadMD2Model: %s has too many frames (%d > %d)", mod->name, numframes, MAXALIASFRAMES);
	if (numverts < 1)
		Sys_Error ("Mod_LoadMD2Model: %s has no vertices", mod->name);
	if (numst < 1)
		Sys_Error ("Mod_LoadMD2Model: %s has no texcoords", mod->name);
	if (numtris < 1)
		Sys_Error ("Mod_LoadMD2Model: %s has no triangles", mod->name);
	if (framesize <= 0)
		Sys_Error ("Mod_LoadMD2Model: %s has invalid frame size", mod->name);

	if (ofs_skins < 0 || ofs_st < 0 || ofs_tris < 0 || ofs_frames < 0 || ofs_end < 0)
		Sys_Error ("Mod_LoadMD2Model: %s has corrupt offsets", mod->name);

	filesize = (com_filesize > 0) ? (size_t)com_filesize : 0;
	if (filesize)
	{
		if ((size_t)ofs_end > filesize)
			Sys_Error ("Mod_LoadMD2Model: %s has invalid end offset", mod->name);
		if (numskins_raw > 0 && ((size_t)ofs_skins + (size_t)numskins_raw * 64 > filesize))
			Sys_Error ("Mod_LoadMD2Model: %s skins exceed file size", mod->name);
		if ((size_t)ofs_st + (size_t)numst * sizeof (md2_texcoord_t) > filesize)
			Sys_Error ("Mod_LoadMD2Model: %s texcoords exceed file size", mod->name);
		if ((size_t)ofs_tris + (size_t)numtris * sizeof (md2_triangle_t) > filesize)
			Sys_Error ("Mod_LoadMD2Model: %s triangles exceed file size", mod->name);
		if ((size_t)ofs_frames + (size_t)framesize * (size_t)numframes > filesize)
			Sys_Error ("Mod_LoadMD2Model: %s frames exceed file size", mod->name);
	}

	start = Hunk_LowMark ();

	size = sizeof (aliashdr_t) + (numframes - 1) * sizeof (pheader->frames[0]);
	pheader = (aliashdr_t *) Hunk_AllocName (size, loadname);
	memset (pheader, 0, size);

	pheader->numframes = numframes;
	pheader->numposes = numframes;
	pheader->poseverttype = PV_QUAKE1;
	pheader->numtris = numtris;
	pheader->skinwidth = skinwidth > 0 ? skinwidth : 1;
	pheader->skinheight = skinheight > 0 ? skinheight : 1;
	VectorClear (pheader->eyeposition);

	mod->numframes = numframes;
	mod->synctype = ST_SYNC;
	mod->flags = 0;

	numskins = numskins_raw;
	if (numskins < 1)
		numskins = 1;
	if (numskins > MAX_SKINS)
		numskins = MAX_SKINS;
	pheader->numskins = numskins;

	skinbase = (numskins_raw > 0) ? (const char *)((byte *)buffer + ofs_skins) : NULL;

	q_strlcpy (modeldir, mod->name, sizeof (modeldir));
	{
		char *slash = strrchr (modeldir, '/');
		if (slash)
			slash[1] = '\0';
		else
			modeldir[0] = '\0';
	}

	for (i = 0; i < pheader->numskins; i++)
	{
		gltexture_t *tex = NULL;

		for (j = 0; j < 4; j++)
		{
			pheader->gltextures[i][j] = notexture;
			pheader->fbtextures[i][j] = NULL;
			pheader->emissivetextures[i][j] = NULL;
		}
		pheader->texels[i] = 0;

		if (skinbase && i < numskins_raw)
		{
			char rawname[65];
			char clean[MAX_QPATH];
			const char *attempts[3];
			char attemptbuf[3][MAX_QPATH];
			int numattempts = 0;

			q_strlcpy (rawname, skinbase + i * 64, sizeof (rawname));
			COM_StripExtension (rawname, clean, sizeof (clean));
			for (j = 0; clean[j]; j++)
				if (clean[j] == '\\')
					clean[j] = '/';

			if (clean[0])
			{
				q_strlcpy (attemptbuf[numattempts], clean, sizeof (attemptbuf[0]));
				attempts[numattempts] = attemptbuf[numattempts];
				numattempts++;

				if (modeldir[0] && !strchr (clean, '/') && numattempts < (int)countof (attempts))
				{
					q_snprintf (attemptbuf[numattempts], sizeof (attemptbuf[0]), "%s%s", modeldir, clean);
					attempts[numattempts] = attemptbuf[numattempts];
					numattempts++;
				}
				if (strncmp (clean, "progs/", 6) && numattempts < (int)countof (attempts))
				{
					q_snprintf (attemptbuf[numattempts], sizeof (attemptbuf[0]), "progs/%s", clean);
					attempts[numattempts] = attemptbuf[numattempts];
					numattempts++;
				}

				for (j = 0; j < numattempts && !tex; j++)
				{
					int mark = Hunk_LowMark ();
					int texwidth = 0, texheight = 0;
					enum srcformat fmt = SRC_RGBA;
					byte *data = Image_LoadImage (attempts[j], &texwidth, &texheight, &fmt);
					if (data)
						tex = TexMgr_LoadImage (mod, attempts[j], texwidth, texheight, fmt, data, attempts[j], 0, TEXPREF_MIPMAP);
					Hunk_FreeToLowMark (mark);
				}
			}
		}

		if (tex)
		{
			for (j = 0; j < 4; j++)
				pheader->gltextures[i][j] = tex;
		}
	}

	md2_tc = (const md2_texcoord_t *) ((byte *)buffer + ofs_st);
	md2_tris = (const md2_triangle_t *) ((byte *)buffer + ofs_tris);

	maxverts = (size_t)numtris * 3;
	st_storage = (stvert_t *) Z_Malloc (maxverts * sizeof (*st_storage));
	tri_storage = (dtriangle_t *) Z_Malloc ((size_t)numtris * sizeof (*tri_storage));
	vertex_remap = (unsigned short *) Z_Malloc (maxverts * sizeof (*vertex_remap));
	pair_keys = (unsigned int *) Z_Malloc (maxverts * sizeof (*pair_keys));
	newnumverts = 0;

	for (i = 0; i < numtris; i++)
	{
		tri_storage[i].facesfront = 1;
		for (j = 0; j < 3; j++)
		{
			unsigned short vindex = LittleShort (md2_tris[i].vertindex[j]);
			unsigned short stindex = LittleShort (md2_tris[i].stindex[j]);
			unsigned int key;

			if (vindex >= (unsigned)numverts || stindex >= (unsigned)numst)
				Sys_Error ("Mod_LoadMD2Model: %s has corrupt triangle indices", mod->name);

			key = ((unsigned int)vindex << 16) | stindex;
			for (k = 0; k < (int)newnumverts; k++)
				if (pair_keys[k] == key)
					break;

			if (k == (int)newnumverts)
			{
				if (newnumverts >= maxverts)
					Sys_Error ("Mod_LoadMD2Model: %s has too many unique vertices", mod->name);
				pair_keys[newnumverts] = key;
				vertex_remap[newnumverts] = vindex;
				st_storage[newnumverts].onseam = 0;
				st_storage[newnumverts].s = LittleShort (md2_tc[stindex].s);
				st_storage[newnumverts].t = LittleShort (md2_tc[stindex].t);
				newnumverts++;
			}

			tri_storage[i].vertindex[j] = k;
		}
	}

	if (newnumverts > MAXALIASVERTS)
		Sys_Error ("Mod_LoadMD2Model: %s has too many vertices (%zu > %d)", mod->name, newnumverts, MAXALIASVERTS);

	pheader->numverts = (int)newnumverts;

	converted = (trivertx_t *) Z_Malloc ((size_t)numframes * newnumverts * sizeof (*converted));

	for (k = 0; k < 3; k++)
	{
		mins[k] = FLT_MAX;
		maxs[k] = -FLT_MAX;
	}
	radius2 = 0;

	for (i = 0; i < numframes; i++)
	{
		md2_frame_t *frame = (md2_frame_t *) ((byte *)buffer + ofs_frames + (size_t)i * framesize);
		md2_vertex_t *verts = (md2_vertex_t *) (frame + 1);
		float fscale[3], ftrans[3];

		for (k = 0; k < 3; k++)
		{
			fscale[k] = LittleFloat (frame->scale[k]);
			ftrans[k] = LittleFloat (frame->translate[k]);
		}

		for (j = 0; j < numverts; j++)
		{
			float pos[3];
			double dist2;

			for (k = 0; k < 3; k++)
			{
				pos[k] = fscale[k] * verts[j].v[k] + ftrans[k];
				mins[k] = q_min (mins[k], pos[k]);
				maxs[k] = q_max (maxs[k], pos[k]);
			}

			dist2 = pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2];
			if (dist2 > radius2)
				radius2 = dist2;
		}
	}

	for (k = 0; k < 3; k++)
	{
		if (mins[k] == FLT_MAX)
			mins[k] = maxs[k] = 0;
		if (maxs[k] < mins[k])
			maxs[k] = mins[k];
		pheader->scale_origin[k] = mins[k];
		if (maxs[k] == mins[k])
			pheader->scale[k] = 1.0f;
		else
			pheader->scale[k] = (maxs[k] - mins[k]) / 255.0f;
	}

	{
		float dx = maxs[0] - mins[0];
		float dy = maxs[1] - mins[1];
		float dz = maxs[2] - mins[2];
		pheader->size = sqrtf (dx * dx + dy * dy + dz * dz);
	}
	pheader->boundingradius = (float) sqrt (radius2);

	posenum = 0;
	for (i = 0; i < numframes; i++)
	{
		md2_frame_t *frame = (md2_frame_t *) ((byte *)buffer + ofs_frames + (size_t)i * framesize);
		md2_vertex_t *verts = (md2_vertex_t *) (frame + 1);
		float fscale[3], ftrans[3];
		trivertx_t *outverts = converted + (size_t)i * newnumverts;
		byte frame_min[3] = {255, 255, 255};
		byte frame_max[3] = {0, 0, 0};

		for (k = 0; k < 3; k++)
		{
			fscale[k] = LittleFloat (frame->scale[k]);
			ftrans[k] = LittleFloat (frame->translate[k]);
		}

		for (j = 0; j < (int)newnumverts; j++)
		{
			md2_vertex_t *vin = &verts[vertex_remap[j]];

			for (k = 0; k < 3; k++)
			{
				float pos = fscale[k] * vin->v[k] + ftrans[k];
				float scale = pheader->scale[k];
				float origin = pheader->scale_origin[k];
				int value = 0;

				if (scale > 0)
				{
					value = (int) floorf (((pos - origin) / scale) + 0.5f);
					if (value < 0) value = 0;
					if (value > 255) value = 255;
				}

				outverts[j].v[k] = value;
				if (value < frame_min[k]) frame_min[k] = value;
				if (value > frame_max[k]) frame_max[k] = value;
			}

			outverts[j].lightnormalindex = vin->lightnormalindex;
		}

		poseverts[i] = outverts;

		pheader->frames[i].firstpose = i;
		pheader->frames[i].numposes = 1;
		pheader->frames[i].interval = 0.1f;
		pheader->frames[i].frame = i;
		q_strlcpy (pheader->frames[i].name, frame->name, sizeof (pheader->frames[i].name));
		for (k = 0; k < 3; k++)
		{
			pheader->frames[i].bboxmin.v[k] = frame_min[k];
			pheader->frames[i].bboxmax.v[k] = frame_max[k];
		}
		pheader->frames[i].bboxmin.lightnormalindex = 0;
		pheader->frames[i].bboxmax.lightnormalindex = 0;

		posenum++;
	}

	stverts = st_storage;
	triangles = tri_storage;

	mod->type = mod_alias;
	Mod_SetExtraFlags (mod);
	Mod_CalcAliasBounds (pheader);

	GL_MakeAliasModelDisplayLists (mod, pheader);

	Z_Free (converted);
	Z_Free (st_storage);
	Z_Free (tri_storage);
	Z_Free (vertex_remap);
	Z_Free (pair_keys);

	{
		int end;
		int total;

		end = Hunk_LowMark ();
		total = end - start;

		Cache_Alloc (&mod->cache, total, loadname);
		if (!mod->cache.data)
			return;
		memcpy (mod->cache.data, pheader, total);

		mod->sortkey = ((CRC_Block (mod->name, strlen (mod->name)) >> 1) & MODSORT_FRAMEMASK) << MODSORT_FRAMEBITS;
		if (mod->flags & MF_HOLEY)
			mod->sortkey |= MODSORT_ALIAS_ALPHATEST;
		else
			mod->sortkey &= ~MODSORT_ALIAS_ALPHATEST;

		Hunk_FreeToLowMark (start);
	}
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
	unsigned int		texflags = TEXPREF_PAD;

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
			pheader->emissivetextures[i][0] = NULL;
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
				pheader->emissivetextures[i][j&3] = NULL;
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
				pheader->emissivetextures[i][j&3] = NULL;
				}
				//johnfitz

				pskintype = (daliasskintype_t *)((byte *)(pskintype) + size);
			}
			k = j;
                        for (/**/; j < 4; j++)
                        {
                                pheader->gltextures[i][j&3] = pheader->gltextures[i][j - k];
                                pheader->fbtextures[i][j&3] = pheader->fbtextures[i][j - k];
                                pheader->emissivetextures[i][j&3] = pheader->emissivetextures[i][j - k];
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
	extern cvar_t r_nolerp_list, r_noshadow_list;

	if (!mod || mod->type != mod_alias)
		return;

	mod->flags &= (0xFF | MF_HOLEY); //only preserve first byte, plus MF_HOLEY

	// nolerp flag
	if (nameInList(r_nolerp_list.string, mod->name))
		mod->flags |= MOD_NOLERP;

	// noshadow flag
	if (nameInList(r_noshadow_list.string, mod->name))
		mod->flags |= MOD_NOSHADOW;

	// fullbright hack (TODO: make this a cvar list)
	if (!strcmp (mod->name, "progs/flame2.mdl") ||
		!strcmp (mod->name, "progs/flame.mdl") ||
		!strcmp (mod->name, "progs/boss.mdl"))
	{
		mod->flags |= MOD_FBRIGHTHACK;
	}
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

		if (COM_FileExists (path, &md5_path_id) && md5_path_id >= mod->path_id)
		{
			char *md5buffer = (char *) COM_LoadMallocFile (path, NULL);
			if (md5buffer)
			{
				Mod_LoadMD5MeshModel (mod, md5buffer);
				free (md5buffer);
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
static void *Mod_LoadSpriteFrame (void * pin, mspriteframe_t **ppframe, int framenum, int version)
{
	dspriteframe_t			*pinframe;
	mspriteframe_t			*pspriteframe;
	int						width, height, size, origin[2];
	enum srcformat		format;
	char				name[64];
	src_offset_t			offset; //johnfitz
	byte				*pixels;

	pinframe = (dspriteframe_t *)pin;

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height;
	pixels = (byte *)(pinframe + 1);

	if (version == SPRITE32_VERSION)
	{
		int datasize = size * 4;
		int i;

		for (i = 0; i < size; i++)
		{
			byte *px = pixels + i * 4;
			byte tmp = px[0];
			px[0] = px[2];
			px[2] = tmp;
		}

		size = datasize;
		format = SRC_RGBA;
	}
	else
	{
		format = SRC_INDEXED;
	}

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
		TexMgr_LoadImage (loadmodel, name, width, height, format,
					  pixels, loadmodel->name, offset,
					  TEXPREF_PAD | TEXPREF_ALPHA | TEXPREF_NOPICMIP); //johnfitz -- TexMgr

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}



/*
=================
Mod_LoadSpriteGroup
=================
*/
static void *Mod_LoadSpriteGroup (void * pin, mspriteframe_t **ppframe, int framenum, spriteframetype_t type, int version)
{
	dspritegroup_t		*pingroup;
	mspritegroup_t		*pspritegroup;
	int					 i, numframes;
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
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i], framenum * 100 + i, version);
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
	if (version != SPRITE_VERSION && version != SPRITE32_VERSION)
		Sys_Error ("%s has wrong version number (%i should be %i or %i)",
				mod->name, version, SPRITE_VERSION, SPRITE32_VERSION);

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
					Mod_LoadSpriteFrame (pframetype + 1, &psprite->frames[i].frameptr, i, version);
		}
		else
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteGroup (pframetype + 1, &psprite->frames[i].frameptr, i, frametype, version);
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
	hashmap = (int *) calloc (hashsize, sizeof (*hashmap));
	weld = (unsigned short *) malloc (numverts * sizeof (*weld));
	normals = (vec3_t *) calloc (numverts, sizeof (vec3_t));
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

	free (normals);
	free (weld);
	free (hashmap);
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
		free(ctx->animfile);
		return;
	}

	MD5EXPECT("numAnimatedComponents");	rawcount = MD5UINT();

	if (ctx->numjoints != numbones)
		Sys_Error ("%s has incorrect bone count", fname);

	raw = (float *) Z_Malloc(sizeof(*raw)*(rawcount+6));
	ab = (md5animbase_t *) Z_Malloc(sizeof(*ab)*ctx->numjoints);

	ctx->posedata = outposes = (bonepose_t *) Hunk_Alloc(sizeof(*outposes)*ctx->numjoints*ctx->numposes);
	frameposes = (bonepose_t *) malloc (sizeof (*frameposes) * ctx->numjoints);
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
	free(frameposes);
	free(ctx->animfile);
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
                                        char emissivename[MAX_QPATH];
                                        surf->gltextures[surf->numskins][f] = TexMgr_LoadImage (mod, texname, fwidth, fheight, fmt, data, texname, 0, TEXPREF_ALPHA|TEXPREF_NOBRIGHT|TEXPREF_MIPMAP );
                                        surf->fbtextures[surf->numskins][f] = NULL;
                                        surf->emissivetextures[surf->numskins][f] = NULL;
                                        if (fmt == SRC_INDEXED)
                                        {       //8bit base texture. use it for fullbrights.
                                                if (Mod_CheckFullbrights (data, fwidth*fheight))
                                                        surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage (mod, va("%s_luma", texname), fwidth, fheight, fmt, data, texname, 0, TEXPREF_ALPHA|TEXPREF_FULLBRIGHT|TEXPREF_MIPMAP );
                                        }
                                        else
                                        {       //we found a 32bit base texture.
                                                unsigned int ewidth, eheight;
                                                enum srcformat efmt = SRC_RGBA;
                                               void *edata;
                                               int loadmark;

                                                q_snprintf(emissivename, sizeof(emissivename), "progs/%s_%02u_%02u_emissive", com_token, surf->numskins, f);
                                                loadmark = Hunk_LowMark ();
                                                edata = Image_LoadImage (emissivename, (int *)&ewidth, (int *)&eheight, &efmt);
                                                if (edata)
                                                        surf->emissivetextures[surf->numskins][f] = TexMgr_LoadImage(mod, emissivename, ewidth, eheight, efmt, edata, emissivename, 0, TEXPREF_MIPMAP);
                                                Hunk_FreeToLowMark (loadmark);

                                                if (!surf->fbtextures[surf->numskins][f])
                                                {
                                                        unsigned int glowwidth, glowheight;
                                                        enum srcformat glowfmt = SRC_RGBA;
                                                        void *glowdata;

                                                        q_snprintf(texname, sizeof(texname), "progs/%s_%02u_%02u_glow", com_token, surf->numskins, f);
                                                        loadmark = Hunk_LowMark ();
                                                        glowdata = Image_LoadImage (texname, (int *)&glowwidth, (int *)&glowheight, &glowfmt);
                                                        if (glowdata)
                                                                surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage(mod, texname, glowwidth, glowheight, glowfmt, glowdata, texname, 0, TEXPREF_MIPMAP);
                                                        Hunk_FreeToLowMark (loadmark);
                                                }
                                                if (!surf->fbtextures[surf->numskins][f])
                                                {
                                                        unsigned int lumawidth, lumaheight;
                                                        enum srcformat lumafmt = SRC_RGBA;
                                                        void *lumadata;

                                                        q_snprintf(texname, sizeof(texname), "progs/%s_%02u_%02u_luma", com_token, surf->numskins, f);
                                                        loadmark = Hunk_LowMark ();
                                                        lumadata = Image_LoadImage (texname, (int *)&lumawidth, (int *)&lumaheight, &lumafmt);
                                                        if (lumadata)
                                                                surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage(mod, texname, lumawidth, lumaheight, lumafmt, lumadata, texname, 0, TEXPREF_MIPMAP);
                                                        Hunk_FreeToLowMark (loadmark);
                                                }
                                        }

					//now try to load glow/luma image from the same place
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
