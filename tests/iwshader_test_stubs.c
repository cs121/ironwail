#include "quakedef.h"

#include <stdarg.h>
#include <stdio.h>

void Con_Printf(const char *fmt, ...)
{
    (void)fmt;
}

void Con_Warning(const char *fmt, ...)
{
    (void)fmt;
}

void Cvar_RegisterVariable(cvar_t *var)
{
    (void)var;
}

cmd_function_t *Cmd_AddCommand(const char *cmd_name, xcommand_t function)
{
    (void)cmd_name;
    (void)function;
    return NULL;
}

void Cmd_RemoveCommand(cmd_function_t *cmd)
{
    (void)cmd;
}

int Cmd_Argc(void)
{
    return 0;
}

const char *Cmd_Argv(int arg)
{
    (void)arg;
    return "";
}

FILE *Sys_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int Sys_FileType(const char *path)
{
    (void)path;
    return FS_ENT_FILE;
}

findfile_t *Sys_FindFirst(const char *dir, const char *ext)
{
    (void)dir;
    (void)ext;
    return NULL;
}

findfile_t *Sys_FindNext(findfile_t *find)
{
    (void)find;
    return NULL;
}

void Sys_FindClose(findfile_t *find)
{
    (void)find;
}

byte *COM_LoadMallocFile(const char *path, int *len)
{
    (void)path;
    if (len)
        *len = 0;
    return NULL;
}

byte *COM_LoadMallocFile_TextMode_OSPath(const char *path, int *len)
{
    (void)path;
    if (len)
        *len = 0;
    return NULL;
}
