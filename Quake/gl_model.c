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
#include <limits.h>

static qmodel_t*	loadmodel;
static char	loadname[32];	// for hunk tags

static void Mod_LoadSpriteModel (qmodel_t *mod, void *buffer);
static void Mod_LoadBrushModel (qmodel_t *mod, void *buffer);
static void Mod_LoadAliasModel (qmodel_t *mod, void *buffer);
static void Mod_LoadMD5MeshModel (qmodel_t *mod, const char *buffer);
static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash);
static void Mod_LoadBSPXLumps (dheader_t *header);
static void Mod_ProcessLeafs_S (const dsleaf_t *in, size_t count);
static void Mod_ProcessLeafs_L1 (const dl1leaf_t *in, size_t count);
static void Mod_ProcessLeafs_L2 (const dl2leaf_t *in, size_t count);

static void Mod_Print (void);

static cvar_t	external_ents = {"external_ents", "1", CVAR_ARCHIVE};
static cvar_t	external_vis = {"external_vis", "1", CVAR_ARCHIVE};
cvar_t			r_md5 = {"r_md5", "1", CVAR_ARCHIVE};

static byte	*mod_novis;
static int	mod_novis_capacity;

static byte	*mod_base;
static byte	*mod_decompressed;
static int	mod_decompressed_capacity;

static void Mod_CheckWaterVis (void);
static void Mod_FindUsedTextures (qmodel_t *mod);

typedef void (*mod_lump_error_fn)(const char *fmt, ...);

static void Mod_LumpError (const char *fmt, ...)
{
	va_list		argptr;
	char		message[1024];

	va_start (argptr, fmt);
	q_vsnprintf (message, sizeof (message), fmt, argptr);
	va_end (argptr);

	Sys_Error ("%s", message);
}

typedef struct
{
	const byte	*data;
	size_t		size;
} mod_lump_info_t;

typedef struct
{
	char		id[4];
	uint32_t	numlumps;
} bspx_header_t;

typedef struct
{
	char		lumpname[24];
	uint32_t	fileofs;
	uint32_t	filelen;
} bspx_lump_t;

typedef struct
{
        uint16_t        lmwidth;
        uint16_t        lmheight;
        int32_t         offset;
        float           world_to_lm[2][4];
} bspx_decoupled_lm_disk_t;

static mod_lump_info_t Mod_LumpData (const char *context, const char *lumpname, const lump_t *l, mod_lump_error_fn error_fn)
{
	mod_lump_info_t info;
	qfileofs_t	start;
	qfileofs_t	length;
	qfileofs_t	end;

	info.data = mod_base + l->fileofs;
	info.size = 0;

	if (!l->filelen)
		return info;

	if (l->fileofs < 0 || l->filelen < 0)
		error_fn ("%s: %s lump has negative offset or length in %s", context, lumpname, loadmodel->name);

	start = (qfileofs_t) l->fileofs;
	length = (qfileofs_t) l->filelen;
	end = start + length;

	if (length < 0 || start < 0 || end < start)
		error_fn ("%s: %s lump overflow in %s", context, lumpname, loadmodel->name);

	const qfileofs_t filesize = loadmodel ? loadmodel->filesize : -1;

	if (filesize >= 0 && end > filesize)
		error_fn ("%s: %s lump extends past file size in %s", context, lumpname, loadmodel->name);

	info.size = (size_t) length;
	return info;
}

