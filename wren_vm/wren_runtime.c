#include "quakedef.h"
#include "common.h"
#include "server.h"
#include "wren_runtime.h"
#include "wren_bindings.h"
#include "wren/include/wren.h"

#define WRENV_MODULE_BOOTSTRAP "q/game"

static struct
{
    qboolean initialized;
    qboolean enabled;
    qboolean strict;
    qboolean init_called;
    int current_skill;
    int server_flags;
    qboolean demo_playback;
    char current_map[MAX_QPATH];
    char active_mod[MAX_OSPATH];
    const char *current_entity_lump;
    WrenVM *vm;
    WrenHandle *game_class;
    WrenHandle *call_init;
    WrenHandle *call_spawn_entities;
    WrenHandle *call_start_frame;
    WrenHandle *call_client_connect;
    WrenHandle *call_client_disconnect;
    WrenHandle *call_on_save;
    WrenHandle *call_on_load;
} wren_state;

static struct
{
    qboolean had_error;
    qboolean not_implemented;
    qboolean missing_method;
} last_error_state;

#define WRENV_RESOLVE_BUFFER_COUNT 8

static void wrenvm_module_dir_from_importer(const char *importer, char *out, size_t size)
{
    if (!importer || importer[0] == '\0' || strncmp(importer, "q/", 2) != 0)
    {
        q_strlcpy(out, "q", size);
        return;
    }

    q_strlcpy(out, importer, size);

    char *slash = strrchr(out, '/');
    if (!slash || slash == out)
    {
        q_strlcpy(out, "q", size);
        return;
    }

    *slash = '\0';
}

static void wrenvm_module_parent(char *path, size_t size)
{
    if (!path || !path[0])
    {
        q_strlcpy(path, "q", size);
        return;
    }

    char *slash = strrchr(path, '/');
    if (!slash)
    {
        q_strlcpy(path, "q", size);
        return;
    }

    if (slash == path)
    {
        path[1] = '\0';
    }
    else
    {
        *slash = '\0';
    }

    if (!path[0])
        q_strlcpy(path, "q", size);
}

static void wrenvm_dispose_handles(void)
{
    if (!wren_state.vm)
        return;

    if (wren_state.call_init)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_init);
        wren_state.call_init = NULL;
    }
    if (wren_state.call_spawn_entities)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_spawn_entities);
        wren_state.call_spawn_entities = NULL;
    }
    if (wren_state.call_start_frame)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_start_frame);
        wren_state.call_start_frame = NULL;
    }
    if (wren_state.call_client_connect)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_client_connect);
        wren_state.call_client_connect = NULL;
    }
    if (wren_state.call_client_disconnect)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_client_disconnect);
        wren_state.call_client_disconnect = NULL;
    }
    if (wren_state.call_on_save)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_on_save);
        wren_state.call_on_save = NULL;
    }
    if (wren_state.call_on_load)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.call_on_load);
        wren_state.call_on_load = NULL;
    }
    if (wren_state.game_class)
    {
        wrenReleaseHandle(wren_state.vm, wren_state.game_class);
        wren_state.game_class = NULL;
    }
}

static void wrenvm_close_vm(void)
{
    if (!wren_state.vm)
        return;

    wrenvm_dispose_handles();
    WRENBindings_Shutdown();
    wrenFreeVM(wren_state.vm);
    wren_state.vm = NULL;
}

static void wrenvm_write_fn(WrenVM *vm, const char *text)
{
    (void)vm;
    Con_Printf("%s", text);
}

