/*
Bootstrap module ownership:
- Owns startup basedir detection and single-argument startup patching.
- Owns migration/setup of user-dir data during startup probing.
Thread-safety assumptions:
- Called only during startup on main thread before FS runtime mutations.
*/

#include "fs_internal.h"

static qboolean COM_SetBaseDir (const char *path)
{
	const char pak0[] = "/" GAMENAME "/pak0.pak";
	char pakpath[countof (com_basedirs[0])];
	size_t i;

	i = strlen (path);
	if (i && (path[i - 1] == '/' || path[i - 1] == '\\'))
		--i;
	if (i + countof (pak0) > countof (pakpath))
		return false;

	memcpy (pakpath, path, i);
	memcpy (pakpath + i, pak0, sizeof (pak0));
	if (!Sys_FileExists (pakpath))
	{
		if (!COM_IsStandalone ())
			return false;
		Con_Warning ("Standalone mode enabled: using basedir without required game data in %s\n", GAMENAME);
	}

	memcpy (com_basedirs[0], path, i);
	com_basedirs[0][i] = 0;
	com_numbasedirs = 1;

	return true;
}

static void COM_AddBaseDir (const char *path)
{
	if (com_numbasedirs >= countof (com_basedirs))
		Sys_Error ("Too many basedirs (%d)", com_numbasedirs);
	if ((size_t) q_strlcpy (com_basedirs[com_numbasedirs++], path, sizeof (com_basedirs[0])) >= sizeof (com_basedirs[0]))
		Sys_Error ("Basedir too long (%d characters, max %d):\n%s\n", (int)strlen (path), (int)sizeof (com_basedirs[0]), path);
}

void COM_BootstrapAddBaseDir (const char *path)
{
	COM_AddBaseDir (path);
}

static void COM_MigrateNightdiveUserFiles (void)
{
	const char	*episodes[] = {"id1", "hipnotic", "rogue", "dopa", "mg1"};
	const char	*filetypes[] = {"cfg", "txt", "sav", "dem", "png", "jpg"};
	const char	*game, *ext;
	char		src[MAX_OSPATH];
	char		dst[MAX_OSPATH];
	char		*subdirs = NULL;
	findfile_t	*moditer, *fileiter;
	size_t		i;

	for (i = 0; i < countof (episodes); i++)
	{
		const char *episode = episodes[i];
		if ((size_t) q_snprintf (src, sizeof (src), "%s/%s/%s", com_nightdivedir, episode, CONFIG_NAME) >= sizeof (src))
			continue;
		if (!Sys_FileExists (src))
			continue;
		q_snprintf (src, sizeof (src), "%s/%s", com_nightdivedir, episode);
		q_snprintf (dst, sizeof (dst), "%s/%s", com_userprefdir, episode);
		Sys_rename (src, dst);
	}

	for (moditer = Sys_FindFirst (com_nightdivedir, NULL); moditer; moditer = Sys_FindNext (moditer))
	{
		char srcmod[MAX_OSPATH];
		char dstmod[MAX_OSPATH];
		char *cfg;

		if (!(moditer->attribs & FA_DIRECTORY))
			continue;
		if (!strcmp (moditer->name, ".") || !strcmp (moditer->name, ".."))
			continue;

		for (i = 0; i < countof (episodes); i++)
			if (!strcmp (moditer->name, episodes[i]))
				break;
		if (i != countof (episodes))
			continue;

		if ((size_t) q_snprintf (src, sizeof (src), "%s/%s/%s", com_nightdivedir, moditer->name, CONFIG_NAME) >= sizeof (src) ||
			(size_t) q_snprintf (dst, sizeof (dst), "%s/%s/%s", com_userprefdir, moditer->name, CONFIG_NAME) >= sizeof (dst))
			continue;
		cfg = (char *) COM_LoadMallocFile_TextMode_OSPath (src, NULL);
		if (!cfg)
			continue;

		COM_WriteFile_OSPath (dst, cfg, strlen (cfg));
		free (cfg);
		Sys_remove (src);

		q_snprintf (srcmod, sizeof (srcmod), "%s/%s", com_nightdivedir, moditer->name);
		q_snprintf (dstmod, sizeof (dstmod), "%s/%s", com_userprefdir, moditer->name);
		for (fileiter = Sys_FindFirst (srcmod, NULL); fileiter; fileiter = Sys_FindNext (fileiter))
		{
			if (fileiter->attribs & FA_DIRECTORY)
				continue;

			ext = COM_FileGetExtension (fileiter->name);
			for (i = 0; i < countof (filetypes); i++)
				if (!q_strcasecmp (ext, filetypes[i]))
					break;
			if (i == countof (filetypes))
				continue;

			if ((size_t) q_snprintf (src, sizeof (src), "%s/%s", srcmod, fileiter->name) < sizeof (src) &&
				(size_t) q_snprintf (dst, sizeof (dst), "%s/%s", dstmod, fileiter->name) < sizeof (dst))
			{
				Sys_rename (src, dst);
			}
		}

		Vec_Append ((void **)&subdirs, 1, moditer->name, strlen (moditer->name) + 1);
	}

	VEC_PUSH (subdirs, '\0');

	for (game = subdirs; *game; game += strlen (game) + 1)
	{
		q_snprintf (src, sizeof (src), "%s/%s", com_nightdivedir, game);
		Sys_remove (game);
	}

	VEC_FREE (subdirs);
}