static size_t Mod_LumpElementCount (const char *context, const char *lumpname, size_t element_size, const lump_t *l, mod_lump_error_fn error_fn, const void **data_out)
{
	mod_lump_info_t info = Mod_LumpData (context, lumpname, l, error_fn);

	if (element_size && (info.size % element_size))
		error_fn ("%s: %s lump size (%d bytes) is not a multiple of %zu in %s", context, lumpname, l->filelen, element_size, loadmodel->name);

	if (data_out)
		*data_out = info.data;

	return element_size ? (info.size / element_size) : 0;
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
		mod_decompressed_capacity = (row + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK;
		mod_decompressed = (byte *) realloc (mod_decompressed, mod_decompressed_capacity);
		if (!mod_decompressed)
			Sys_Error ("Mod_DecompressVis: realloc() failed on %d bytes", mod_decompressed_capacity);
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
		mod_novis = (byte *) realloc (mod_novis, mod_novis_capacity);
		if (!mod_novis)
			Sys_Error ("Mod_NoVisPVS: realloc() failed on %d bytes", mod_novis_capacity);
		
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
	loadmodel->filesize = com_filesize;

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

	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buf);
		break;

	default:
		Mod_LoadBrushModel (mod, buf);
		break;
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

	mod = Mod_FindName (name);

	return Mod_LoadModel (mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

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
static void Mod_LoadLighting (lump_t *l)
{
	mod_lump_info_t lump = Mod_LumpData ("Mod_LoadLighting", "lighting", l, Mod_LumpError);
	const byte	*raw = lump.data;
	size_t		raw_size = lump.size;
	byte		*out, *data, *inbuf;
	byte		d, q64_b0, q64_b1;
	char		litfilename[MAX_OSPATH];
	unsigned int	path_id;
	int		mark;

	loadmodel->lightdata = NULL;
	loadmodel->litfile = false;

	if (loadmodel->bspx.rgb_lightdata)
	{
		loadmodel->lightdata = loadmodel->bspx.rgb_lightdata;
		loadmodel->litfile = true;
		return;
	}
	// LordHavoc: check for a .lit file
	q_strlcpy(litfilename, loadmodel->name, sizeof(litfilename));
	COM_StripExtension(litfilename, litfilename, sizeof(litfilename));
	q_strlcat(litfilename, ".lit", sizeof(litfilename));
	mark = Hunk_LowMark();
	data = (byte*) COM_LoadHunkFile (litfilename, &path_id);
	if (data)
	{
		// use lit file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < loadmodel->path_id)
		{
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("ignored %s from a gamedir with lower priority\n", litfilename);
		}
		else if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
		{
			int version = LittleLong(((int *)data)[1]);
			if (version == 1)
			{
				qfileofs_t expected = 8 + (qfileofs_t)raw_size * 3;
				if (com_filesize == expected)
				{
					Con_DPrintf2("%s loaded\n", litfilename);
					loadmodel->lightdata = data + 8;
					loadmodel->litfile = true;
					return;
				}
				Hunk_FreeToLowMark(mark);
				Con_Printf("Outdated .lit file (%s should be %" SDL_PRIs64 " bytes, not %" SDL_PRIs64 "\n", litfilename, expected, com_filesize);
			}
			else
			{
				Hunk_FreeToLowMark(mark);
				Con_Printf("Unknown .lit file version (%d)\n", version);
			}
		}
		else
		{
			Hunk_FreeToLowMark(mark);
			Con_Printf("Corrupt .lit file (old version?), ignoring\n");
		}
	}

	// LordHavoc: no .lit found, expand the white lighting data to color
	if (!raw_size)
		return;

	if (loadmodel->bspversion == BSPVERSION_QUAKE64)
	{
		// RGB lightmap samples are packed in 16bits.
		// RRRRR GGGGG BBBBBB
		if (raw_size & 1)
			Host_Error("Mod_LoadLighting: odd Quake64 lightmap size in %s", loadmodel->name);
		if ((raw_size / 2) > (size_t)INT_MAX / 3)
			Host_Error("Mod_LoadLighting: Quake64 lightmap too large in %s", loadmodel->name);

		loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill ((int)((raw_size / 2) * 3), litfilename);
		const byte *in = raw;
		out = loadmodel->lightdata;

		for (size_t i = 0; i < raw_size / 2; i++)
		{
			q64_b0 = *in++;
			q64_b1 = *in++;

			*out++ = q64_b0 & 0xf8;/* 0b11111000 */
			*out++ = (byte)(((q64_b0 & 0x07) << 5) + ((q64_b1 & 0xc0) >> 5));/* 0b00000111, 0b11000000 */
			*out++ = (byte)((q64_b1 & 0x3f) << 2);/* 0b00111111 */
		}
		return;
	}

	if (raw_size > (size_t)INT_MAX / 3)
		Host_Error("Mod_LoadLighting: lightmap too large in %s", loadmodel->name);

	loadmodel->lightdata = (byte *) Hunk_AllocNameNoFill ((int)(raw_size * 3), litfilename);
	inbuf = loadmodel->lightdata + (int)(raw_size * 2); // place the file at the end, so it will not be overwritten until the very last write
	out = loadmodel->lightdata;
	memcpy (inbuf, raw, raw_size);
	for (size_t i = 0; i < raw_size; i++)
	{
		d = *inbuf++;
		*out++ = d;
		*out++ = d;
		*out++ = d;
	}
}


/*
=================
Mod_LoadVisibility
=================
*/
static void Mod_LoadVisibility (lump_t *l)
{
	mod_lump_info_t lump = Mod_LumpData ("Mod_LoadVisibility", "visibility", l, Mod_LumpError);
	size_t size = lump.size;

	loadmodel->viswarn = false;
	if (!size)
	{
		loadmodel->visdata = NULL;
		return;
	}
	if (size > (size_t)INT_MAX)
		Host_Error("Mod_LoadVisibility: vis lump too large in %s", loadmodel->name);
	loadmodel->visdata = (byte *) Hunk_AllocNameNoFill ((int)size, loadname);
	memcpy (loadmodel->visdata, lump.data, size);
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
	mod_lump_info_t lump = Mod_LumpData ("Mod_LoadEntities", "entities", l, Mod_LumpError);
	size_t size = lump.size;

	if (! external_ents.value)
		goto _load_embedded;

	mark = Hunk_LowMark();
	if (size > 0) {
		crc = CRC_Block(lump.data, size - 1);
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
	if (!size)
	{
		loadmodel->entities = NULL;
		return;
	}
	if (size > (size_t)INT_MAX)
		Host_Error("Mod_LoadEntities: entities lump too large in %s", loadmodel->name);
	loadmodel->entities = (char *) Hunk_AllocNameNoFill ((int)size, loadname);
	memcpy (loadmodel->entities, lump.data, size);
}


/*
=================
Mod_LoadVertexes
=================
*/
static void Mod_LoadVertexes (lump_t *l)
{
	const dvertex_t	*in;
	mvertex_t	*out;
	size_t		count;
	int		i;

	count = Mod_LumpElementCount ("Mod_LoadVertexes", "vertexes", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX / sizeof(*out))
		Host_Error("Mod_LoadVertexes: too many vertexes in %s", loadmodel->name);
	out = (mvertex_t *) Hunk_AllocNameNoFill ((int)(count * sizeof(*out)), loadname);

	loadmodel->vertexes = out;
	loadmodel->numvertexes = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
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
	size_t count;
	int i;

	if (bsp2)
	{
		const dledge_t *in;

		count = Mod_LumpElementCount ("Mod_LoadEdges", "edges", sizeof(*in), l, Mod_LumpError, (const void **)&in);
		if (count >= (size_t)INT_MAX)
			Host_Error("Mod_LoadEdges: too many edges in %s", loadmodel->name);
		if (count + 1 > (size_t)INT_MAX / sizeof(*out))
			Host_Error("Mod_LoadEdges: edge allocation overflow in %s", loadmodel->name);
		out = (medge_t *) Hunk_AllocNameNoFill ((int)((count + 1) * sizeof(*out)), loadname);

		loadmodel->edges = out;
		loadmodel->numedges = (int)count;

		for (i=0 ; i<(int)count ; i++, in++, out++)
		{
			out->v[0] = LittleLong(in->v[0]);
			out->v[1] = LittleLong(in->v[1]);
		}
	}
	else
	{
		const dsedge_t *in;

		count = Mod_LumpElementCount ("Mod_LoadEdges", "edges", sizeof(*in), l, Mod_LumpError, (const void **)&in);
		if (count >= (size_t)INT_MAX)
			Host_Error("Mod_LoadEdges: too many edges in %s", loadmodel->name);
		if (count + 1 > (size_t)INT_MAX / sizeof(*out))
			Host_Error("Mod_LoadEdges: edge allocation overflow in %s", loadmodel->name);
		out = (medge_t *) Hunk_AllocNameNoFill ((int)((count + 1) * sizeof(*out)), loadname);

		loadmodel->edges = out;
		loadmodel->numedges = (int)count;

		for (i=0 ; i<(int)count ; i++, in++, out++)
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
	const texinfo_t *in;
	mtexinfo_t *out;
	size_t count;
	int i, j, miptex;
	int missing = 0; //johnfitz

	count = Mod_LumpElementCount ("Mod_LoadTexinfo", "texinfo", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX / sizeof(*out))
		Host_Error("Mod_LoadTexinfo: too many texinfo entries in %s", loadmodel->name);
	out = (mtexinfo_t *) Hunk_AllocNameNoFill ((int)(count * sizeof(*out)), loadname);

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
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
		int scale = 1 << s->lightmap_shift;
		double scaled = (double)scale;
		int bmin = scale * (int) floor (mins[i] / scaled);
		int bmax = scale * (int) ceil (maxs[i] / scaled);

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
	const dsface_t	*ins = NULL;
	const dlface_t	*inl = NULL;
	msurface_t	*out;
	size_t		count;
	int		i, surfnum, lofs;
	int		planenum, side, texinfon;
	const byte	*lmshift = loadmodel->bspx.lmshift;
	size_t		lmshift_count = loadmodel->bspx.lmshift_count;
	const int32_t	*lmoffsets = loadmodel->bspx.lmoffsets;
	size_t		lmoffset_count = loadmodel->bspx.lmoffset_count;
	const void		*lmstyles_data = loadmodel->bspx.lmstyles;
	size_t		lmstyles_size = loadmodel->bspx.lmstyles_size;
	int		lmstyles_per_face = loadmodel->bspx.lmstyles_per_face;
	qboolean	lmstyles_is_16bit = loadmodel->bspx.lmstyles_is_16bit;
	qboolean	lmstyles_overflow_warned = false;
	qboolean	lmstyles_range_warned = false;
	qboolean	lightdir_warned = false;
	const char      *lmstyle_name = lmstyles_is_16bit ? "LMSTYLE16" : "LMSTYLE";
	const model_bspx_decoupled_lm_t *decoupled_lm = loadmodel->bspx.decoupled_lm;
	int             decoupled_lm_count = loadmodel->bspx.decoupled_lm_count;

	if (bsp2)
	{
		count = Mod_LumpElementCount ("Mod_LoadFaces", "faces", sizeof(*inl), l, Mod_LumpError, (const void **)&inl);
	}
	else
	{
		count = Mod_LumpElementCount ("Mod_LoadFaces", "faces", sizeof(*ins), l, Mod_LumpError, (const void **)&ins);
	}
	if (count > (size_t)INT_MAX / sizeof(*out))
		Host_Error("Mod_LoadFaces: too many faces in %s", loadmodel->name);
	out = (msurface_t *)Hunk_AllocName ( (int)(count*sizeof(*out)), loadname);

	//johnfitz -- warn mappers about exceeding old limits
	if (count > 32767 && !bsp2)
		Con_DWarning ("%zu faces exceeds standard limit of 32767.\n", count);
	//johnfitz

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = (int)count;

	if (lmstyles_data && lmstyles_per_face > 0)
	{
		size_t	entry_size = lmstyles_is_16bit ? sizeof(uint16_t) : sizeof(byte);
		size_t	total_entries;

		if (!entry_size || (lmstyles_size % entry_size) != 0)
		{
			Con_Printf("%s: ignoring malformed %s BSPX lump (unexpected size %zu bytes)\n", loadmodel->name, lmstyle_name, lmstyles_size);
			lmstyles_data = NULL;
			lmstyles_per_face = 0;
		}
		else
		{
			total_entries = lmstyles_size / entry_size;
			if (!total_entries || (total_entries % (size_t)lmstyles_per_face))
			{
				Con_Printf("%s: ignoring malformed %s BSPX lump (per-face count mismatch)\n", loadmodel->name, lmstyle_name);
				lmstyles_data = NULL;
				lmstyles_per_face = 0;
			}
			else
			{
				size_t	face_total = total_entries / (size_t)lmstyles_per_face;
				if (face_total != count)
				{
					Con_Printf("%s: ignoring %s BSPX lump with %zu faces (expected %zu)\n", loadmodel->name, lmstyle_name, face_total, count);
					lmstyles_data = NULL;
					lmstyles_per_face = 0;
				}
			}
		}
	}
	else
	{
		lmstyles_data = NULL;
		lmstyles_per_face = 0;
	}

	for (surfnum=0 ; surfnum<(int)count ; surfnum++, out++)
        {
                texture_t *texture;
                const model_bspx_decoupled_lm_t *dlm = NULL;
		if (decoupled_lm && surfnum < decoupled_lm_count)
		{
			const model_bspx_decoupled_lm_t *candidate = &decoupled_lm[surfnum];
			if (candidate->lmwidth && candidate->lmheight)
				dlm = candidate;
		}
                out->bspx_has_decoupled_lm = false;
                out->bspx_lmwidth = 0;
                out->bspx_lmheight = 0;
                out->deluxemap = NULL;
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

		byte shift = 4;
		if (lmshift && surfnum < (int)lmshift_count)
		{
			shift = lmshift[surfnum];
			if (shift > 7)
				shift = 7;
		}
		if (dlm)
			shift = 4;
		out->lightmap_shift = shift;

		if (lmoffsets && surfnum < (int)lmoffset_count)
			lofs = lmoffsets[surfnum];
		if (dlm)
			lofs = dlm->offset;

		if (lmstyles_data)
		{
			size_t	base = (size_t)surfnum * (size_t)lmstyles_per_face;
			int		copy_count = lmstyles_per_face;
			if (copy_count > MAXLIGHTMAPS)
			{
				if (!lmstyles_overflow_warned)
				{
					Con_Warning("%s: %s BSPX lump specifies %d styles per face; truncating to %d\n", loadmodel->name, lmstyle_name, lmstyles_per_face, MAXLIGHTMAPS);
					lmstyles_overflow_warned = true;
				}
				copy_count = MAXLIGHTMAPS;
			}
			for (i = 0; i < MAXLIGHTMAPS; i++)
				out->styles[i] = 255;
			if (lmstyles_is_16bit)
			{
				const uint16_t *styles16 = ((const uint16_t *)lmstyles_data) + base;
				for (i = 0; i < copy_count; i++)
				{
					uint16_t value = styles16[i];
					if (value == 0xffff)
					{
						out->styles[i] = 255;
					}
					else if (value > 255)
					{
						if (!lmstyles_range_warned)
						{
							Con_Warning("%s: %s BSPX lump contains lightstyle %u outside the 0-255 range; treating as unlit\n", loadmodel->name, lmstyle_name, value);
							lmstyles_range_warned = true;
						}
						out->styles[i] = 255;
					}
					else
						out->styles[i] = (byte)value;
				}
			}
			else
			{
				const byte *styles8 = ((const byte *)lmstyles_data) + base;
				for (i = 0; i < copy_count; i++)
					out->styles[i] = styles8[i];
			}
		}

		if (dlm)
		{
			out->bspx_has_decoupled_lm = true;
			out->bspx_lmwidth = dlm->lmwidth;
			out->bspx_lmheight = dlm->lmheight;
			for (int row = 0; row < 2; row++)
			{
				for (int col = 0; col < 4; col++)
					out->bspx_world_to_lm[row][col] = dlm->world_to_lm[row][col];
			}
			if (dlm->offset < 0)
				lofs = -1;
			else
				lofs = dlm->offset;
			out->texturemins[0] = out->texturemins[1] = 0;
			out->extents[0] = ((int)dlm->lmwidth - 1) << out->lightmap_shift;
			out->extents[1] = ((int)dlm->lmheight - 1) << out->lightmap_shift;
		}

		out->flags = 0;
		if (out->numedges < 3)
			Con_Warning("surfnum %d: bad numedges %d\n", surfnum, out->numedges);

		if (side)
			out->flags |= SURF_PLANEBACK;

		out->plane = loadmodel->planes + planenum;

		out->texinfo = loadmodel->texinfo + texinfon;

		if (!out->bspx_has_decoupled_lm)
			CalcSurfaceExtents (out);

		Mod_CalcSurfaceBounds (out); //johnfitz -- for per-surface frustum culling

	// lighting info
		if (loadmodel->bspversion == BSPVERSION_QUAKE64)
			lofs /= 2; // Q64 samples are 16bits instead 8 in normal Quake

		if (lofs == -1)
			out->samples = NULL;
		else
			out->samples = loadmodel->lightdata + (lofs * 3); //johnfitz -- lit support via lordhavoc (was "+ i")

		texture = loadmodel->textures[out->texinfo->texnum];
		if (loadmodel->bspx.lightdir_data && lofs >= 0)
		{
			int smax = (out->extents[0] >> out->lightmap_shift) + 1;
			int tmax = (out->extents[1] >> out->lightmap_shift) + 1;
			size_t luxels = (size_t)smax * (size_t)tmax;
			size_t offset = (size_t)lofs * 3;
			size_t needed = luxels * 3;
			if (offset + needed <= loadmodel->bspx.lightdir_data_size)
				out->deluxemap = loadmodel->bspx.lightdir_data + offset;
			else if (!lightdir_warned)
			{
				Con_Warning("%s: ignoring malformed LIGHTINGDIR BSPX data (surf %d)\n", loadmodel->name, surfnum);
				lightdir_warned = true;
			}
		}


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
	int		i, j, p;
	size_t		count;
	const dsnode_t		*in;
	mnode_t		*out;

	count = Mod_LumpElementCount ("Mod_LoadNodes", "nodes", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadNodes: too many nodes in %s", loadmodel->name);
	out = (mnode_t *) Hunk_AllocName ((int)(count * sizeof(*out)), loadname);

	//johnfitz -- warn mappers about exceeding old limits
	if (count > 32767)
		Con_DWarning ("Mod_LoadNodes: %zu nodes exceeds standard limit of 32767.\n", count);
	//johnfitz

	loadmodel->nodes = out;
	loadmodel->numnodes = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong (in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = (unsigned short) LittleShort (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = (unsigned short) LittleShort (in->numfaces); //johnfitz -- explicit cast as unsigned short

		for (j=0 ; j<2 ; j++)
		{
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = (unsigned short) LittleShort (in->children[j]);
			if (p < (int)count)
				out->children[j] = loadmodel->nodes + p;
			else
			{
				p = 65535 - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else
				{
					Con_Printf ("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			}
			//johnfitz
		}
	}
}

static void Mod_LoadNodes_L1 (lump_t *l)
{
	int		i, j, p;
	size_t		count;
	const dl1node_t		*in;
	mnode_t		*out;

	count = Mod_LumpElementCount ("Mod_LoadNodes", "nodes", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadNodes: too many nodes in %s", loadmodel->name);
	out = (mnode_t *) Hunk_AllocName ((int)(count * sizeof(*out)), loadname);

	loadmodel->nodes = out;
	loadmodel->numnodes = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong (in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = LittleLong (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = LittleLong (in->numfaces); //johnfitz -- explicit cast as unsigned short

		for (j=0 ; j<2 ; j++)
		{
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = LittleLong (in->children[j]);
			if (p >= 0 && p < (int)count)
				out->children[j] = loadmodel->nodes + p;
			else
			{
				p = 0xffffffff - p; //note this uses 0xffffffff intentionally, -1 is leaf 0
				if (p >= 0 && p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else
				{
					Con_Printf ("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			}
			//johnfitz
		}
	}
}

static void Mod_LoadNodes_L2 (lump_t *l)
{
	int		i, j, p;
	size_t		count;
	const dl2node_t		*in;
	mnode_t		*out;

	count = Mod_LumpElementCount ("Mod_LoadNodes", "nodes", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadNodes: too many nodes in %s", loadmodel->name);
	out = (mnode_t *) Hunk_AllocName ((int)(count * sizeof(*out)), loadname);

	loadmodel->nodes = out;
	loadmodel->numnodes = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleFloat (in->mins[j]);
			out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
		}

		p = LittleLong (in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = LittleLong (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = LittleLong (in->numfaces); //johnfitz -- explicit cast as unsigned short

		for (j=0 ; j<2 ; j++)
		{
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = LittleLong (in->children[j]);
			if (p >= 0 && p < (int)count)
				out->children[j] = loadmodel->nodes + p;
			else
			{
				p = 0xffffffff - p; //note this uses 0xffffffff intentionally, -1 is leaf 0
				if (p >= 0 && p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else
				{
					Con_Printf ("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
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
		Mod_LoadNodes_L2 (l);
	else if (bsp2)
		Mod_LoadNodes_L1 (l);
	else
		Mod_LoadNodes_S (l);

	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

static void Mod_ProcessLeafs_S (const dsleaf_t *in, size_t count)
{
	mleaf_t	*out;
	int		i, j, p;

	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadLeafs: too many leafs in %s", loadmodel->name);
	out = (mleaf_t *) Hunk_AllocName ( (int)(count*sizeof(*out)), loadname);

	//johnfitz
	if (count > 32767)
		Host_Error ("Mod_LoadLeafs: %zu leafs exceeds limit of 32767.", count);
	//johnfitz

	loadmodel->leafs = out;
	loadmodel->numleafs = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
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


static void Mod_ProcessLeafs_L1 (const dl1leaf_t *in, size_t count)
{
	mleaf_t	*out;
	int		i, j, p;

	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadLeafs: too many leafs in %s", loadmodel->name);

	out = (mleaf_t *) Hunk_AllocName ( (int)(count * sizeof(*out)), loadname);

	loadmodel->leafs = out;
	loadmodel->numleafs = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
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


static void Mod_ProcessLeafs_L2 (const dl2leaf_t *in, size_t count)
{
	mleaf_t	*out;
	int		i, j, p;

	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadLeafs: too many leafs in %s", loadmodel->name);

	out = (mleaf_t *) Hunk_AllocName ( (int)(count * sizeof(*out)), loadname);

	loadmodel->leafs = out;
	loadmodel->numleafs = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
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

static void Mod_LoadLeafs (lump_t *l, int bsp2)
{
        size_t count = 0;

        loadmodel->leafs = NULL;
        loadmodel->numleafs = 0;

        if (bsp2 == 2)
        {
                const dl2leaf_t *in = NULL;

                count = Mod_LumpElementCount ("Mod_LoadLeafs", "leafs", sizeof(*in), l, Mod_LumpError, (const void **)&in);
                if (count)
                        Mod_ProcessLeafs_L2 (in, count);
        }
        else if (bsp2)
        {
                const dl1leaf_t *in = NULL;

                count = Mod_LumpElementCount ("Mod_LoadLeafs", "leafs", sizeof(*in), l, Mod_LumpError, (const void **)&in);
                if (count)
                        Mod_ProcessLeafs_L1 (in, count);
        }
        else
        {
                const dsleaf_t *in = NULL;

                count = Mod_LumpElementCount ("Mod_LoadLeafs", "leafs", sizeof(*in), l, Mod_LumpError, (const void **)&in);
                if (count)
                        Mod_ProcessLeafs_S (in, count);
        }
}

static void Mod_LoadClipnodes (lump_t *l, qboolean bsp2)
{
	const dsclipnode_t	*ins = NULL;
	const dlclipnode_t	*inl = NULL;
	mclipnode_t	*out; //johnfitz -- was dclipnode_t
	size_t		total;
	int		i, count;
	hull_t		*hull;

	if (bsp2)
	{
		total = Mod_LumpElementCount ("Mod_LoadClipnodes", "clipnodes", sizeof(*inl), l, Mod_LumpError, (const void **)&inl);
	}
	else
	{
		total = Mod_LumpElementCount ("Mod_LoadClipnodes", "clipnodes", sizeof(*ins), l, Mod_LumpError, (const void **)&ins);
	}

	if (total > (size_t)INT_MAX)
		Host_Error ("Mod_LoadClipnodes: too many clipnodes in %s", loadmodel->name);
	count = (int)total;

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

	hull = &loadmodel->hulls[3];
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

static void Mod_MakeHull0 (void)
{
	mnode_t         *in, *child;
	mclipnode_t *out; //johnfitz -- was dclipnode_t
	int             i, j, count;
	hull_t          *hull;

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

static void Mod_LoadMarksurfaces (lump_t *l, int bsp2)
{
	int             *out;
	size_t          total;

	if (bsp2)
	{
		const unsigned int *in;
		total = Mod_LumpElementCount ("Mod_LoadMarksurfaces", "marksurfaces", sizeof(*in), l, Mod_LumpError, (const void **)&in);
		if (total > (size_t)INT_MAX)
			Host_Error ("Mod_LoadMarksurfaces: too many marksurfaces in %s", loadmodel->name);
		out = (int*)Hunk_AllocNameNoFill ( (int)(total*sizeof(*out)), loadname);

		loadmodel->marksurfaces = out;
		loadmodel->nummarksurfaces = (int)total;

		for (size_t i=0 ; i<total ; i++)
		{
			int j = LittleLong(in[i]);
			if (j >= loadmodel->numsurfaces)
				Host_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = j;
		}
	}
	else
	{
		const short *in;
		total = Mod_LumpElementCount ("Mod_LoadMarksurfaces", "marksurfaces", sizeof(*in), l, Mod_LumpError, (const void **)&in);
		if (total > (size_t)INT_MAX)
			Host_Error ("Mod_LoadMarksurfaces: too many marksurfaces in %s", loadmodel->name);
		out = (int*)Hunk_AllocNameNoFill ( (int)(total*sizeof(*out)), loadname);

		loadmodel->marksurfaces = out;
		loadmodel->nummarksurfaces = (int)total;

		//johnfitz -- warn mappers about exceeding old limits
		if (total > 32767)
			Con_DWarning ("%zu marksurfaces exceeds standard limit of 32767.\n", total);
		//johnfitz

		for (size_t i=0 ; i<total ; i++)
		{
			int j = (unsigned short)LittleShort(in[i]); //johnfitz -- explicit cast as unsigned short
			if (j >= loadmodel->numsurfaces)
				Sys_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = j;
		}
	}
}

static void Mod_LoadSurfedges (lump_t *l)
{
	const int *in;
	int *out;
	size_t count;

	count = Mod_LumpElementCount ("Mod_LoadSurfedges", "surfedges", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadSurfedges: too many surfedges in %s", loadmodel->name);
	out = (int *) Hunk_AllocNameNoFill ( (int)(count*sizeof(*out)), loadname);

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = (int)count;

	for (size_t i=0 ; i<count ; i++)
		out[i] = LittleLong (in[i]);
}

static void Mod_LoadPlanes (lump_t *l)
{
	const dplane_t *in;
	mplane_t *out;
	size_t count;
	int i, j, bits;

	count = Mod_LumpElementCount ("Mod_LoadPlanes", "planes", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX / sizeof(*out))
		Host_Error ("Mod_LoadPlanes: too many planes in %s", loadmodel->name);
	out = (mplane_t *) Hunk_AllocNameNoFill ( (int)(count*sizeof(*out)), loadname);

	loadmodel->planes = out;
	loadmodel->numplanes = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
	{
		out->normal[0] = LittleFloat (in->normal[0]);
		out->normal[1] = LittleFloat (in->normal[1]);
		out->normal[2] = LittleFloat (in->normal[2]);
		out->dist = LittleFloat (in->dist);

		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			if (out->normal[j] < 0)
				bits |= (1<<j);
		}

		out->type = LittleLong (in->type);
		out->signbits = bits;
	}
}

static void Mod_LoadSubmodels (lump_t *l)
{
	const dmodel_t *in;
	dmodel_t *out;
	size_t count;
	int i, j;

	count = Mod_LumpElementCount ("Mod_LoadSubmodels", "submodels", sizeof(*in), l, Mod_LumpError, (const void **)&in);
	if (count > (size_t)INT_MAX)
		Host_Error ("Mod_LoadSubmodels: too many submodels in %s", loadmodel->name);
	out = (dmodel_t *) Hunk_AllocNameNoFill ( (int)(count*sizeof(*out)), loadname);

	loadmodel->submodels = out;
	loadmodel->numsubmodels = (int)count;

	for (i=0 ; i<(int)count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->mins[j] = LittleFloat (in->mins[j]);
			out->maxs[j] = LittleFloat (in->maxs[j]);
			out->origin[j] = LittleFloat (in->origin[j]);
		}

		for (j=0 ; j<MAX_MAP_HULLS ; j++)
			out->headnode[j] = LittleLong (in->headnode[j]);

		out->visleafs = LittleLong (in->visleafs);
		out->firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);
	}
}

static float RadiusFromBounds (const vec3_t mins, const vec3_t maxs)
{
	vec3_t	corner;
	int	i;

	for (i = 0; i < 3; i++)
	{
		float minabs = fabsf (mins[i]);
		float maxabs = fabsf (maxs[i]);
		corner[i] = (minabs > maxabs) ? minabs : maxabs;
	}

	return VectorLength (corner);
}

/* EXTERNAL VIS FILE SUPPORT */
typedef struct vispatch_s
{
	char	mapname[32];
	int	filelen;
} vispatch_t;

#define VISPATCH_HEADER_LEN 36

static FILE *Mod_FindVisibilityExternal (void)
{
	vispatch_t	header;
	char	visfilename[MAX_QPATH];
	const char	*shortname;
	unsigned int	path_id;
	FILE	*f;
	long	pos;
	size_t	r;

	q_snprintf (visfilename, sizeof(visfilename), "maps/%s.vis", loadname);
	if (COM_FOpenFile (visfilename, &f, &path_id) < 0)
	{
		Con_DPrintf ("%s not found, trying ", visfilename);
		q_snprintf (visfilename, sizeof(visfilename), "%s.vis", COM_SkipPath (com_gamedir));
		Con_DPrintf ("%s\n", visfilename);
		if (COM_FOpenFile (visfilename, &f, &path_id) < 0)
		{
			Con_DPrintf ("external vis not found\n");
			return NULL;
		}
	}
	if (path_id < loadmodel->path_id)
	{
		fclose (f);
		Con_DPrintf ("ignored %s from a gamedir with lower priority\n", visfilename);
		return NULL;
	}

	Con_DPrintf ("Found external VIS %s\n", visfilename);

	shortname = COM_SkipPath (loadmodel->name);
	pos = 0;
	while ((r = fread (&header, 1, VISPATCH_HEADER_LEN, f)) == VISPATCH_HEADER_LEN)
	{
		header.filelen = LittleLong (header.filelen);
		if (header.filelen <= 0)
		{
			fclose (f);
			return NULL;
		}
		if (!q_strcasecmp (header.mapname, shortname))
			break;
		pos += header.filelen + VISPATCH_HEADER_LEN;
		if (fseek (f, pos, SEEK_SET) != 0)
		{
			fclose (f);
			return NULL;
		}
	}
	if (r != VISPATCH_HEADER_LEN)
	{
		fclose (f);
		Con_DPrintf ("%s not found in %s\n", shortname, visfilename);
		return NULL;
	}

	return f;
}

static byte *Mod_LoadVisibilityExternal (FILE *f)
{
	int	filelen = 0;
	int	mark;
	byte	*visdata;

	if (fread (&filelen, 4, 1, f) != 1)
		return NULL;
	filelen = LittleLong (filelen);
	if (filelen <= 0)
		return NULL;
	Con_DPrintf ("...%d bytes visibility data\n", filelen);
	mark = Hunk_LowMark ();
	visdata = (byte *) Hunk_AllocNameNoFill (filelen, "EXT_VIS");
	if (!fread (visdata, filelen, 1, f))
	{
		Hunk_FreeToLowMark (mark);
		return NULL;
	}
	return visdata;
}

static void Mod_LoadLeafsExternal(FILE* f, int bsp2)
{
	int		mark, filelen;
	size_t		elem_size, count;
	void	*in;

	filelen = 0;
	if (fread(&filelen, 4, 1, f) != 1)
	{
		Con_Warning ("Couldn't read external leaf data length\n");
		return;
	}

	filelen = LittleLong(filelen);
	if (filelen <= 0)
		return;

	if (bsp2 == 2)
		elem_size = sizeof(dl2leaf_t);
	else if (bsp2)
		elem_size = sizeof(dl1leaf_t);
	else
		elem_size = sizeof(dsleaf_t);

	if ((size_t)filelen % elem_size)
	{
		Con_Warning ("Ignoring external leaf data with unexpected size (%d bytes)\n", filelen);
		return;
	}

	count = (size_t)filelen / elem_size;
	if (!count)
		return;

	Con_DPrintf("...%d bytes leaf data\n", filelen);
	mark = Hunk_LowMark ();
	in = Hunk_AllocNameNoFill (filelen, "EXT_LEAF");
	if (!fread(in, filelen, 1, f))
	{
		Hunk_FreeToLowMark (mark);
		return;
	}

	if (bsp2 == 2)
		Mod_ProcessLeafs_L2((const dl2leaf_t *)in, count);
	else if (bsp2)
		Mod_ProcessLeafs_L1((const dl1leaf_t *)in, count);
	else
		Mod_ProcessLeafs_S((const dsleaf_t *)in, count);

	Hunk_FreeToLowMark (mark);
}

static void Mod_CheckWaterVis (void)
{
	int	flags = 0;
	qboolean	water_present = false, water_vis = false;
	qboolean	lava_present = false, lava_vis = false;
	qboolean	slime_present = false, slime_vis = false;
	qboolean	tele_present = false, tele_vis = false;
	int	i, j;

	if (!loadmodel->leafs || loadmodel->numleafs <= 1 || !loadmodel->marksurfaces || !loadmodel->surfaces)
	{
		loadmodel->contentstransparent = 0;
		return;
	}

	for (i = 1; i < loadmodel->numleafs; i++)
	{
		mleaf_t *leaf = &loadmodel->leafs[i];
		qboolean leaf_has_vis = (leaf->compressed_vis != NULL);

		for (j = 0; j < leaf->nummarksurfaces; j++)
		{
			int surfindex = leaf->firstmarksurface[j];
			msurface_t *surf;

			if (surfindex < 0 || surfindex >= loadmodel->numsurfaces)
				continue;

			surf = &loadmodel->surfaces[surfindex];
			if (surf->flags & SURF_DRAWWATER)
			{
				water_present = true;
				if (leaf_has_vis)
					water_vis = true;
			}
			if (surf->flags & SURF_DRAWLAVA)
			{
				lava_present = true;
				if (leaf_has_vis)
					lava_vis = true;
			}
			if (surf->flags & SURF_DRAWSLIME)
			{
				slime_present = true;
				if (leaf_has_vis)
					slime_vis = true;
			}
			if (surf->flags & SURF_DRAWTELE)
			{
				tele_present = true;
				if (leaf_has_vis)
					tele_vis = true;
			}
		}
	}

	if (water_present && water_vis)
		flags |= SURF_DRAWWATER;
	if (lava_present && lava_vis)
		flags |= SURF_DRAWLAVA;
	if (slime_present && slime_vis)
		flags |= SURF_DRAWSLIME;
	if (tele_present && tele_vis)
		flags |= SURF_DRAWTELE;

	loadmodel->contentstransparent = flags;
}

static void Mod_FindUsedTextures (qmodel_t *mod)
{
	int	counts[TEXTYPE_COUNT] = {0};
	int	offsets[TEXTYPE_COUNT];
	int	total = 0;
	int	first, count, i;
	qboolean	*used;
	textype_t	*types;

	memset (mod->texofs, 0, sizeof (mod->texofs));
	mod->usedtextures = NULL;

	if (!mod->textures || mod->numtextures <= 0 || !mod->surfaces)
		return;

	first = mod->firstmodelsurface;
	count = mod->nummodelsurfaces;
	if (count <= 0)
	{
		mod->texofs[TEXTYPE_COUNT] = 0;
		return;
	}
	if (first < 0)
		first = 0;
	if (first > mod->numsurfaces)
		first = mod->numsurfaces;
	if (first + count > mod->numsurfaces)
		count = mod->numsurfaces - first;
	if (count <= 0)
	{
		mod->texofs[TEXTYPE_COUNT] = 0;
		return;
	}

	used = (qboolean *) calloc ((size_t)mod->numtextures, sizeof (*used));
	types = (textype_t *) malloc ((size_t)mod->numtextures * sizeof (*types));
	if (!used || !types)
	{
		free (used);
		free (types);
		Sys_Error ("Mod_FindUsedTextures: out of memory (%d textures)", mod->numtextures);
	}

	for (i = 0; i < mod->numtextures; i++)
		types[i] = (textype_t)TEXTYPE_COUNT;

	for (i = 0; i < count; i++)
	{
		msurface_t *surf = &mod->surfaces[first + i];
		int texnum = -1;

		if (surf->texinfo)
			texnum = surf->texinfo->texnum;
		if (texnum < 0 || texnum >= mod->numtextures)
			continue;
		if (used[texnum])
			continue;

		used[texnum] = true;
		if (mod->textures[texnum] && (unsigned)mod->textures[texnum]->type < TEXTYPE_COUNT)
			types[texnum] = mod->textures[texnum]->type;
		else
			types[texnum] = TEXTYPE_DEFAULT;

		counts[types[texnum]]++;
		total++;
	}

	for (i = 0; i < TEXTYPE_COUNT; i++)
		mod->texofs[i + 1] = mod->texofs[i] + counts[i];

	if (total > 0)
	{
		mod->usedtextures = (int *) Hunk_AllocName (total * sizeof (int), loadname);
		for (i = 0; i < TEXTYPE_COUNT; i++)
			offsets[i] = mod->texofs[i];

		for (i = 0; i < mod->numtextures; i++)
		{
			textype_t type = types[i];
			if ((unsigned)type >= TEXTYPE_COUNT)
				continue;
			mod->usedtextures[offsets[type]++] = i;
		}
	}

	free (used);
	free (types);
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
	dheader_t	*header;
	dmodel_t 	*bm;
	float		radius; //johnfitz

	loadmodel->type = mod_brush;
	memset(&loadmodel->bspx, 0, sizeof(loadmodel->bspx));

	header = (dheader_t *)buffer;

	mod->bspversion = LittleLong (header->version);

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
		Sys_Error ("Mod_LoadBrushModel: %s has unsupported version number (%i)", mod->name, mod->bspversion);
		break;
	}

// swap all the lumps
	mod_base = (byte *)header;

	for (i = 0; i < (int) sizeof(dheader_t) / 4; i++)
		((int *)header)[i] = LittleLong ( ((int *)header)[i]);

	Mod_LoadBSPXLumps (header);

// load into heap

	Mod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (&header->lumps[LUMP_EDGES], bsp2);
	Mod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);
	Mod_LoadTextures (&header->lumps[LUMP_TEXTURES]);
	Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (&header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces (&header->lumps[LUMP_FACES], bsp2);
	Mod_LoadMarksurfaces (&header->lumps[LUMP_MARKSURFACES], bsp2);

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
				Mod_LoadLeafsExternal(fvis, bsp2);
			}
			fclose(fvis);
			if (loadmodel->visdata && loadmodel->leafs && loadmodel->numleafs) {
				goto visdone;
			}
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("External VIS data failed, using standard vis.\n");
		}
	}

	Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (&header->lumps[LUMP_LEAFS], bsp2);
visdone:
	Mod_LoadNodes (&header->lumps[LUMP_NODES], bsp2);
	Mod_LoadClipnodes (&header->lumps[LUMP_CLIPNODES], bsp2);
	Mod_LoadEntities (&header->lumps[LUMP_ENTITIES]);
	Mod_LoadSubmodels (&header->lumps[LUMP_MODELS]);

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


static void Mod_LoadBSPXLumps (dheader_t *header)
{
	size_t bspxofs = 0;
	size_t filesize;
	const byte *base;
	uint32_t numlumps;
	const bspx_lump_t *xlump;

	if (com_filesize <= 0)
		return;

	filesize = (size_t)com_filesize;
	base = mod_base;

	size_t	face_struct_size = 0;
	size_t	face_count = 0;

	switch (loadmodel->bspversion)
	{
	case BSPVERSION:
	case BSPVERSION_QUAKE64:
		face_struct_size = sizeof(dsface_t);
		break;
	case BSP2VERSION_2PSB:
	case BSP2VERSION_BSP2:
		face_struct_size = sizeof(dlface_t);
		break;
	default:
		break;
	}

	if (face_struct_size)
	{
		int faces_filelen = header->lumps[LUMP_FACES].filelen;
		if (faces_filelen > 0 && (faces_filelen % (int)face_struct_size) == 0)
			face_count = (size_t)faces_filelen / face_struct_size;
	}

	for (int i = 0; i < HEADER_LUMPS; i++)
	{
		int fileofs = header->lumps[i].fileofs;
		int filelen = header->lumps[i].filelen;
		if (filelen <= 0)
			continue;

		size_t end = (size_t)fileofs + (size_t)filelen;
		if (end > bspxofs)
			bspxofs = end;
	}

	bspxofs = (bspxofs + 3) & ~(size_t)3;

	if (bspxofs + sizeof(bspx_header_t) > filesize)
		return;

	const bspx_header_t *bspx = (const bspx_header_t *)(base + bspxofs);
	if (memcmp(bspx->id, "BSPX", 4))
		return;

	numlumps = LittleLong(bspx->numlumps);
	xlump = (const bspx_lump_t *)(bspx + 1);

	for (uint32_t i = 0; i < numlumps; i++, xlump++)
	{
		char name[25];
		int fileofs = (int)LittleLong(xlump->fileofs);
		int filelen = (int)LittleLong(xlump->filelen);

		memcpy(name, xlump->lumpname, sizeof(xlump->lumpname));
		name[sizeof(name) - 1] = '\0';

		if (fileofs < 0 || filelen <= 0)
			continue;

		size_t end = (size_t)fileofs + (size_t)filelen;
		if (end > filesize)
			continue;

		const byte *lumpdata = base + fileofs;

		if (!strcmp(name, "LMSHIFT"))
		{
			if (loadmodel->bspx.lmshift)
				continue;
			loadmodel->bspx.lmshift = (byte *)Hunk_AllocName(filelen, loadname);
			memcpy(loadmodel->bspx.lmshift, lumpdata, filelen);
			loadmodel->bspx.lmshift_count = filelen;
		}
		else if (!strcmp(name, "LMOFFSET"))
		{
			if (loadmodel->bspx.lmoffsets)
				continue;
			if ((size_t)filelen % sizeof(int32_t))
			{
				Con_Printf("%s: ignoring malformed LMOFFSET BSPX lump\n", loadmodel->name);
				continue;
			}
			int count = filelen / (int)sizeof(int32_t);
			loadmodel->bspx.lmoffsets = (int32_t *)Hunk_AllocName(filelen, loadname);
			for (int j = 0; j < count; j++)
				loadmodel->bspx.lmoffsets[j] = LittleLong(((const int32_t *)lumpdata)[j]);
			loadmodel->bspx.lmoffset_count = count;
		}
		else if (!strcmp(name, "DECOUPLED_LM"))
		{
			if (loadmodel->bspx.decoupled_lm)
				continue;
			if ((size_t)filelen % sizeof(bspx_decoupled_lm_disk_t))
			{
				Con_Printf("%s: ignoring malformed DECOUPLED_LM BSPX lump\n", loadmodel->name);
				continue;
			}
			size_t count = (size_t)filelen / sizeof(bspx_decoupled_lm_disk_t);
			if (!count)
				continue;
			size_t entry_size = sizeof(model_bspx_decoupled_lm_t);
			if (count > (size_t)INT_MAX / entry_size)
			{
				Con_Printf("%s: ignoring oversized DECOUPLED_LM BSPX lump (%zu entries)\n", loadmodel->name, count);
				continue;
			}
			model_bspx_decoupled_lm_t *entries = (model_bspx_decoupled_lm_t *)Hunk_AllocName((int)(count * entry_size), loadname);
			const bspx_decoupled_lm_disk_t *src = (const bspx_decoupled_lm_disk_t *)lumpdata;
			for (size_t j = 0; j < count; j++)
			{
				entries[j].lmwidth = LittleShort(src[j].lmwidth);
				entries[j].lmheight = LittleShort(src[j].lmheight);
				entries[j].offset = LittleLong(src[j].offset);
				for (int r = 0; r < 2; r++)
				{
					for (int c = 0; c < 4; c++)
						entries[j].world_to_lm[r][c] = LittleFloat(src[j].world_to_lm[r][c]);
				}
			}
			loadmodel->bspx.decoupled_lm = entries;
			loadmodel->bspx.decoupled_lm_count = (int)count;
		}
		else if (!strcmp(name, "LMSTYLE") || !strcmp(name, "LMSTYLE16"))
		{
			qboolean is16 = !strcmp(name, "LMSTYLE16");
			size_t entry_size = is16 ? sizeof(uint16_t) : sizeof(byte);

			if (!face_count)
			{
				Con_Printf("%s: ignoring %s BSPX lump with no faces\n", loadmodel->name, name);
				continue;
			}

			if ((size_t)filelen % face_count)
			{
				Con_Printf("%s: ignoring malformed %s BSPX lump (length %d)\n", loadmodel->name, name, filelen);
				continue;
			}

			size_t per_face_bytes = (size_t)filelen / face_count;
			if (!per_face_bytes || (per_face_bytes % entry_size))
			{
				Con_Printf("%s: ignoring malformed %s BSPX lump (per-face size %zu)\n", loadmodel->name, name, per_face_bytes);
				continue;
			}

			int per_face = (int)(per_face_bytes / entry_size);
			if (per_face <= 0)
			{
				Con_Printf("%s: ignoring malformed %s BSPX lump (per-face count %d)\n", loadmodel->name, name, per_face);
				continue;
			}

			if (loadmodel->bspx.lmstyles)
			{
				if (loadmodel->bspx.lmstyles_is_16bit)
				{
					if (!is16)
						continue;
				}
				else if (!is16)
					continue;
			}

			byte *styles = (byte *)Hunk_AllocName(filelen, loadname);
			memcpy(styles, lumpdata, filelen);
			if (is16)
			{
				uint16_t *styles16 = (uint16_t *)styles;
				size_t total = (size_t)filelen / sizeof(uint16_t);
				for (size_t j = 0; j < total; j++)
					styles16[j] = (uint16_t)LittleShort(styles16[j]);
			}

			loadmodel->bspx.lmstyles = styles;
			loadmodel->bspx.lmstyles_size = filelen;
			loadmodel->bspx.lmstyles_per_face = per_face;
			loadmodel->bspx.lmstyles_is_16bit = is16;
		}
		else if (!strcmp(name, "RGBLIGHTING"))
		{
			if (loadmodel->bspx.rgb_lightdata)
				continue;
			loadmodel->bspx.rgb_lightdata = (byte *)Hunk_AllocName(filelen, loadname);
			memcpy(loadmodel->bspx.rgb_lightdata, lumpdata, filelen);
			loadmodel->bspx.rgb_lightdata_size = filelen;
		}
		else if (!strcmp(name, "LIGHTINGDIR"))
		{
			if (loadmodel->bspx.lightdir_data)
				continue;
			loadmodel->bspx.lightdir_data = (byte *)Hunk_AllocName(filelen, loadname);
			memcpy(loadmodel->bspx.lightdir_data, lumpdata, filelen);
			loadmodel->bspx.lightdir_data_size = filelen;
		}
		else if (!strcmp(name, "LIGHTING_E5BGR9"))
		{
			if (loadmodel->bspx.hdr_lightdata)
				continue;
			loadmodel->bspx.hdr_lightdata = (byte *)Hunk_AllocName(filelen, loadname);
			memcpy(loadmodel->bspx.hdr_lightdata, lumpdata, filelen);
			loadmodel->bspx.hdr_lightdata_size = filelen;
		}
	}
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
					{	//8bit base texture. use it for fullbrights.
						if (Mod_CheckFullbrights (data, fwidth*fheight))
							surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage (mod, va("%s_luma", texname), fwidth, fheight, fmt, data, texname, 0, TEXPREF_ALPHA|TEXPREF_FULLBRIGHT|TEXPREF_MIPMAP );
					}
					else
					{	//we found a 32bit base texture.
                                                q_snprintf(emissivename, sizeof(emissivename), "progs/%s_%02u_%02u_emissive", com_token, surf->numskins, f);
                                                surf->emissivetextures[surf->numskins][f] = TexMgr_LoadImage(mod, emissivename, surf->skinwidth, surf->skinheight, SRC_RGBA, NULL, emissivename, 0, TEXPREF_MIPMAP);
                                                if (!surf->fbtextures[surf->numskins][f])
                                                {
                                                        q_snprintf(texname, sizeof(texname), "progs/%s_%02u_%02u_glow", com_token, surf->numskins, f);
                                                        surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage(mod, texname, surf->skinwidth, surf->skinheight, SRC_RGBA, NULL, texname, 0, TEXPREF_MIPMAP);
                                                }
                                                if (!surf->fbtextures[surf->numskins][f])
                                                {
                                                        q_snprintf(texname, sizeof(texname), "progs/%s_%02u_%02u_luma", com_token, surf->numskins, f);
                                                        surf->fbtextures[surf->numskins][f] = TexMgr_LoadImage(mod, texname, surf->skinwidth, surf->skinheight, SRC_RGBA, NULL, texname, 0, TEXPREF_MIPMAP);
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