static void wrenvm_error_fn(WrenVM *vm, WrenErrorType type, const char *module, int line, const char *message)
{
    (void)vm;
    if (type == WREN_ERROR_COMPILE)
    {
        Con_Printf("[wren] compile error in %s:%d: %s\n", module ? module : "<script>", line, message ? message : "<no message>");
        last_error_state.had_error = true;
        return;
    }

    if (type == WREN_ERROR_RUNTIME)
    {
        last_error_state.had_error = true;
        if (message && !q_strcasecmp(message, "NotImplemented"))
        {
            last_error_state.not_implemented = true;
            return;
        }
        if (message && strstr(message, "does not implement"))
            last_error_state.missing_method = true;

        Con_Printf("[wren] runtime error in %s:%d: %s\n", module ? module : "<runtime>", line, message ? message : "<no message>");
        return;
    }

    if (type == WREN_ERROR_STACK_TRACE)
    {
        Con_Printf("[wren] %s:%d: %s\n", module ? module : "<stack>", line, message ? message : "<no message>");
    }
}

static const char *wrenvm_resolve_module(WrenVM *vm, const char *importer, const char *name)
{
    (void)vm;

    if (!name || !name[0])
        return name;

    if (!strncmp(name, "q/", 2))
        return name;

    static char buffers[WRENV_RESOLVE_BUFFER_COUNT][MAX_QPATH];
    static int next_buffer = 0;

    char *resolved = buffers[next_buffer];
    next_buffer = (next_buffer + 1) % WRENV_RESOLVE_BUFFER_COUNT;

    if (name[0] != '.')
    {
        q_snprintf(resolved, MAX_QPATH, "q/%s", name);
        return resolved;
    }

    char base[MAX_QPATH];
    wrenvm_module_dir_from_importer(importer, base, sizeof(base));

    const char *cursor = name;
    while (cursor[0] == '.')
    {
        if (!strncmp(cursor, "../", 3))
        {
            wrenvm_module_parent(base, sizeof(base));
            cursor += 3;
            continue;
        }
        if (!strncmp(cursor, "./", 2))
        {
            cursor += 2;
            continue;
        }
        break;
    }

    if (!cursor[0])
    {
        q_strlcpy(resolved, base, MAX_QPATH);
        return resolved;
    }

    if (!strncmp(cursor, "/", 1))
        ++cursor;

    if (base[0] && q_strcasecmp(base, "q") != 0)
        q_snprintf(resolved, MAX_QPATH, "%s/%s", base, cursor);
    else
        q_snprintf(resolved, MAX_QPATH, "q/%s", cursor);

    return resolved;
}

static void wrenvm_release_module(WrenVM *vm, const char *name, WrenLoadModuleResult result)
{
    (void)vm;
    (void)name;
    if (result.source)
        free((void *)result.source);
}

static WrenLoadModuleResult wrenvm_load_module(WrenVM *vm, const char *name)
{
    (void)vm;
    WrenLoadModuleResult result;
    memset(&result, 0, sizeof(result));

    if (!name)
        return result;

    if (!strncmp(name, "q/", 2))
    {
        const char *local = name + 2;
        char script_path[MAX_QPATH];
        q_snprintf(script_path, sizeof(script_path), "scripts/%s.wren", local);
        byte *data = COM_LoadMallocFile(script_path, NULL);
        if (!data)
        {
            Con_DPrintf("[wren] module '%s' not found (path %s)\n", name, script_path);
            return result;
        }
        result.source = (const char *)data;
        result.userData = data;
        result.onComplete = wrenvm_release_module;
        return result;
    }

    return result;
}

static qboolean wrenvm_import_module(const char *module_name)
{
    char qualified_name[MAX_QPATH];
    char import_source[MAX_QPATH + 16];

    if (!module_name || !module_name[0])
        return true;

    q_snprintf(qualified_name, sizeof(qualified_name), "q/%s", module_name);

    if (wrenHasModule(wren_state.vm, qualified_name))
        return true;

    q_snprintf(import_source, sizeof(import_source), "import \"%s\"\n", qualified_name);
    if (wrenInterpret(wren_state.vm, "__bootstrap", import_source) == WREN_RESULT_SUCCESS)
        return true;

    Con_Printf("[wren] failed to load module '%s'\n", qualified_name);
    return false;
}