const char *COM_BootstrapSetBaseDirRec (const char *start)
{
	char	buf[MAX_OSPATH];
	size_t	i, len;

	q_strlcpy (buf, start, sizeof (buf));
	len = strlen (start);

	for (i = len - 1; i > 1; i--)
	{
		if (Sys_IsPathSep (buf[i]))
		{
			buf[i] = '\0';
			if (COM_SetBaseDir (buf))
				return start + i + 1;
		}
	}

	return NULL;
}

static const char *COM_MakeRelative (const char *basepath, const char *fullpath)
{
	for (; *basepath && *fullpath; ++basepath, ++fullpath)
	{
		if (Sys_IsPathSep (*basepath) != Sys_IsPathSep (*fullpath))
			return NULL;
		if (*basepath != *fullpath)
			return NULL;
	}

	while (Sys_IsPathSep (*fullpath))
		++fullpath;

	return fullpath;
}

qboolean COM_BootstrapPatchCmdLine (const char *fullpath)
{
	static char	game[MAX_QPATH];
	char		qpath[MAX_QPATH];
	char		printpath[MAX_OSPATH];
	const char	*relpath;
	const char	*sep;
	int			type;
	int			i;

	type = Sys_FileType (fullpath);
	if (type == FS_ENT_NONE)
	{
		UTF8_ToQuake (printpath, sizeof (printpath), fullpath);
		Con_SafePrintf ("\"%s\" does not exist\n", printpath);
		return false;
	}

	relpath = NULL;
	for (i = 0; i < com_numbasedirs; i++)
	{
		relpath = COM_MakeRelative (com_basedirs[i], fullpath);
		if (relpath)
			break;
	}
	if (!relpath)
	{
		UTF8_ToQuake (printpath, sizeof (printpath), fullpath);
		Con_SafePrintf ("\"%s\" does not belong to an existing Quake installation\n", printpath);
		return false;
	}

	sep = COM_FirstPathSep (relpath);
	if ((uintptr_t)(sep - relpath) >= sizeof (game))
	{
		UTF8_ToQuake (printpath, sizeof (printpath), relpath);
		Con_SafePrintf ("\"%s\" is too long\n", printpath);
		return false;
	}

	UTF8_ToQuake (printpath, sizeof (printpath), relpath);

	if (*sep)
	{
		Q_strncpy (game, relpath, (int)(sep - relpath));
		COM_AddArg ("-game");
		COM_AddArg (game);
		relpath = sep + 1;
	}
	else if (type == FS_ENT_DIRECTORY)
	{
		COM_AddArg ("-game");
		COM_AddArg (relpath);
		return true;
	}
	else
	{
		game[0] = '\0';
	}

	q_strlcpy (qpath, relpath, sizeof (qpath));
	COM_NormalizePath (qpath);

	switch (type)
	{
	case FS_ENT_DIRECTORY:
		if (qpath[0])
		{
			if (q_strcasecmp (qpath, "maps") == 0)
			{
				Cbuf_AddText ("menu_maps\n");
				return true;
			}
			UTF8_ToQuake (printpath, sizeof (printpath), qpath);
			Con_SafePrintf ("\x02subdir \"%s\" ignored\n", printpath);
		}
		return true;

	case FS_ENT_FILE:
		{
			const char *ext = COM_FileGetExtension (qpath);

			if (q_strcasecmp (ext, "bsp") == 0)
			{
				if (!game[0])
				{
					Con_SafePrintf ("Map \"%s\" not in a mod dir, ignoring.\n", printpath);
					return false;
				}
				if (q_strncasecmp (qpath, "maps/", 5) != 0)
				{
					Con_SafePrintf ("Map \"%s\" not in the \"maps\" dir, ignoring.\n", printpath);
					return false;
				}
				memmove (qpath, qpath + 5, strlen (qpath + 5) + 1);
				Cbuf_AddText (va ("menu_maps \"%s\"\n", qpath));
				return true;
			}

			if (q_strcasecmp (ext, "sav") == 0)
			{
				const char *kex = game[0] ? "" : "kex";
				Cbuf_AddText (va ("load \"%s\" %s\n", qpath, kex));
				return true;
			}

			if (q_strcasecmp (ext, "dem") == 0)
			{
				if (!game[0])
				{
					Con_SafePrintf ("Demo \"%s\" not in a mod dir, ignoring.\n", printpath);
					return false;
				}
				Cbuf_AddText (va ("playdemo \"%s\"\n", qpath));
				return true;
			}

			break;
		}

	default:
		break;
	}

	if (!game[0])
		Con_SafePrintf ("File \"%s\" not in a mod dir, ignoring.\n", printpath);
	else
		Con_SafePrintf ("Unsupported file type \"%s\", ignoring.\n", printpath);

	return false;
}

