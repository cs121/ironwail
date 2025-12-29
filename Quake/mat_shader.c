/*
Copyright (C) 2024

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

#include "quakedef.h"
#include "mat_shader.h"
#include "mat_shader_parse.h"
#include "miniz.h"

#define MAT_SHADER_HASH_SIZE 256
#define MAT_SHADER_LIST_LIMIT 64

typedef struct mat_shader_entry_s
{
	shader_material_t	*material;
	unsigned int		hash;
	struct mat_shader_entry_s *next;
} mat_shader_entry_t;

typedef struct mat_shader_warn_s
{
	char *token;
} mat_shader_warn_t;

static mat_shader_entry_t *mat_shader_hash[MAT_SHADER_HASH_SIZE];
static shader_material_t **mat_shader_list;
static mat_shader_warn_t *mat_shader_warned;
static qboolean mat_shader_loaded;

cvar_t r_shaders = { "r_shaders", "1", CVAR_ARCHIVE };
cvar_t r_shader_debug = { "r_shader_debug", "0", CVAR_ARCHIVE };
static cvar_t r_reloadshaders = { "r_reloadshaders", "0", CVAR_NONE };

char *Mat_Shader_DupString (const char *value)
{
	size_t len;
	char *out;

	if (!value)
		return NULL;

	len = strlen (value) + 1;
	out = (char *) Z_Malloc (len);
	memcpy (out, value, len);
	return out;
}

static void Mat_Shader_FreeMaterial (shader_material_t *material)
{
	size_t i;

	if (!material)
		return;

	for (i = 0; i < VEC_SIZE (material->stages); ++i)
	{
		mat_shader_stage_t *stage = &material->stages[i];
		if (stage->map_path)
			Z_Free (stage->map_path);
	}
	VEC_FREE (material->stages);

	if (material->editor_image)
		Z_Free (material->editor_image);
	if (material->name)
		Z_Free (material->name);
	if (material->source_file)
		Z_Free (material->source_file);

	Z_Free (material);
}

static void Mat_Shader_Reset (void)
{
	size_t i;

	for (i = 0; i < VEC_SIZE (mat_shader_list); ++i)
		Mat_Shader_FreeMaterial (mat_shader_list[i]);

	VEC_FREE (mat_shader_list);

	for (i = 0; i < countof (mat_shader_hash); ++i)
	{
		mat_shader_entry_t *entry = mat_shader_hash[i];
		while (entry)
		{
			mat_shader_entry_t *next = entry->next;
			Z_Free (entry);
			entry = next;
		}
		mat_shader_hash[i] = NULL;
	}

	for (i = 0; i < VEC_SIZE (mat_shader_warned); ++i)
	{
		if (mat_shader_warned[i].token)
			Z_Free (mat_shader_warned[i].token);
	}
	VEC_FREE (mat_shader_warned);
}

static unsigned int Mat_Shader_Hash (const char *name)
{
	return COM_HashString (name) % MAT_SHADER_HASH_SIZE;
}

static qboolean Mat_Shader_TokenWarned (const char *token)
{
	size_t i;

	for (i = 0; i < VEC_SIZE (mat_shader_warned); ++i)
	{
		if (!q_strcasecmp (mat_shader_warned[i].token, token))
			return true;
	}

	return false;
}

static void Mat_Shader_AddWarned (const char *token)
{
	mat_shader_warn_t warn;

	memset (&warn, 0, sizeof (warn));
	warn.token = Mat_Shader_DupString (token);
	VEC_PUSH (mat_shader_warned, warn);
}

static void Mat_Shader_WarnOnce (const char *warn_key, const char *token, const char *context)
{
	if (Mat_Shader_TokenWarned (warn_key))
		return;

	Mat_Shader_AddWarned (warn_key);
	Con_Warning ("MatShader: unknown token '%s' in %s\n", token, context ? context : "shader");
}

static void Mat_Shader_Register (shader_material_t *material)
{
	mat_shader_entry_t *entry;
	unsigned int bucket;

	entry = (mat_shader_entry_t *) Z_Malloc (sizeof (*entry));
	entry->material = material;
	entry->hash = Mat_Shader_Hash (material->name);
	bucket = entry->hash;
	entry->next = mat_shader_hash[bucket];
	mat_shader_hash[bucket] = entry;

	VEC_PUSH (mat_shader_list, material);
}

static void Mat_Shader_Unregister (const shader_material_t *material)
{
	unsigned int bucket;
	mat_shader_entry_t **entry;
	size_t i;

	if (!material)
		return;

	bucket = Mat_Shader_Hash (material->name);
	entry = &mat_shader_hash[bucket];
	while (*entry)
	{
		if ((*entry)->material == material)
		{
			mat_shader_entry_t *next = (*entry)->next;
			Z_Free (*entry);
			*entry = next;
			break;
		}
		entry = &(*entry)->next;
	}

	for (i = 0; i < VEC_SIZE (mat_shader_list); ++i)
	{
		if (mat_shader_list[i] == material)
		{
			mat_shader_list[i] = VEC_LAST (mat_shader_list);
			VEC_POP (mat_shader_list);
			break;
		}
	}
}

static const shader_material_t *Mat_Shader_FindInternal (const char *name)
{
	unsigned int bucket;
	mat_shader_entry_t *entry;

	if (!name || !name[0])
		return NULL;

	bucket = Mat_Shader_Hash (name);
	for (entry = mat_shader_hash[bucket]; entry; entry = entry->next)
	{
		if (!q_strcasecmp (entry->material->name, name))
			return entry->material;
	}

	return NULL;
}

static qboolean Mat_Shader_ReadFile (const char *path, const byte *data, size_t size, const char *label)
{
	char *buffer;
	int parsed;

	if (!data || size == 0)
		return false;

	buffer = (char *) Z_Malloc (size + 1);
	memcpy (buffer, data, size);
	buffer[size] = '\0';
	parsed = Mat_Shader_ParseFile (path, buffer, label);
	Z_Free (buffer);

	return parsed > 0;
}

static qboolean Mat_Shader_LoadFromDirectory (const searchpath_t *search)
{
	char script_dir[MAX_OSPATH];
	findfile_t *find;
	qboolean loaded_any = false;

	if ((size_t) q_snprintf (script_dir, sizeof (script_dir), "%s/scripts", search->filename) >= sizeof (script_dir))
		return false;

	for (find = Sys_FindFirst (script_dir, "shader"); find; find = Sys_FindNext (find))
	{
		char fullpath[MAX_OSPATH];
		char relpath[MAX_QPATH];
		byte *buffer;
		int handle;
		int size;

		if (find->attribs & FA_DIRECTORY)
			continue;

		q_snprintf (relpath, sizeof (relpath), "scripts/%s", find->name);
		q_snprintf (fullpath, sizeof (fullpath), "%s/%s", script_dir, find->name);

		size = (int) Sys_FileOpenRead (fullpath, &handle);
		if (size <= 0)
		{
			if (handle >= 0)
				Sys_FileClose (handle);
			continue;
		}

		buffer = (byte *) malloc (size + 1);
		if (!buffer)
		{
			Sys_FileClose (handle);
			continue;
		}

		if (Sys_FileRead (handle, buffer, size) != size)
		{
			free (buffer);
			Sys_FileClose (handle);
			continue;
		}

		Sys_FileClose (handle);
		buffer[size] = '\0';

		if (Mat_Shader_ReadFile (relpath, buffer, (size_t) size, relpath))
			loaded_any = true;

		free (buffer);
	}

	return loaded_any;
}

static qboolean Mat_Shader_LoadFromPack (const searchpath_t *search)
{
	int i;
	qboolean loaded_any = false;
	pack_t *pak = search->pack;

	for (i = 0; i < pak->numfiles; ++i)
	{
		const char *name = pak->files[i].name;
		size_t size = pak->files[i].filelen;
		byte *buffer = NULL;

		if (q_strncasecmp (name, "scripts/", 8))
			continue;
		if (q_strcasecmp (COM_FileGetExtension (name), "shader"))
			continue;

		if (pak->is_pk3)
		{
			size_t extracted_size = 0;
			void *extracted = mz_zip_reader_extract_to_heap ((mz_zip_archive *) pak->zip, pak->files[i].filepos, &extracted_size, 0);
			if (!extracted || extracted_size == 0)
				continue;
			buffer = (byte *) extracted;
			size = extracted_size;
		}
		else
		{
			buffer = (byte *) malloc (size + 1);
			if (!buffer)
				continue;
			Sys_FileSeek (pak->handle, pak->files[i].filepos);
			if (Sys_FileRead (pak->handle, buffer, (int) size) != (int) size)
			{
				free (buffer);
				continue;
			}
		}

		if (buffer)
		{
			buffer[size] = '\0';
			if (Mat_Shader_ReadFile (name, buffer, size, name))
				loaded_any = true;

			if (pak->is_pk3)
				MZ_FREE (buffer);
			else
				free (buffer);
		}
	}

	return loaded_any;
}

static void Mat_Shader_LoadAll (void)
{
	searchpath_t *search;
	searchpath_t **paths = NULL;
	size_t count = 0;
	size_t i;

	if (mat_shader_loaded && r_reloadshaders.value <= 0.f)
		return;

	Mat_Shader_Reset ();
	mat_shader_loaded = true;

	if (r_shaders.value <= 0.f)
		return;

	for (search = com_searchpaths; search; search = search->next)
	{
		VEC_PUSH (paths, search);
	}

	count = VEC_SIZE (paths);
	for (i = count; i > 0; --i)
	{
		search = paths[i - 1];
		if (*search->filename)
			Mat_Shader_LoadFromDirectory (search);
		else if (search->pack)
			Mat_Shader_LoadFromPack (search);
	}

	VEC_FREE (paths);
}

static void Mat_Shader_Reload_f (cvar_t *var)
{
	if (var->value <= 0.f)
		return;
	Con_Printf ("Reloading material shaders\n");
	r_reloadshaders.value = 0.f;
	Mat_Shader_LoadAll ();
}

static void Mat_Shader_List_f (void)
{
	size_t count = Mat_Shader_Count ();
	size_t limit = MAT_SHADER_LIST_LIMIT;
	size_t i;

	if (Cmd_Argc () > 1)
		limit = (size_t) q_max (0, Q_atoi (Cmd_Argv (1)));

	Con_Printf ("Loaded shaders: %zu\n", count);
	for (i = 0; i < count && i < limit; ++i)
	{
		const shader_material_t *material = Mat_Shader_GetByIndex (i);
		Con_Printf ("%s\n", material->name);
	}
}

static void Mat_Shader_Print_f (void)
{
	const shader_material_t *material;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("Usage: shaderprint <name>\n");
		return;
	}

	material = Mat_Shader_Find (Cmd_Argv (1));
	if (!material)
	{
		Con_Printf ("No shader named '%s'\n", Cmd_Argv (1));
		return;
	}

	Mat_Shader_Print (material);
}

void Mat_Shader_Init (void)
{
	Cvar_RegisterVariable (&r_shaders);
	Cvar_RegisterVariable (&r_shader_debug);
	Cvar_RegisterVariable (&r_reloadshaders);
	Cvar_SetCallback (&r_reloadshaders, Mat_Shader_Reload_f);

	Cmd_AddCommand ("shaderlist", Mat_Shader_List_f);
	Cmd_AddCommand ("shaderprint", Mat_Shader_Print_f);
}

void Mat_Shader_Shutdown (void)
{
	Mat_Shader_Reset ();
	mat_shader_loaded = false;
}

void Mat_Shader_Reload (void)
{
	Mat_Shader_LoadAll ();
}

size_t Mat_Shader_Count (void)
{
	Mat_Shader_LoadAll ();
	return VEC_SIZE (mat_shader_list);
}

const shader_material_t *Mat_Shader_GetByIndex (size_t index)
{
	Mat_Shader_LoadAll ();
	if (index >= VEC_SIZE (mat_shader_list))
		return NULL;
	return mat_shader_list[index];
}

void Mat_Shader_Canonicalize (const char *name, char *out, size_t out_size)
{
	size_t i;

	if (!out || out_size == 0)
		return;

	q_strlcpy (out, name ? name : "", out_size);
	for (i = 0; out[i]; ++i)
	{
		if (out[i] == '\\')
			out[i] = '/';
	}
	COM_StripExtension (out, out, out_size);
	q_strlwr (out);
}

const shader_material_t *Mat_Shader_Find (const char *name)
{
	char canonical[MAX_QPATH];

	if (!name || !name[0])
		return NULL;

	Mat_Shader_LoadAll ();
	Mat_Shader_Canonicalize (name, canonical, sizeof (canonical));
	return Mat_Shader_FindInternal (canonical);
}

const shader_material_t *Mat_Shader_FindForTextureName (const char *texname, const char *mapname)
{
	char candidate[MAX_QPATH];
	const shader_material_t *material;

	if (!texname || !texname[0])
		return NULL;

	if (mapname && mapname[0])
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s/%s", mapname, texname);
		material = Mat_Shader_Find (candidate);
		if (material)
			return material;
	}

	q_snprintf (candidate, sizeof (candidate), "textures/%s", texname);
	material = Mat_Shader_Find (candidate);
	if (material)
		return material;

	return Mat_Shader_Find (texname);
}

const char *Mat_Shader_GetStage0Map (const shader_material_t *material, const char *texname)
{
	if (!material || !material->stage0.map_path || !material->stage0.map_path[0])
		return NULL;

	if (texname && texname[0])
	{
		char canonical_tex[MAX_QPATH];
		Mat_Shader_Canonicalize (texname, canonical_tex, sizeof (canonical_tex));
		if (q_strcasecmp (material->name, canonical_tex))
			return NULL;
	}

	return material->stage0.map_path;
}

unsigned int Mat_Shader_GetTextureFlags (const shader_material_t *material)
{
	unsigned int flags = 0u;

	if (!material)
		return flags;

	if (material->render_flags & MAT_RENDER_NODRAW)
		flags |= MAT_SHADERFLAG_NODRAW;
	if (material->render_flags & MAT_RENDER_SKY)
		flags |= MAT_SHADERFLAG_SKY;
	if (material->render_flags & MAT_RENDER_TRANS)
		flags |= MAT_SHADERFLAG_TRANS;
	if (material->render_flags & MAT_RENDER_ALPHASHADOW)
		flags |= MAT_SHADERFLAG_ALPHASHADOW;
	if (material->render_flags & MAT_RENDER_FOG)
		flags |= MAT_SHADERFLAG_FOG;
	if (material->surfaceparms & MAT_SURFPARM_SOLID)
		flags |= MAT_SHADERFLAG_SOLID;
	if (material->surfaceparms & MAT_SURFPARM_NONSOLID)
		flags |= MAT_SHADERFLAG_NONSOLID;
	if (material->surfaceparms & MAT_SURFPARM_PLAYERCLIP)
		flags |= MAT_SHADERFLAG_PLAYERCLIP;
	if (material->surfaceparms & MAT_SURFPARM_MONSTERCLIP)
		flags |= MAT_SHADERFLAG_MONSTERCLIP;
	if (material->surfaceparms & MAT_SURFPARM_STONE)
		flags |= MAT_SHADERFLAG_STONE;
	if (material->emissive_enable)
		flags |= MAT_SHADERFLAG_EMISSIVE;
	if (material->bloom_enable)
		flags |= MAT_SHADERFLAG_BLOOM;
	if (material->godray_enable)
		flags |= MAT_SHADERFLAG_GODRAY;

	return flags;
}

void Mat_Shader_ApplyToTexture (texture_t *tex, const char *mapname)
{
	unsigned int flags;
	const shader_material_t *material = NULL;
	char candidate[MAX_QPATH];

	if (!tex)
		return;
	if (r_shaders.value <= 0.f)
		return;

	if (mapname && mapname[0])
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s/%s", mapname, tex->name);
		material = Mat_Shader_Find (candidate);
		if (material)
			tex->shader_map = Mat_Shader_GetStage0Map (material, candidate);
	}

	if (!material)
	{
		q_snprintf (candidate, sizeof (candidate), "textures/%s", tex->name);
		material = Mat_Shader_Find (candidate);
		if (material)
			tex->shader_map = Mat_Shader_GetStage0Map (material, candidate);
	}

	if (!material)
	{
		material = Mat_Shader_Find (tex->name);
		if (material)
			tex->shader_map = Mat_Shader_GetStage0Map (material, tex->name);
	}

	if (!material)
		return;

	flags = Mat_Shader_GetTextureFlags (material);

	tex->shader = material;
	tex->shader_flags = flags;

	if ((material->render_flags & MAT_RENDER_SKY) && tex->type != TEXTYPE_SKY)
		tex->type = TEXTYPE_SKY;
	if ((material->render_flags & MAT_RENDER_TRANS) && r_shader_debug.value >= 1.f)
		Con_DPrintf ("MatShader: surfaceparm trans on %s (TODO: blend path)\n", tex->name);

	if (r_shader_debug.value >= 1.f)
	{
		if (tex->shader_map)
			Con_Printf ("MatShader: %s overrides %s\n", tex->name, tex->shader_map);
	}
}

void Mat_Shader_Print (const shader_material_t *material)
{
	if (!material)
		return;

	Con_Printf ("Shader: %s\n", material->name);
	if (material->source_file && material->source_file[0])
		Con_Printf ("  source: %s\n", material->source_file);
	if (material->editor_image)
		Con_Printf ("  editor image: %s\n", material->editor_image);
	Con_Printf ("  surfaceparms: 0x%08x\n", material->surfaceparms);
	Con_Printf ("  render flags: 0x%08x\n", material->render_flags);
	Con_Printf ("  content flags: 0x%08x\n", material->content_flags);
	Con_Printf ("  emissive: %s (scale %.2f)\n", material->emissive_enable ? "on" : "off", material->emissive_scale);
	Con_Printf ("  bloom: %s (scale %.2f)\n", material->bloom_enable ? "on" : "off", material->bloom_scale);
	Con_Printf ("  godray: %s (scale %.2f)\n", material->godray_enable ? "on" : "off", material->godray_scale);
	if (material->stage0.map_path)
		Con_Printf ("  stage0 map: %s\n", material->stage0.map_path);
}

void Mat_Shader_Insert (shader_material_t *material)
{
	shader_material_t *owned;
	const shader_material_t *existing;

	if (!material || !material->name || !material->name[0])
		return;

	existing = Mat_Shader_FindInternal (material->name);
	if (existing)
	{
		Mat_Shader_Unregister (existing);
		Mat_Shader_FreeMaterial ((shader_material_t *) existing);
	}

	owned = (shader_material_t *) Z_Malloc (sizeof (*owned));
	memcpy (owned, material, sizeof (*owned));
	Mat_Shader_Register (owned);
}

void Mat_Shader_Remove (const shader_material_t *material)
{
	Mat_Shader_Unregister (material);
	Mat_Shader_FreeMaterial ((shader_material_t *) material);
}

void Mat_Shader_ReportUnknownToken (const char *token, const char *context)
{
	char warn_key[MAX_QPATH * 2];

	if (context && context[0])
		q_snprintf (warn_key, sizeof (warn_key), "%s::%s", context, token);
	else
		q_snprintf (warn_key, sizeof (warn_key), "%s", token);

	Mat_Shader_WarnOnce (warn_key, token, context);
}
