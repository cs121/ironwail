#ifndef WREN_VM_H
#define WREN_VM_H

#include "q_stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wrenvm_hook {
    WRENV_HOOK_INIT,
    WRENV_HOOK_SPAWN_ENTITIES,
    WRENV_HOOK_START_FRAME,
    WRENV_HOOK_CLIENT_CONNECT,
    WRENV_HOOK_CLIENT_DISCONNECT,
    WRENV_HOOK_ON_SAVE,
    WRENV_HOOK_ON_LOAD
} wrenvm_hook_t;

typedef enum wrenvm_call_result {
    WRENV_CALL_OK,
    WRENV_CALL_MISSING,
    WRENV_CALL_FALLBACK,
    WRENV_CALL_ERROR
} wrenvm_call_result_t;

void WRENVM_InitSystem(void);
void WRENVM_ShutdownSystem(void);
void WRENVM_ResetForNewServer(const char *mapname, const char *entity_lump);

wrenvm_call_result_t WRENVM_CallInit(void);
wrenvm_call_result_t WRENVM_CallSpawnEntities(void);
wrenvm_call_result_t WRENVM_CallStartFrame(float dt);
wrenvm_call_result_t WRENVM_CallClientConnect(int client_id);
wrenvm_call_result_t WRENVM_CallClientDisconnect(int client_id);
wrenvm_call_result_t WRENVM_CallOnSave(void);
wrenvm_call_result_t WRENVM_CallOnLoad(void);

qboolean WRENVM_IsEnabled(void);
qboolean WRENVM_IsStrict(void);

void WRENVM_NotifySkillChanged(int skill);
void WRENVM_NotifyServerFlags(int flags);
void WRENVM_SetDemoPlayback(qboolean playback);

// Engine level helpers used by bindings
void WRENVM_SpawnEntitiesFallback(void);
const char *WRENVM_GetCurrentMapName(void);

#ifdef __cplusplus
}
#endif

#endif // WREN_VM_H