static qboolean wrenvm_import_scripts_from_directory(const char *directory, const char *module_prefix)
{
    findfile_t *find = Sys_FindFirst(directory, NULL);
    if (!find)
        return true;

    do
    {
        char full_path[MAX_OSPATH];

        if (!q_strcasecmp(find->name, ".") || !q_strcasecmp(find->name, ".."))
            continue;

        q_snprintf(full_path, sizeof(full_path), "%s/%s", directory, find->name);

        if (find->attribs & FA_DIRECTORY)
        {
            char next_prefix[MAX_QPATH];

            if (module_prefix && module_prefix[0])
                q_snprintf(next_prefix, sizeof(next_prefix), "%s/%s", module_prefix, find->name);
            else
                q_strlcpy(next_prefix, find->name, sizeof(next_prefix));

            if (!wrenvm_import_scripts_from_directory(full_path, next_prefix))
            {
                Sys_FindClose(find);
                return false;
            }
        }
        else
        {
            const char *ext = COM_FileGetExtension(find->name);
            if (ext[0] && !q_strcasecmp(ext, "wren"))
            {
                char module_name[MAX_QPATH];
                char base_name[MAX_QPATH];

                COM_StripExtension(find->name, base_name, sizeof(base_name));
                if (module_prefix && module_prefix[0])
                    q_snprintf(module_name, sizeof(module_name), "%s/%s", module_prefix, base_name);
                else
                    q_strlcpy(module_name, base_name, sizeof(module_name));

                if (!wrenvm_import_module(module_name))
                {
                    Sys_FindClose(find);
                    return false;
                }
            }
        }
    } while ((find = Sys_FindNext(find)) != NULL);

    return true;
}

static qboolean wrenvm_import_scripts_from_pack(pack_t *pak)
{
    int i;

    if (!pak)
        return true;

    for (i = 0; i < pak->numfiles; i++)
    {
        const char *entry = pak->files[i].name;
        const char *relative;
        char module_name[MAX_QPATH];

        if (q_strncasecmp(entry, "scripts/", 8) != 0)
            continue;

        relative = entry + 8;
        if (!relative[0])
            continue;

        if (!q_strcasecmp(relative, ".") || !q_strcasecmp(relative, ".."))
            continue;

        if (relative[strlen(relative) - 1] == '/')
            continue;

        if (q_strcasecmp(COM_FileGetExtension(relative), "wren") != 0)
            continue;

        q_strlcpy(module_name, relative, sizeof(module_name));
        COM_StripExtension(module_name, module_name, sizeof(module_name));
        if (!module_name[0])
            continue;

        if (!wrenvm_import_module(module_name))
            return false;
    }

    return true;
}

static qboolean wrenvm_import_scripts_from_searchpath(searchpath_t *search)
{
    if (!search)
        return true;

    if (search->pack)
        return wrenvm_import_scripts_from_pack(search->pack);

    if (!search->filename[0])
        return true;

    char scripts_dir[MAX_OSPATH];
    q_snprintf(scripts_dir, sizeof(scripts_dir), "%s/scripts", search->filename);

    if (!(Sys_FileType(scripts_dir) & FS_ENT_DIRECTORY))
        return true;

    return wrenvm_import_scripts_from_directory(scripts_dir, "");
}

static qboolean wrenvm_import_all_scripts(void)
{
    searchpath_t *search;

    for (search = com_searchpaths; search; search = search->next)
    {
        if (!wrenvm_import_scripts_from_searchpath(search))
            return false;
    }

    return true;
}

static qboolean wrenvm_create_vm(void)
{
    WrenConfiguration config;
    wrenInitConfiguration(&config);
    config.writeFn = wrenvm_write_fn;
    config.errorFn = wrenvm_error_fn;
    config.resolveModuleFn = wrenvm_resolve_module;
    config.loadModuleFn = wrenvm_load_module;

    WRENBindings_Configure(&config);

    wren_state.vm = wrenNewVM(&config);
    if (!wren_state.vm)
        return false;

    WRENBindings_BindRuntime(wren_state.vm);
    return true;
}

