/*
FS pack/PK3 module ownership:
- Owns loading/unloading of pack_t backends (PAK and PK3).
- Callers own searchpath_t nodes and must free them separately.
Thread-safety assumptions:
- Filesystem mutation is single-threaded during startup/game-dir changes.
- Read-side access is lock-free after startup; no concurrent mutation supported.
*/

#include "quakedef.h"
#include "fs_internal.h"
#include "miniz.h"
#include <limits.h>

#define MAX_FILES_IN_PACK	2048

typedef struct
{
	char	name[56];
	int		filepos, filelen;
} dpackfile_t;

typedef struct
{
	char	id[4];
	int		dirofs;
	int		dirlen;
} dpackheader_t;

// known pak0.pak metadata for optional integrity reporting
#define PAK0_COUNT		339	/* id1/pak0.pak - v1.0x */
#define PAK0_CRC_V100		13900	/* id1/pak0.pak - v1.00 */
#define PAK0_CRC_V101		62751	/* id1/pak0.pak - v1.01 */
#define PAK0_CRC_V106		32981	/* id1/pak0.pak - v1.06 */

extern cvar_t fs_integrity_report;

static void COM_ReportPak0Integrity (const char *packfile, const dpackfile_t *info, int dirlen, int numpackfiles)
{
	unsigned short crc;

	if (numpackfiles != PAK0_COUNT)
		Con_Warning ("Integrity report: %s has %i files (expected %i)\n", packfile, numpackfiles, PAK0_COUNT);

	crc = CRC_Block (info, dirlen);
	if (crc != PAK0_CRC_V106 && crc != PAK0_CRC_V101 && crc != PAK0_CRC_V100)
		Con_Warning ("Integrity report: %s directory CRC %u does not match known versions\n", packfile, crc);
}

pack_t *COM_LoadPackFile (const char *packfile)
{
	dpackheader_t	header;
	int		i;
	packfile_t	*newfiles;
	int		numpackfiles;
	pack_t		*pack;
	int		packhandle;
	dpackfile_t	info[MAX_FILES_IN_PACK];

	if (Sys_FileOpenRead (packfile, &packhandle) == -1)
		return NULL;

	if (Sys_FileRead(packhandle, &header, sizeof(header)) != (int) sizeof(header) ||
	    header.id[0] != 'P' || header.id[1] != 'A' || header.id[2] != 'C' || header.id[3] != 'K')
		Sys_Error ("%s is not a packfile", packfile);

	header.dirofs = LittleLong (header.dirofs);
	header.dirlen = LittleLong (header.dirlen);

	numpackfiles = header.dirlen / sizeof(dpackfile_t);

	if (header.dirlen < 0 || header.dirofs < 0)
	{
		Sys_Error ("Invalid packfile %s (dirlen: %i, dirofs: %i)",
					packfile, header.dirlen, header.dirofs);
	}
	if (!numpackfiles)
	{
		Sys_Printf ("WARNING: %s has no files, ignored\n", packfile);
		Sys_FileClose (packhandle);
		return NULL;
	}
	if (numpackfiles > MAX_FILES_IN_PACK)
		Sys_Error ("%s has %i files", packfile, numpackfiles);

	newfiles = (packfile_t *) Z_Malloc(numpackfiles * sizeof(packfile_t));

	Sys_FileSeek (packhandle, header.dirofs);
	if (Sys_FileRead(packhandle, info, header.dirlen) != header.dirlen)
		Sys_Error ("Error reading %s", packfile);

	if (fs_integrity_report.value && COM_IsPak0File (packfile))
		COM_ReportPak0Integrity (packfile, info, header.dirlen, numpackfiles);

	for (i = 0; i < numpackfiles; i++)
	{
		q_strlcpy (newfiles[i].name, info[i].name, sizeof(newfiles[i].name));
		newfiles[i].filepos = LittleLong(info[i].filepos);
		newfiles[i].filelen = LittleLong(info[i].filelen);
	}

	pack = (pack_t *) Z_Malloc (sizeof (pack_t));
	q_strlcpy (pack->filename, packfile, sizeof(pack->filename));
	pack->handle = packhandle;
	pack->numfiles = numpackfiles;
	pack->files = newfiles;
	pack->is_pk3 = false;
	pack->zip = NULL;

	return pack;
}