void COM_BootstrapInitBaseDir (void)
{
	steamgame_t steamquake = {0};
	char path[MAX_OSPATH];
	char original[MAX_OSPATH] = {0};
	char remastered[MAX_OSPATH] = {0};
	int i, steam = 0, gog = 0, egs = 0;

	i = COM_CheckParm ("-basedir");
	if (i)
	{
		const char *dir;
		if (i >= com_argc - 1)
			Sys_Error (
				"Please specify a valid Quake directory after -basedir\n"
				"(one that has an " GAMENAME " subdirectory containing pak0.pak)\n"
			);

		dir = com_argv[++i];
		if (!COM_SetBaseDir (dir))
			Sys_Error (
				"The specified -basedir is not a valid Quake directory:\n"
				"%s\n"
				"doesn't have an " GAMENAME " subdirectory containing pak0.pak.\n",
				dir
			);

		for (;;)
		{
			i = COM_CheckParmNext (i, "-basedir");
			if (!i)
				break;
			if (i >= com_argc - 1)
				Sys_Error ("Please specify a directory after -basedir\n");
			COM_AddBaseDir (com_argv[++i]);
		}

		return;
	}

	steam = COM_CheckParm ("-steam");
	if (steam)
		goto try_steam;
	gog = COM_CheckParm ("-gog");
	if (gog)
		goto try_gog;
	egs = COM_CheckParm ("-egs");
	if (!egs)
		egs = COM_CheckParm ("-epic");
	if (egs)
		goto try_egs;

	if (COM_SetBaseDir (host_parms->basedir))
		return;
	if (COM_BootstrapSetBaseDirRec (host_parms->basedir))
		return;

	if (host_parms->userdir && host_parms->userdir != host_parms->basedir && COM_SetBaseDir (host_parms->userdir))
	{
		host_parms->basedir = host_parms->userdir;
		return;
	}

	if (!COM_CheckParm ("-nosteam"))
	{
	try_steam:
		if (Steam_FindGame (&steamquake, QUAKE_STEAM_APPID) &&
			Steam_ResolvePath (original, sizeof (original), &steamquake))
		{
			if ((size_t) q_snprintf (remastered, sizeof (remastered), "%s/rerelease", original) >= sizeof (remastered))
				remastered[0] = '\0';
			else if (!Sys_GetSteamQuakeUserDir (com_nightdivedir, sizeof (com_nightdivedir), steamquake.library))
				com_nightdivedir[0] = '\0';
		}
		else
		{
			memset (&steamquake, 0, sizeof (steamquake));
		}
		if (steam)
			goto storesetup;
	}

	if (!COM_CheckParm ("-nogog"))
	{
	try_gog:
		if (!original[0] && !Sys_GetGOGQuakeDir (original, sizeof (original)))
			original[0] = '\0';
		if (!remastered[0])
		{
			if (Sys_GetGOGQuakeEnhancedDir (remastered, sizeof (remastered)))
			{
				if (!com_nightdivedir[0] && !Sys_GetGOGQuakeEnhancedUserDir (com_nightdivedir, sizeof (com_nightdivedir)))
					com_nightdivedir[0] = '\0';
			}
			else
				remastered[0] = '\0';
		}
		if (gog)
			goto storesetup;
	}

	if (!COM_CheckParm ("-noegs") && !COM_CheckParm ("-noepic"))
	{
	try_egs:
		if (!remastered[0])
		{
			if (EGS_FindGame (remastered, sizeof (remastered), QUAKE_EGS_NAMESPACE, QUAKE_EGS_ITEM_ID, QUAKE_EGS_APP_NAME))
			{
				if (!Sys_GetGOGQuakeEnhancedUserDir (com_nightdivedir, sizeof (com_nightdivedir)))
					com_nightdivedir[0] = '\0';
			}
			else
				remastered[0] = '\0';
		}
		if (egs)
			goto storesetup;
	}

storesetup:
	if (original[0] || remastered[0])
	{
		quakeflavor_t flavor;
		if (original[0] && remastered[0])
		{
			if (COM_CheckParm ("-prefremaster") || COM_CheckParm ("-remaster") || COM_CheckParm ("-remastered"))
				flavor = QUAKE_FLAVOR_REMASTERED;
			else if (COM_CheckParm ("-preforiginal") || COM_CheckParm ("-original"))
				flavor = QUAKE_FLAVOR_ORIGINAL;
			else
				flavor = ChooseQuakeFlavor ();
		}
		else
			flavor = remastered[0] ? QUAKE_FLAVOR_REMASTERED : QUAKE_FLAVOR_ORIGINAL;
		q_strlcpy (path, flavor == QUAKE_FLAVOR_REMASTERED ? remastered : original, sizeof (path));

		if (steamquake.appid)
			Steam_Init (&steamquake);

		if (COM_SetBaseDir (path))
		{
			if (!Sys_GetAltUserPrefDir (flavor == QUAKE_FLAVOR_REMASTERED, com_userprefdir, sizeof (com_userprefdir)))
				Sys_Error ("Couldn't set up settings dir");

			if (flavor == QUAKE_FLAVOR_REMASTERED)
			{
				if (com_nightdivedir[0])
				{
					COM_MigrateNightdiveUserFiles ();
					COM_AddBaseDir (com_nightdivedir);
				}
				else
					Con_Warning ("Nightdive dir not found\n");
			}
			else
			{
				com_nightdivedir[0] = '\0';
			}

			host_parms->userdir = com_userprefdir;

			return;
		}
	}

	if (steam)
		Sys_Error ("Couldn't find Steam Quake");
	if (gog)
		Sys_Error ("Couldn't find GOG Quake");
	if (egs)
		Sys_Error ("Couldn't find Epic Games Store Quake");

	Sys_Error (
		"Couldn't determine where Quake is installed.\n"
		"Please use the -basedir option to specify a path\n"
		"(with an " GAMENAME " subdirectory containing pak0.pak)"
	);
}

void COM_BootstrapChooseStartArgFlavor (const char *startarg)
{
	steamgame_t steamquake;
	char steampath[MAX_OSPATH];
	char userdir[MAX_OSPATH];

	if (Sys_GetAltUserPrefDir (true, userdir, sizeof (userdir)) && COM_MakeRelative (userdir, startarg))
	{
		COM_AddArg ("-prefremaster");
		return;
	}

	if (Sys_GetAltUserPrefDir (false, userdir, sizeof (userdir)) && COM_MakeRelative (userdir, startarg))
	{
		COM_AddArg ("-preforiginal");
		return;
	}

	if (Steam_FindGame (&steamquake, QUAKE_STEAM_APPID) &&
		Steam_ResolvePath (steampath, sizeof (steampath), &steamquake) &&
		Sys_GetSteamQuakeUserDir (userdir, sizeof (userdir), steamquake.library) &&
		COM_MakeRelative (userdir, startarg))
	{
		COM_AddArg ("-prefremaster");
		return;
	}

	if (Sys_GetGOGQuakeEnhancedUserDir (userdir, sizeof (userdir)) && COM_MakeRelative (userdir, startarg))
	{
		COM_AddArg ("-prefremaster");
		return;
	}
}