static qboolean wrenvm_load_game_module(void)
{
    if (!wren_state.vm)
        return false;

    if (wrenInterpret(wren_state.vm, "__bootstrap", "import \"" WRENV_MODULE_BOOTSTRAP "\"\n") != WREN_RESULT_SUCCESS)
    {
        Con_Printf("[wren] failed to load module '%s'\n", WRENV_MODULE_BOOTSTRAP);
        return false;
    }

    if (!wrenHasVariable(wren_state.vm, WRENV_MODULE_BOOTSTRAP, "Game"))
    {
        Con_Printf("[wren] module '%s' does not define Game class\n", WRENV_MODULE_BOOTSTRAP);
        return false;
    }

    wrenEnsureSlots(wren_state.vm, 1);
    wrenGetVariable(wren_state.vm, WRENV_MODULE_BOOTSTRAP, "Game", 0);
    wren_state.game_class = wrenGetSlotHandle(wren_state.vm, 0);
    wren_state.call_init = wrenMakeCallHandle(wren_state.vm, "init()");
    wren_state.call_spawn_entities = wrenMakeCallHandle(wren_state.vm, "spawnEntities()");
    wren_state.call_start_frame = wrenMakeCallHandle(wren_state.vm, "startFrame(_)");
    wren_state.call_client_connect = wrenMakeCallHandle(wren_state.vm, "clientConnect(_)");
    wren_state.call_client_disconnect = wrenMakeCallHandle(wren_state.vm, "clientDisconnect(_)");
    wren_state.call_on_save = wrenMakeCallHandle(wren_state.vm, "onSave()");
    wren_state.call_on_load = wrenMakeCallHandle(wren_state.vm, "onLoad()");

    if (!wrenvm_import_all_scripts())
        return false;

    return true;
}

static qboolean wrenvm_ensure_vm(void)
{
    if (!wren_state.enabled)
        return false;

    if (!wren_state.vm)
    {
        if (!wrenvm_create_vm())
        {
            Con_Printf("[wren] failed to create VM\n");
            wren_state.enabled = false;
            return false;
        }

        if (!wrenvm_load_game_module())
        {
            Con_Printf("[wren] disabling Wren runtime (module load failed)\n");
            wrenvm_close_vm();
            wren_state.enabled = false;
            return false;
        }
    }

    return true;
}

static wrenvm_call_result_t wrenvm_call_handle(WrenHandle *handle, int num_slots, void (*prepare)(WrenVM *))
{
    if (!wren_state.vm || !wren_state.game_class || !handle)
        return WRENV_CALL_MISSING;

    wrenEnsureSlots(wren_state.vm, num_slots);
    wrenSetSlotHandle(wren_state.vm, 0, wren_state.game_class);
    if (prepare)
        prepare(wren_state.vm);

    last_error_state.had_error = false;
    last_error_state.not_implemented = false;
    last_error_state.missing_method = false;

    WrenInterpretResult result = wrenCall(wren_state.vm, handle);
    if (result == WREN_RESULT_SUCCESS)
        return WRENV_CALL_OK;

    if (last_error_state.not_implemented)
        return WRENV_CALL_FALLBACK;
    if (last_error_state.missing_method)
        return WRENV_CALL_MISSING;

    return WRENV_CALL_ERROR;
}

void WRENVM_InitSystem(void)
{
    memset(&wren_state, 0, sizeof(wren_state));
    memset(&last_error_state, 0, sizeof(last_error_state));

    if (COM_CheckParm("-nowren"))
    {
        wren_state.enabled = false;
        return;
    }

    wren_state.enabled = true;
    wren_state.strict = COM_CheckParm("-wrenstrict") ? true : false;
    wren_state.current_skill = (int)skill.value;
    wren_state.active_mod[0] = '\0';

    if (!wrenvm_ensure_vm())
        return;

    wren_state.initialized = true;
    if (com_gamedir[0])
        q_strlcpy(wren_state.active_mod, com_gamedir, sizeof(wren_state.active_mod));
    WRENVM_CallInit();
}

