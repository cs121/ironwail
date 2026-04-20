/*
FS searchpath module ownership:
- Owns com_searchpaths/com_base_searchpaths/file_from_pak state.
- Owns adding/removing searchpath_t nodes for game directory changes.
Thread-safety assumptions:
- Searchpath writes occur on main thread during startup/mod switches only.
- Readers may traverse without locks after mutation is quiescent.
*/

#include "fs_internal.h"

searchpath_t *com_searchpaths;
searchpath_t *com_base_searchpaths;
THREAD_LOCAL int file_from_pak;

searchpath_t *COM_GetSearchPaths (void)
{
	return com_searchpaths;
}

searchpath_t *COM_GetBaseSearchPaths (void)
{
	return com_base_searchpaths;
}

searchpath_t **COM_GetSearchPathsPointer (void)
{
	return &com_searchpaths;
}

void COM_SetSearchPaths (searchpath_t *search)
{
	com_searchpaths = search;
}

void COM_SetBaseSearchPaths (searchpath_t *search)
{
	com_base_searchpaths = search;
}

int COM_GetFileFromPak (void)
{
	return file_from_pak;
}

void COM_SetFileFromPak (int value)
{
	file_from_pak = value;
}

void COM_PushSearchPath (searchpath_t *search, qboolean append)
{
	searchpath_t *tail;

	if (!append)
	{
		search->next = com_searchpaths;
		com_searchpaths = search;
		return;
	}

	search->next = NULL;
	if (!com_searchpaths)
	{
		com_searchpaths = search;
		return;
	}

	tail = com_searchpaths;
	while (tail->next)
		tail = tail->next;
	tail->next = search;
}

void COM_AddEnginePak (unsigned int path_id, qboolean append)
{
	int i;
	char pakfile[MAX_OSPATH];
	pack_t *pak = NULL;
	qboolean modified = *COM_ModifiedFlag ();

	if (host_parms->exedir)
	{
		q_snprintf (pakfile, sizeof(pakfile), "%s/" ENGINE_PAK, host_parms->exedir);
		pak = COM_LoadPackFile (pakfile);
	}

	if (!pak)
	{
		q_snprintf (pakfile, sizeof(pakfile), "%s/" ENGINE_PAK, host_parms->basedir);
		pak = COM_LoadPackFile (pakfile);
	}

	if (!pak)
	{
		for (i = 0; i < com_numbasedirs; i++)
		{
			q_snprintf (pakfile, sizeof(pakfile), "%s/" ENGINE_PAK, com_basedirs[i]);
			pak = COM_LoadPackFile (pakfile);
			if (pak)
				break;
		}
	}

	if (pak)
	{
		searchpath_t *search = (searchpath_t *) Z_Malloc(sizeof(searchpath_t));
		search->pack = pak;
		search->path_id = path_id;
		COM_PushSearchPath (search, append);
	}

	*COM_ModifiedFlag () = modified;
}

void COM_AddGameDirectory (const char *dir)
{
	const char *base;
	int i, j;
	unsigned int path_id;
	qboolean append_paths;
	searchpath_t *search;
	pack_t *pak;
	char pakfile[MAX_OSPATH];

	if (*com_gamenames)
		q_strlcat(com_gamenames, ";", sizeof(com_gamenames));
	q_strlcat(com_gamenames, dir, sizeof(com_gamenames));

	if (!q_strcasecmp(dir,"rogue")) {
		rogue = true;
		standard_quake = false;
	}
	if (!q_strcasecmp(dir,"hipnotic") || !q_strcasecmp(dir,"quoth")) {
		hipnotic = true;
		standard_quake = false;
	}
	if (!q_strcasecmp(dir,"q64")) {
		quake64 = true;
	}

	if (com_searchpaths)
		path_id = com_searchpaths->path_id << 1;
	else
		path_id = 1U;

	append_paths = (COM_IsStandalone () && !q_strcasecmp (dir, GAMENAME));

	for (j = 0; j < com_numbasedirs; j++)
	{
		base = com_basedirs[j];
		q_snprintf (com_gamedir, sizeof (com_gamedir), "%s/%s", base, dir);

		search = (searchpath_t *) Z_Malloc(sizeof(searchpath_t));
		search->path_id = path_id;
		q_strlcpy (search->filename, com_gamedir, sizeof(search->filename));
		COM_PushSearchPath (search, append_paths);

		for (i = 0; ; i++)
		{
			q_snprintf (pakfile, sizeof(pakfile), "%s/pak%i.pak", com_gamedir, i);
			pak = COM_LoadPackFile (pakfile);
			if (!pak)
				break;

			search = (searchpath_t *) Z_Malloc(sizeof(searchpath_t));
			search->path_id = path_id;
			search->pack = pak;
			COM_PushSearchPath (search, append_paths);

			if (i == 0 && j == 0 && path_id == 1u && !fitzmode)
				COM_AddEnginePak (path_id, append_paths);
		}

		COM_AddPk3Files (com_gamedir, path_id, append_paths);
	}
}

void COM_ResetGameDirectories(const char *newgamedirs)
{
	const char *newpath, *path;
	searchpath_t *search;

	while (com_searchpaths != com_base_searchpaths)
	{
		if (com_searchpaths->pack)
			COM_FreePack (com_searchpaths->pack);

		search = com_searchpaths->next;
		Z_Free (com_searchpaths);
		com_searchpaths = search;
	}
	hipnotic = false;
	rogue = false;
	quake64 = false;
	standard_quake = true;
	*com_gamenames = 0;
	q_strlcpy (com_gamedir, va("%s/%s", com_basedirs[com_numbasedirs-1], GAMENAME), sizeof(com_gamedir));

	for(newpath = newgamedirs; newpath && *newpath; )
	{
		char *e = strchr(newpath, ';');
		if (e)
			*e++ = 0;

		if (!q_strcasecmp(GAMENAME, newpath))
			path = NULL;
		else for (path = newgamedirs; path < newpath; path += strlen(path)+1)
		{
			if (!q_strcasecmp(path, newpath))
				break;
		}

		if (path == newpath)
			COM_AddGameDirectory(newpath);
		newpath = e;
	}
}