pack_t *COM_LoadZipFile (const char *zipfile)
{
	mz_zip_archive *zip;
	mz_zip_archive_file_stat file_stat;
	int numfiles, added, i;
	packfile_t *newfiles;
	pack_t *pack;

	zip = (mz_zip_archive *) Z_Malloc (sizeof (*zip));
	memset (zip, 0, sizeof (*zip));

	if (!mz_zip_reader_init_file (zip, zipfile, 0))
	{
		Z_Free (zip);
		return NULL;
	}

	numfiles = (int) mz_zip_reader_get_num_files (zip);
	if (numfiles <= 0)
	{
		mz_zip_reader_end (zip);
		Z_Free (zip);
		return NULL;
	}

	newfiles = (packfile_t *) Z_Malloc (numfiles * sizeof (packfile_t));

	for (i = 0, added = 0; i < numfiles; i++)
	{
		if (!mz_zip_reader_file_stat (zip, i, &file_stat))
			continue;
		if (file_stat.m_is_directory)
			continue;
		if (file_stat.m_uncomp_size > INT_MAX)
		{
			Con_Warning ("Ignoring oversized file %s in %s\n", file_stat.m_filename, zipfile);
			continue;
		}

		q_strlcpy (newfiles[added].name, file_stat.m_filename, sizeof (newfiles[added].name));
		newfiles[added].filepos = i;
		newfiles[added].filelen = (int) file_stat.m_uncomp_size;
		added++;
	}

	if (!added)
	{
		mz_zip_reader_end (zip);
		Z_Free (newfiles);
		Z_Free (zip);
		return NULL;
	}

	pack = (pack_t *) Z_Malloc (sizeof (pack_t));
	q_strlcpy (pack->filename, zipfile, sizeof (pack->filename));
	pack->handle = -1;
	pack->numfiles = added;
	pack->files = newfiles;
	pack->is_pk3 = true;
	pack->zip = zip;

	return pack;
}

qboolean COM_ExtractZipEntry (pack_t *pack, int file_index, void **out_data, size_t *out_size)
{
	mz_zip_archive *zip = (mz_zip_archive *) pack->zip;

	if (!zip)
		return false;

	*out_data = mz_zip_reader_extract_to_heap (zip, file_index, out_size, 0);
	return *out_data != NULL;
}

void COM_FreePack (pack_t *pack)
{
	if (!pack)
		return;

	if (pack->is_pk3)
	{
		if (pack->zip)
			mz_zip_reader_end ((mz_zip_archive *) pack->zip);
		Z_Free (pack->zip);
	}
	else if (pack->handle != -1)
	{
		Sys_FileClose (pack->handle);
	}

	Z_Free (pack->files);
	Z_Free (pack);
}

static int COM_Pk3Compare (const void *a, const void *b)
{
	const char *left = *(const char *const *) a;
	const char *right = *(const char *const *) b;

	return q_strcasecmp (left, right);
}

void COM_AddPk3Files (const char *gamedir, unsigned int path_id, qboolean append)
{
	char **pk3files = NULL;
	size_t numpk3 = 0, maxpk3 = 0;
	findfile_t *find;

	for (find = Sys_FindFirst (gamedir, NULL); find; find = Sys_FindNext (find))
	{
		const char *ext;

		if (find->attribs & FA_DIRECTORY)
			continue;

		ext = COM_FileGetExtension (find->name);
		if (!ext || q_strcasecmp (ext, "pk3"))
			continue;

		if (numpk3 == maxpk3)
		{
			maxpk3 = maxpk3 ? maxpk3 * 2 : 8;
			pk3files = (char **) (pk3files ? Z_Realloc (pk3files, maxpk3 * sizeof (char *)) : Z_Malloc (maxpk3 * sizeof (char *)));
		}

		pk3files[numpk3] = (char *) Z_Malloc (MAX_OSPATH);
		q_snprintf (pk3files[numpk3], MAX_OSPATH, "%s/%s", gamedir, find->name);
		numpk3++;
	}

	if (numpk3 > 1)
		qsort (pk3files, numpk3, sizeof (char *), COM_Pk3Compare);

	while (numpk3--)
	{
		pack_t *pak = COM_LoadZipFile (pk3files[numpk3]);

		if (pak)
		{
			searchpath_t *search = (searchpath_t *) Z_Malloc(sizeof(searchpath_t));
			search->path_id = path_id;
			search->pack = pak;
			COM_PushSearchPath (search, append);
			*COM_ModifiedFlag () = true;
		}

		Z_Free (pk3files[numpk3]);
	}

	if (pk3files)
		Z_Free (pk3files);
}