void WRENVM_ShutdownSystem(void)
{
    wrenvm_close_vm();
    memset(&wren_state, 0, sizeof(wren_state));
    memset(&last_error_state, 0, sizeof(last_error_state));
}

void WRENVM_ResetForNewServer(const char *mapname, const char *entity_lump)
{
    if (!wren_state.enabled)
        return;

    if (!wrenvm_ensure_vm())
        return;

    if (!wren_state.active_mod[0] && com_gamedir[0])
        q_strlcpy(wren_state.active_mod, com_gamedir, sizeof(wren_state.active_mod));

    q_strlcpy(wren_state.current_map, mapname ? mapname : "", sizeof(wren_state.current_map));
    wren_state.current_entity_lump = entity_lump;

    if (com_gamedir[0] && q_strcasecmp(wren_state.active_mod, com_gamedir))
    {
        wrenvm_close_vm();
        wren_state.init_called = false;
        q_strlcpy(wren_state.active_mod, com_gamedir, sizeof(wren_state.active_mod));
        if (!wrenvm_ensure_vm())
            return;
        WRENVM_CallInit();
    }
}

wrenvm_call_result_t WRENVM_CallInit(void)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    if (!wren_state.init_called)
    {
        wrenvm_call_result_t result = wrenvm_call_handle(wren_state.call_init, 1, NULL);
        if (result == WRENV_CALL_OK)
            wren_state.init_called = true;
        return result;
    }

    return WRENV_CALL_OK;
}

wrenvm_call_result_t WRENVM_CallSpawnEntities(void)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    return wrenvm_call_handle(wren_state.call_spawn_entities, 1, NULL);
}

static float last_frame_dt;

static void wrenvm_prepare_startframe(WrenVM *vm)
{
    wrenSetSlotDouble(vm, 1, last_frame_dt);
}

wrenvm_call_result_t WRENVM_CallStartFrame(float dt)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    last_frame_dt = dt;
    return wrenvm_call_handle(wren_state.call_start_frame, 2, wrenvm_prepare_startframe);
}

static int pending_client_id;

static void wrenvm_prepare_client(WrenVM *vm)
{
    wrenSetSlotDouble(vm, 1, (double)pending_client_id);
}

wrenvm_call_result_t WRENVM_CallClientConnect(int client_id)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    pending_client_id = client_id;
    return wrenvm_call_handle(wren_state.call_client_connect, 2, wrenvm_prepare_client);
}

wrenvm_call_result_t WRENVM_CallClientDisconnect(int client_id)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    pending_client_id = client_id;
    return wrenvm_call_handle(wren_state.call_client_disconnect, 2, wrenvm_prepare_client);
}

wrenvm_call_result_t WRENVM_CallOnSave(void)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    return wrenvm_call_handle(wren_state.call_on_save, 1, NULL);
}

wrenvm_call_result_t WRENVM_CallOnLoad(void)
{
    if (!wren_state.enabled)
        return WRENV_CALL_MISSING;

    return wrenvm_call_handle(wren_state.call_on_load, 1, NULL);
}

qboolean WRENVM_IsEnabled(void)
{
    return wren_state.enabled && wren_state.vm != NULL;
}

qboolean WRENVM_IsStrict(void)
{
    return wren_state.strict;
}

void WRENVM_NotifySkillChanged(int skill)
{
    wren_state.current_skill = skill;
}

void WRENVM_NotifyServerFlags(int flags)
{
    wren_state.server_flags = flags;
}

void WRENVM_SetDemoPlayback(qboolean playback)
{
    wren_state.demo_playback = playback;
}

void WRENVM_SpawnEntitiesFallback(void)
{
    if (!wren_state.current_entity_lump)
        return;

    ED_LoadFromFile(wren_state.current_entity_lump);
}

const char *WRENVM_GetCurrentMapName(void)
{
    return wren_state.current_map;
}
