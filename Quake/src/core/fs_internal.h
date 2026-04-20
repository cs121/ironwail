#ifndef FS_INTERNAL_H
#define FS_INTERNAL_H

#include "quakedef.h"

qboolean COM_IsPak0File (const char *packfile);
qboolean COM_IsStandalone (void);
qboolean *COM_ModifiedFlag (void);

pack_t *COM_LoadPackFile (const char *packfile);
pack_t *COM_LoadZipFile (const char *zipfile);
qboolean COM_ExtractZipEntry (pack_t *pack, int file_index, void **out_data, size_t *out_size);
void COM_FreePack (pack_t *pack);
void COM_AddPk3Files (const char *gamedir, unsigned int path_id, qboolean append);

void COM_BootstrapInitBaseDir (void);
const char *COM_BootstrapSetBaseDirRec (const char *start);
void COM_BootstrapAddBaseDir (const char *path);
void COM_BootstrapChooseStartArgFlavor (const char *startarg);
qboolean COM_BootstrapPatchCmdLine (const char *fullpath);

#endif
